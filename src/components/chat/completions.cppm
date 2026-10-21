/**
 * @file completions.cppm
 * @brief Chat Completions resource: create, stream, and manage stored completions.
 * @see https://developers.openai.com/api/reference/resources/chat/subresources/completions
 */
export module moai:chat_completions;

import std;
import mcr;
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
         * @brief Create a completion on mcr's global thread pool without blocking the caller.
         * @param params Request parameters, copied into the task.
         * @param options Per-request overrides, copied into the task. `stop_token` cancels the
         * transfer and any retry wait from another thread.
         * @return A future yielding the completion or an error, or an immediate error when the pool
         * has been cleaned up or the task cannot be submitted.
         * @note The future must be consumed with `get()`; discarding it does not stop the task.
         * The client state is kept alive by the task, so the Client may be destroyed first.
         */
        [[nodiscard]] auto CreateAsync(
            ChatCompletionCreateParams params,
            RequestOptions options = {}
        ) const -> Result<std::future<Result<ChatCompletion>>>;

        /**
         * @brief Create a lazy completion task driven by mcr's curl multi coroutine runtime.
         * @param params Request parameters, copied into the task.
         * @param options Per-request overrides, copied into the task. `stop_token`, `Task::Cancel()`,
         * and parent task cancellation all stop the transfer and the retry backoff.
         * @return Single-consumer task yielding the completion or an error.
         * @note The task and its result live independently of the Client and of the caller's
         * original parameter objects. Await it or pass it to `mcr::sync_wait`; the coroutine
         * runtime must be running and may be shut down with `mcr::Coro::Cleanup()`.
         */
        [[nodiscard]] auto CreateCoro(
            ChatCompletionCreateParams params,
            RequestOptions options = {}
        ) const -> mcr::Task<Result<ChatCompletion>>;

        /**
         * @brief Stream a completion on mcr's global thread pool.
         * @param params Request parameters, copied into the task; `"stream": true` is added.
         * @param on_chunk Consumer invoked on a pool worker thread for each chunk. Its exceptions
         * propagate through the future.
         * @param options Per-request overrides, copied into the task.
         * @return A future yielding success after the stream completes or the first error.
         * @note A request is retried only before the first chunk is delivered.
         */
        [[nodiscard]] auto CreateStreamAsync(
            ChatCompletionCreateParams params,
            ChatCompletionChunkCallback on_chunk,
            RequestOptions options = {}
        ) const -> Result<std::future<Result<void>>>;

        /**
         * @brief Stream a completion through mcr's curl multi coroutine runtime.
         * @param params Request parameters, copied into the task; `"stream": true` is added.
         * @param on_chunk Consumer invoked on the coroutine runtime's I/O thread for each chunk.
         * Returning false stops the stream with ErrorKind::Cancelled. The callback must not block;
         * the chunk reference is only valid during the call.
         * @param options Per-request overrides, copied into the task.
         * @return Single-consumer task yielding success after `[DONE]` or the first error.
         * @note Retried only before the first chunk is delivered.
         */
        [[nodiscard]] auto CreateStreamCoro(
            ChatCompletionCreateParams params,
            ChatCompletionChunkCallback on_chunk,
            RequestOptions options = {}
        ) const -> mcr::Task<Result<void>>;

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
