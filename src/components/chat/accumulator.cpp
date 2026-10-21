/**
 * @file accumulator.cpp
 * @brief Chunk merging for ChatCompletionAccumulator.
 */
module moai;

import std;
import :chat_accumulator;
import :chat_types;

namespace moai {

    auto ChatCompletionAccumulator::Add(ChatCompletionChunk const& chunk) -> void {
        if (m_completion.id.empty()) {
            m_completion.id = chunk.id;
        }
        if (m_completion.created == 0) {
            m_completion.created = chunk.created;
        }
        if (m_completion.model.empty()) {
            m_completion.model = chunk.model;
        }
        if (chunk.service_tier) {
            m_completion.service_tier = chunk.service_tier;
        }
        if (chunk.system_fingerprint) {
            m_completion.system_fingerprint = chunk.system_fingerprint;
        }
        if (chunk.usage) {
            m_completion.usage = chunk.usage;
        }

        for (auto const& update : chunk.choices) {
            auto& choice{ choiceAt(update.index) };
            auto& message{ choice.message };
            auto const& delta{ update.delta };
            if (delta.role) {
                message.role = *delta.role;
            }
            if (delta.content) {
                message.content = message.content.value_or(std::string{}) + *delta.content;
            }
            if (delta.refusal) {
                message.refusal = message.refusal.value_or(std::string{}) + *delta.refusal;
            }
            for (auto const& fragment : delta.tool_calls) {
                auto const key{ std::pair{ update.index, fragment.index } };
                auto found{ m_tool_calls.find(key) };
                if (found == m_tool_calls.end()) {
                    found = m_tool_calls.emplace(key, message.tool_calls.size()).first;
                    message.tool_calls.emplace_back(ChatCompletionMessageFunctionToolCall{});
                }
                auto* call{ std::get_if<ChatCompletionMessageFunctionToolCall>(&message.tool_calls[found->second]) };
                if (call == nullptr) {
                    continue;
                }
                if (fragment.id) {
                    call->id = *fragment.id;
                }
                if (fragment.function) {
                    if (fragment.function->name) {
                        call->function.name += *fragment.function->name;
                    }
                    if (fragment.function->arguments) {
                        call->function.arguments += *fragment.function->arguments;
                    }
                }
            }
            if (update.finish_reason) {
                choice.finish_reason = *update.finish_reason;
            }
            if (update.logprobs) {
                if (!choice.logprobs) {
                    choice.logprobs = ChatCompletionChoiceLogprobs{};
                }
                auto const append{ [](auto& target, auto const& source) {
                    if (source) {
                        if (!target) {
                            target.emplace();
                        }
                        target->insert(target->end(), source->begin(), source->end());
                    }
                } };
                append(choice.logprobs->content, update.logprobs->content);
                append(choice.logprobs->refusal, update.logprobs->refusal);
            }
        }
    }

    auto ChatCompletionAccumulator::Take() -> ChatCompletion {
        m_tool_calls.clear();
        return std::exchange(m_completion, ChatCompletion{});
    }

    auto ChatCompletionAccumulator::choiceAt(std::int64_t index) -> ChatCompletion::Choice& {
        auto position{ std::ranges::lower_bound(m_completion.choices, index, {}, &ChatCompletion::Choice::index) };
        if (position == m_completion.choices.end() || position->index != index) {
            position = m_completion.choices.insert(position, ChatCompletion::Choice{ .index = index });
        }
        return *position;
    }

} // namespace moai
