/**
 * @file accumulator.cppm
 * @brief Combine streamed chunks into a complete ChatCompletion.
 */
export module moai:chat_accumulator;

import std;
import :chat_types;

export namespace moai {

    /**
     * @brief Merge ChatCompletionChunk objects in arrival order into one ChatCompletion.
     * @note Content and refusal text are concatenated per choice; tool call fragments are grouped by
     * their index and their arguments concatenated. Usage is taken from the chunk that reports it.
     * Not thread-safe; feed chunks from the stream callback.
     *
     * @code
     * moai::ChatCompletionAccumulator accumulator;
     * auto result = client.chat.completions.CreateStream(params, [&](auto const& chunk) {
     *     accumulator.Add(chunk);
     *     return true;
     * });
     * auto const& completion = accumulator.Completion();
     * @endcode
     */
    class ChatCompletionAccumulator {
    public:
        /**
         * @brief Create an empty accumulator.
         */
        ChatCompletionAccumulator() = default;

        /**
         * @brief Merge one chunk.
         * @param chunk Next chunk of the same stream.
         */
        auto Add(ChatCompletionChunk const& chunk) -> void;

        /**
         * @brief Return the completion accumulated so far.
         * @return Completion whose choices are ordered by index; `raw` stays empty.
         */
        [[nodiscard]] auto Completion() const noexcept -> ChatCompletion const& { return m_completion; }

        /**
         * @brief Move the accumulated completion out and reset the accumulator.
         */
        [[nodiscard]] auto Take() -> ChatCompletion;

    private:
        /**
         * @brief Find or insert the choice with the given index.
         */
        auto choiceAt(std::int64_t index) -> ChatCompletion::Choice&;

        ChatCompletion m_completion{};                                             ///< Accumulated result.
        std::map<std::pair<std::int64_t, std::int64_t>, std::size_t> m_tool_calls; ///< (choice, tool index) to position in tool_calls.
    };

} // namespace moai
