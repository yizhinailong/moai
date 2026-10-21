/**
 * @file completions.cppm
 * @brief Chat Completions resource: create, stream, and manage stored completions.
 * @see https://developers.openai.com/api/reference/resources/chat/subresources/completions
 */
export module moai:chat_completions;

import std;
import :chat_types;
import :config;
import :error;
import :pagination;
import :transport;

export namespace moai {

    /**
     * @brief Consumer of streamed chunks; return false to stop the stream.
     * @note Invoked on the thread running the transfer. The chunk is only valid during the call.
     */
    using ChatCompletionChunkCallback = std::function<bool(ChatCompletionChunk const&)>;

    /**
     * @brief Messages of stored completions (`/chat/completions/{id}/messages`).
     * @note Obtain through `client.chat.completions.messages`; shares the client's state.
     */
    class ChatCompletionMessages {
    public:
        /**
         * @brief Bind to shared client state; created by Client.
         */
        explicit ChatCompletionMessages(std::shared_ptr<detail::ClientCore const> core) : m_core{ std::move(core) } {}

        /**
         * @brief List the messages of a stored completion.
         * @param completion_id Completion created with store=true.
         * @param params Pagination parameters.
         * @param options Per-request overrides.
         * @return One page of messages, or an error.
         */
        [[nodiscard]] auto List(
            std::string_view completion_id,
            ChatCompletionMessageListParams const& params = {},
            RequestOptions const& options = {}
        ) const -> Result<CursorPage<ChatCompletionStoreMessage>>;

    private:
        std::shared_ptr<detail::ClientCore const> m_core; ///< Shared client state.
    };

    /**
     * @brief Chat Completions resource (`/chat/completions`).
     * @note Thread-safe; calls may run concurrently from several threads. Every call blocks until
     * the response is complete or an error occurs.
     */
    class ChatCompletions {
    public:
        /**
         * @brief Bind to shared client state; created by Client.
         */
        explicit ChatCompletions(std::shared_ptr<detail::ClientCore const> core) : m_core{ core }, messages{ std::move(core) } {}

        /**
         * @brief Create a completion and wait for the full response.
         * @param params Request parameters.
         * @param options Per-request overrides.
         * @return The completion, or an error.
         */
        [[nodiscard]] auto Create(ChatCompletionCreateParams const& params, RequestOptions const& options = {}) const -> Result<ChatCompletion>;

        /**
         * @brief Create a completion and deliver it as server-sent chunks.
         * @param params Request parameters; `"stream": true` is added automatically.
         * @param on_chunk Consumer invoked for each chunk on the calling thread; returning false stops
         * the stream with ErrorKind::Cancelled. Exceptions it throws propagate after the transfer stops.
         * @param options Per-request overrides.
         * @return Success after the stream completes, or the first error. The request is retried only
         * before the first chunk is delivered.
         */
        [[nodiscard]] auto CreateStream(
            ChatCompletionCreateParams const& params,
            ChatCompletionChunkCallback const& on_chunk,
            RequestOptions const& options = {}
        ) const -> Result<void>;

        /**
         * @brief Retrieve a completion created with store=true.
         * @param completion_id Completion ID.
         * @param options Per-request overrides.
         */
        [[nodiscard]] auto Retrieve(std::string_view completion_id, RequestOptions const& options = {}) const -> Result<ChatCompletion>;

        /**
         * @brief Replace the metadata of a stored completion.
         * @param completion_id Completion ID.
         * @param metadata New metadata; nullopt sends null to clear it.
         * @param options Per-request overrides.
         * @return The updated completion.
         */
        [[nodiscard]] auto Update(
            std::string_view completion_id,
            std::optional<std::map<std::string, std::string>> const& metadata,
            RequestOptions const& options = {}
        ) const -> Result<ChatCompletion>;

        /**
         * @brief List stored completions.
         * @param params Filters and pagination.
         * @param options Per-request overrides.
         */
        [[nodiscard]] auto List(ChatCompletionListParams const& params = {}, RequestOptions const& options = {}) const -> Result<CursorPage<ChatCompletion>>;

        /**
         * @brief Delete a stored completion.
         * @param completion_id Completion ID.
         * @param options Per-request overrides.
         */
        [[nodiscard]] auto Delete(std::string_view completion_id, RequestOptions const& options = {}) const -> Result<ChatCompletionDeleted>;

    private:
        std::shared_ptr<detail::ClientCore const> m_core; ///< Shared client state.

    public:
        ChatCompletionMessages messages; ///< Messages of stored completions.
    };

    /**
     * @brief Chat resource group (`client.chat`).
     */
    class Chat {
    public:
        /**
         * @brief Bind to shared client state; created by Client.
         */
        explicit Chat(std::shared_ptr<detail::ClientCore const> core) : completions{ std::move(core) } {}

        ChatCompletions completions; ///< Chat Completions resource.
    };

} // namespace moai
