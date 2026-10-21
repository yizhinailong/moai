/**
 * @file helpers.cppm
 * @brief Helpers for structured outputs and tool call arguments.
 */
export module moai:chat_helpers;

import std;
import :chat_types;
import :error;
import :json;

export namespace moai {

    /**
     * @brief Reject a choice whose finish reason prevents using its output as data.
     * @param choice Choice of a ChatCompletion.
     * @return Success for "stop", "tool_calls", and other values; ErrorKind::LengthFinishReason
     * when the output was truncated at the token limit; ErrorKind::ContentFilterFinishReason when
     * the content filter blocked it.
     * @note Check this before parsing tool arguments or structured output; truncated output is
     * usually incomplete JSON.
     */
    [[nodiscard]] auto check_finish_reason(ChatCompletion::Choice const& choice) -> Result<void>;

    /**
     * @brief Parse the structured output of a completion into JSON.
     * @param completion Response of a request made with a JSON response format.
     * @param choice_index Choice to read, normally zero.
     * @return Parsed content, InvalidArgument for an out-of-range index, the finish-reason error
     * from check_finish_reason, or ErrorKind::Decode when the message has no content or the
     * content is not valid JSON.
     * @note Structured Outputs usually guarantee valid JSON, but JSON mode does not; treat a
     * Decode error as a model failure and retry or fall back.
     */
    [[nodiscard]] auto parse_structured_output(ChatCompletion const& completion, std::size_t choice_index = 0) -> Result<Json>;

    /**
     * @brief Parse the JSON arguments of a function tool call.
     * @param call Function tool call requested by the model.
     * @return Parsed arguments, or ErrorKind::Decode when they are not valid JSON.
     * @note Custom tool calls carry free-form input instead and are not supported by this helper.
     */
    [[nodiscard]] auto parse_tool_arguments(ChatCompletionMessageFunctionToolCall const& call) -> Result<Json>;

} // namespace moai
