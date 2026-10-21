/**
 * @file conversation.cppm
 * @brief Message history helper for multi-turn Chat Completions.
 */
export module moai:chat_conversation;

import std;
import :chat_types;
import :error;
import :json;

export namespace moai {

    /**
     * @brief Ordered chat history that can be reused across requests.
     * @note The conversation stores request messages only and does not depend on a client or on
     * `ChatCompletionCreateParams`; copy `Messages()` into the parameters of the next call. It is
     * not thread-safe; synchronize access when several threads share one conversation.
     *
     * @code
     * moai::Conversation conversation;
     * conversation.Add(moai::system_message("Answer in one sentence."));
     * conversation.Add(moai::user_message("What is a C++ module?"));
     * auto completion = client->chat.completions.Create({
     *     .model = "gpt-5-mini",
     *     .messages = conversation.Messages(),
     * });
     * if (completion) {
     *     conversation.Add(*completion);
     * }
     * @endcode
     */
    class Conversation {
    public:
        /**
         * @brief Create an empty conversation.
         */
        Conversation() = default;

        /**
         * @brief Create a conversation with existing messages in order.
         * @param messages Initial history.
         */
        explicit Conversation(std::vector<ChatCompletionMessageParam> messages);

        /**
         * @brief Append one request message.
         * @param message Message to append; the conversation takes ownership.
         * @note When a message limit is set, the oldest non-instruction turn is trimmed afterwards.
         */
        auto Add(ChatCompletionMessageParam message) -> void;

        /**
         * @brief Append the assistant message of a completion.
         * @param completion Response of a previous request.
         * @param choice_index Choice to append, normally zero.
         * @return Success, or InvalidArgument when `choice_index` is out of range.
         */
        [[nodiscard]] auto Add(ChatCompletion const& completion, std::size_t choice_index = 0) -> Result<void>;

        /**
         * @brief Append the result of a tool call.
         * @param tool_call_id ID of the answered call.
         * @param content Tool output; use a JSON string for structured results.
         */
        auto AddToolResult(std::string tool_call_id, std::string content) -> void;

        /**
         * @brief Remove every message.
         */
        auto Clear() -> void;

        /**
         * @brief Limit the number of stored messages.
         * @param limit Maximum message count including developer and system instructions, or
         * nullopt for no limit. Trimming never removes leading developer or system messages, so a
         * conversation may exceed the limit when those messages alone do.
         */
        auto SetMessageLimit(std::optional<std::size_t> limit) -> void;

        /**
         * @brief Return the configured message limit, if any.
         */
        [[nodiscard]] auto MessageLimit() const noexcept -> std::optional<std::size_t> { return m_limit; }

        /**
         * @brief Return the history in transmission order.
         * @note The reference remains valid until the next modification.
         */
        [[nodiscard]] auto Messages() const noexcept -> std::vector<ChatCompletionMessageParam> const& { return m_messages; }

        /**
         * @brief Return the number of stored messages.
         */
        [[nodiscard]] auto Size() const noexcept -> std::size_t { return m_messages.size(); }

        /**
         * @brief Return whether no message is stored.
         */
        [[nodiscard]] auto Empty() const noexcept -> bool { return m_messages.empty(); }

        /**
         * @brief Export the history as a JSON array of message objects.
         * @return Array in transmission order; it can be decoded again with FromJson.
         */
        [[nodiscard]] auto ToJson() const -> Json;

        /**
         * @brief Import a history exported by ToJson.
         * @param json JSON array of message objects.
         * @return The decoded conversation, or ErrorKind::Decode when the structure is invalid.
         */
        [[nodiscard]] static auto FromJson(Json const& json) -> Result<Conversation>;

    private:
        /**
         * @brief Apply the message limit.
         */
        auto trim() -> void;

        /**
         * @brief Remove the oldest turn, keeping developer and system instructions.
         * @note An assistant message that requested tool calls is removed together with the tool
         * messages that answer it, so trimming never leaves orphaned tool results.
         */
        auto removeOldestTurn() -> void;

        std::vector<ChatCompletionMessageParam> m_messages{}; ///< Messages in transmission order.
        std::optional<std::size_t> m_limit{};                 ///< Optional maximum message count.
    };

} // namespace moai
