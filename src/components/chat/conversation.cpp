/**
 * @file conversation.cpp
 * @brief Message history maintenance and JSON import/export.
 */
module moai;

import std;
import :chat_conversation;
import :chat_types;
import :error;
import :json;

namespace moai {

    namespace {

        /**
         * @brief Check whether a message carries standing instructions.
         */
        auto is_instruction(ChatCompletionMessageParam const& message) -> bool {
            return std::holds_alternative<ChatCompletionDeveloperMessageParam>(message) || std::holds_alternative<ChatCompletionSystemMessageParam>(message);
        }

        /**
         * @brief Return the ID of a tool call of either kind.
         */
        auto tool_call_id(ChatCompletionMessageToolCall const& call) -> std::string const& {
            return std::visit([](auto const& value) -> std::string const& { return value.id; }, call);
        }

    } // namespace

    Conversation::Conversation(std::vector<ChatCompletionMessageParam> messages) : m_messages{ std::move(messages) } {
        trim();
    }

    auto Conversation::Add(ChatCompletionMessageParam message) -> void {
        m_messages.push_back(std::move(message));
        trim();
    }

    auto Conversation::Add(ChatCompletion const& completion, std::size_t choice_index) -> Result<void> {
        if (choice_index >= completion.choices.size()) {
            return std::unexpected{ Error{
                .kind = ErrorKind::InvalidArgument,
                .message = std::format("choice index {} is out of range: the completion has {} choices", choice_index, completion.choices.size()),
            } };
        }
        Add(assistant_message(completion.choices[choice_index].message));
        return {};
    }

    auto Conversation::AddToolResult(std::string tool_call_id, std::string content) -> void {
        Add(tool_message(std::move(tool_call_id), std::move(content)));
    }

    auto Conversation::Clear() -> void {
        m_messages.clear();
    }

    auto Conversation::SetMessageLimit(std::optional<std::size_t> limit) -> void {
        m_limit = limit;
        trim();
    }

    auto Conversation::trim() -> void {
        if (!m_limit) {
            return;
        }
        while (m_messages.size() > *m_limit) {
            auto const before{ m_messages.size() };
            removeOldestTurn();
            if (m_messages.size() == before) {
                break; // Only developer and system instructions remain.
            }
        }
    }

    auto Conversation::removeOldestTurn() -> void {
        auto const first{ std::ranges::find_if(m_messages, [](ChatCompletionMessageParam const& message) { return !is_instruction(message); }) };
        if (first == m_messages.end()) {
            return;
        }
        std::set<std::string> answered;
        if (auto const* assistant{ std::get_if<ChatCompletionAssistantMessageParam>(&*first) }) {
            for (auto const& call : assistant->tool_calls) {
                answered.insert(tool_call_id(call));
            }
        }
        auto next{ m_messages.erase(first) };
        while (!answered.empty() && next != m_messages.end()) {
            auto const* tool{ std::get_if<ChatCompletionToolMessageParam>(&*next) };
            if (tool == nullptr || !answered.contains(tool->tool_call_id)) {
                break;
            }
            next = m_messages.erase(next);
        }
    }

    auto Conversation::ToJson() const -> Json {
        Json messages = Json::array();
        for (auto const& message : m_messages) {
            messages.push_back(detail::encode_message(message));
        }
        return messages;
    }

    auto Conversation::FromJson(Json const& json) -> Result<Conversation> {
        if (!json.is_array()) {
            return std::unexpected{ Error{ .kind = ErrorKind::Decode, .message = std::format("<root>: expected array of messages, got {}", json.type_name()) } };
        }
        try {
            std::vector<ChatCompletionMessageParam> messages;
            messages.reserve(json.size());
            for (std::size_t index{}; index < json.size(); ++index) {
                messages.push_back(detail::decode_message_param(detail::JsonReader{ json[index], std::format("[{}]", index) }));
            }
            return Conversation{ std::move(messages) };
        } catch (detail::DecodeFailure const& failure) {
            return std::unexpected{ Error{ .kind = ErrorKind::Decode, .message = failure.what() } };
        } catch (Json::exception const& failure) {
            return std::unexpected{ Error{ .kind = ErrorKind::Decode, .message = failure.what() } };
        }
    }

} // namespace moai
