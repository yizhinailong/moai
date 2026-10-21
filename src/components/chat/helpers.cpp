/**
 * @file helpers.cpp
 * @brief Implementation of structured output and tool argument helpers.
 */
module moai;

import std;
import :chat_helpers;
import :chat_types;
import :error;
import :json;

namespace moai {

    auto check_finish_reason(ChatCompletion::Choice const& choice) -> Result<void> {
        if (choice.finish_reason == "length") {
            return std::unexpected{ Error{
                .kind = ErrorKind::LengthFinishReason,
                .message = "the model stopped because it reached the output token limit",
            } };
        }
        if (choice.finish_reason == "content_filter") {
            return std::unexpected{ Error{
                .kind = ErrorKind::ContentFilterFinishReason,
                .message = "the model stopped because the content filter blocked its output",
            } };
        }
        return {};
    }

    auto parse_structured_output(ChatCompletion const& completion, std::size_t choice_index) -> Result<Json> {
        if (choice_index >= completion.choices.size()) {
            return std::unexpected{ Error{
                .kind = ErrorKind::InvalidArgument,
                .message = std::format("choice index {} is out of range: the completion has {} choices", choice_index, completion.choices.size()),
            } };
        }
        auto const& choice{ completion.choices[choice_index] };
        if (auto const reason{ check_finish_reason(choice) }; !reason) {
            return std::unexpected{ reason.error() };
        }
        if (!choice.message.content) {
            return std::unexpected{ Error{ .kind = ErrorKind::Decode, .message = "the assistant message has no content to parse" } };
        }
        auto parsed = Json::parse(*choice.message.content, nullptr, false);
        if (parsed.is_discarded()) {
            return std::unexpected{ Error{
                .kind = ErrorKind::Decode,
                .message = "the assistant message content is not valid JSON",
                .body = choice.message.content->substr(0, 64 * 1024),
            } };
        }
        return parsed;
    }

    auto parse_tool_arguments(ChatCompletionMessageFunctionToolCall const& call) -> Result<Json> {
        auto parsed = Json::parse(call.function.arguments, nullptr, false);
        if (parsed.is_discarded()) {
            return std::unexpected{ Error{
                .kind = ErrorKind::Decode,
                .message = std::format("arguments of function '{}' are not valid JSON", call.function.name),
                .body = call.function.arguments.substr(0, 64 * 1024),
            } };
        }
        return parsed;
    }

} // namespace moai
