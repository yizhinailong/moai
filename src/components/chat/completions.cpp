/**
 * @file completions.cpp
 * @brief Request construction and response mapping for the Chat Completions resource.
 */
module moai;

import std;
import mcr;
import :chat_completions;
import :chat_types;
import :config;
import :error;
import :json;
import :pagination;
import :transport;

namespace moai {

    namespace {

        constexpr std::string_view COMPLETIONS_PATH{ "/chat/completions" }; ///< Resource root.

        /**
         * @brief Build the path of one stored completion.
         */
        auto completion_path(std::string_view completion_id) -> std::string {
            return std::format("{}/{}", COMPLETIONS_PATH, detail::encode_path_segment(completion_id));
        }

        /**
         * @brief Append an optional query parameter.
         */
        auto add_query(QueryParameters& query, std::string_view key, std::optional<std::string> const& value) -> void {
            if (value) {
                query.emplace_back(key, *value);
            }
        }

        /**
         * @brief Append an optional integer query parameter.
         */
        auto add_query(QueryParameters& query, std::string_view key, std::optional<std::int64_t> const& value) -> void {
            if (value) {
                query.emplace_back(key, std::to_string(*value));
            }
        }

        /**
         * @brief Send a request and decode its JSON response.
         */
        template <typename Decode>
        auto send_and_decode(detail::ClientCore const& core, detail::ApiRequest const& request, RequestOptions const& options, Decode&& decode)
            -> Result<std::invoke_result_t<Decode&, detail::JsonReader const&>> {
            auto response{ core.Send(request, options) };
            if (!response) {
                return std::unexpected{ std::move(response.error()) };
            }
            return detail::decode_response(*response, std::forward<Decode>(decode));
        }

        /**
         * @brief Build the create request body, optionally enabling streaming.
         */
        auto create_body(ChatCompletionCreateParams const& params, bool stream) -> Json {
            Json body = detail::encode_create_params(params);
            if (stream) {
                body["stream"] = true;
            }
            return body;
        }

        /**
         * @brief Wrap a chunk consumer with response decoding and cancellation semantics.
         */
        auto make_chunk_handler(ChatCompletionChunkCallback on_chunk) -> detail::StreamEventHandler {
            return [on_chunk = std::move(on_chunk)](Json&& payload) -> Result<void> {
                ChatCompletionChunk chunk;
                try {
                    chunk = detail::decode_chat_completion_chunk(detail::JsonReader{ payload });
                } catch (detail::DecodeFailure const& failure) {
                    return std::unexpected{ Error{ .kind = ErrorKind::Decode, .message = failure.what() } };
                } catch (Json::exception const& failure) {
                    return std::unexpected{ Error{ .kind = ErrorKind::Decode, .message = failure.what() } };
                }
                chunk.raw = std::move(payload);
                if (!on_chunk(chunk)) {
                    return std::unexpected{ Error{ .kind = ErrorKind::Cancelled, .message = "stream stopped by the chunk callback" } };
                }
                return {};
            };
        }

        /**
         * @brief Submit a task to mcr's global thread pool without blocking the caller.
         * @return A future for the task result, or RequestSetup when the pool is unavailable.
         */
        template <typename Fn>
        auto submit_task(Fn fn) -> Result<std::future<std::invoke_result_t<std::decay_t<Fn>>>> {
            auto* pool{ mcr::GlobalThreadPool::GetInstance() };
            if (pool == nullptr) {
                return std::unexpected{ Error{ .kind = ErrorKind::RequestSetup, .message = "asynchronous runtime has been cleaned up" } };
            }
            try {
                auto future{ pool->Submit(std::move(fn)) };
                if (!future) {
                    return std::unexpected{ Error{
                        .kind = ErrorKind::RequestSetup,
                        .message = future.error().message.empty() ? std::string{ "cannot submit asynchronous task" } : future.error().message,
                        .transport_code = future.error().code,
                    } };
                }
                return std::move(*future);
            } catch (std::exception const& failure) {
                return std::unexpected{ Error{ .kind = ErrorKind::RequestSetup, .message = failure.what() } };
            }
        }

        /**
         * @brief Create a completion in a coroutine frame that owns all request state.
         */
        auto create_coro(
            std::shared_ptr<detail::ClientCore const> core,
            ChatCompletionCreateParams params,
            RequestOptions options
        ) -> mcr::Task<Result<ChatCompletion>> {
            auto request{ detail::ApiRequest{
                .method = detail::HttpMethod::Post,
                .path = std::string{ COMPLETIONS_PATH },
                .body = create_body(params, false),
            } };
            auto response{ co_await detail::send_coro(std::move(core), std::move(request), std::move(options)) };
            if (!response) {
                co_return std::unexpected{ std::move(response.error()) };
            }
            co_return detail::decode_response(*response, detail::decode_chat_completion);
        }

        /**
         * @brief Stream a completion in a coroutine frame that owns all request state.
         */
        auto create_stream_coro(
            std::shared_ptr<detail::ClientCore const> core,
            ChatCompletionCreateParams params,
            ChatCompletionChunkCallback on_chunk,
            RequestOptions options
        ) -> mcr::Task<Result<void>> {
            if (!on_chunk) {
                co_return std::unexpected{ Error{ .kind = ErrorKind::InvalidArgument, .message = "on_chunk must not be empty" } };
            }
            auto request{ detail::ApiRequest{
                .method = detail::HttpMethod::Post,
                .path = std::string{ COMPLETIONS_PATH },
                .body = create_body(params, true),
            } };
            co_return co_await detail::send_stream_coro(
                std::move(core),
                std::move(request),
                std::move(options),
                make_chunk_handler(std::move(on_chunk))
            );
        }

    } // namespace

    auto ChatCompletions::Create(ChatCompletionCreateParams const& params, RequestOptions const& options) const -> Result<ChatCompletion> {
        return send_and_decode(
            *m_core,
            detail::ApiRequest{ .method = detail::HttpMethod::Post, .path = std::string{ COMPLETIONS_PATH }, .body = detail::encode_create_params(params) },
            options,
            detail::decode_chat_completion
        );
    }

    auto ChatCompletions::CreateStream(
        ChatCompletionCreateParams const& params,
        ChatCompletionChunkCallback const& on_chunk,
        RequestOptions const& options
    ) const -> Result<void> {
        if (!on_chunk) {
            return std::unexpected{ Error{ .kind = ErrorKind::InvalidArgument, .message = "on_chunk must not be empty" } };
        }
        return m_core->SendStream(
            detail::ApiRequest{ .method = detail::HttpMethod::Post, .path = std::string{ COMPLETIONS_PATH }, .body = create_body(params, true) },
            options,
            make_chunk_handler(on_chunk)
        );
    }

    auto ChatCompletions::CreateAsync(ChatCompletionCreateParams params, RequestOptions options) const
        -> Result<std::future<Result<ChatCompletion>>> {
        auto core{ m_core };
        return submit_task([core = std::move(core), params = std::move(params), options = std::move(options)]() mutable {
            auto request{ detail::ApiRequest{
                .method = detail::HttpMethod::Post,
                .path = std::string{ COMPLETIONS_PATH },
                .body = create_body(params, false),
            } };
            return send_and_decode(*core, request, options, detail::decode_chat_completion);
        });
    }

    auto ChatCompletions::CreateCoro(ChatCompletionCreateParams params, RequestOptions options) const
        -> mcr::Task<Result<ChatCompletion>> {
        return create_coro(m_core, std::move(params), std::move(options));
    }

    auto ChatCompletions::CreateStreamAsync(
        ChatCompletionCreateParams params,
        ChatCompletionChunkCallback on_chunk,
        RequestOptions options
    ) const -> Result<std::future<Result<void>>> {
        if (!on_chunk) {
            return std::unexpected{ Error{ .kind = ErrorKind::InvalidArgument, .message = "on_chunk must not be empty" } };
        }
        auto core{ m_core };
        return submit_task([core = std::move(core), params = std::move(params), on_chunk = std::move(on_chunk), options = std::move(options)]() mutable {
            auto request{ detail::ApiRequest{
                .method = detail::HttpMethod::Post,
                .path = std::string{ COMPLETIONS_PATH },
                .body = create_body(params, true),
            } };
            return core->SendStream(request, options, make_chunk_handler(std::move(on_chunk)));
        });
    }

    auto ChatCompletions::CreateStreamCoro(
        ChatCompletionCreateParams params,
        ChatCompletionChunkCallback on_chunk,
        RequestOptions options
    ) const -> mcr::Task<Result<void>> {
        return create_stream_coro(m_core, std::move(params), std::move(on_chunk), std::move(options));
    }

    auto ChatCompletions::Retrieve(std::string_view completion_id, RequestOptions const& options) const -> Result<ChatCompletion> {
        if (auto valid{ detail::require_id(completion_id, "completion_id") }; !valid) {
            return std::unexpected{ std::move(valid.error()) };
        }
        return send_and_decode(*m_core, detail::ApiRequest{ .method = detail::HttpMethod::Get, .path = completion_path(completion_id) }, options, detail::decode_chat_completion);
    }

    auto ChatCompletions::Update(
        std::string_view completion_id,
        std::optional<std::map<std::string, std::string>> const& metadata,
        RequestOptions const& options
    ) const -> Result<ChatCompletion> {
        if (auto valid{ detail::require_id(completion_id, "completion_id") }; !valid) {
            return std::unexpected{ std::move(valid.error()) };
        }
        Json body = Json::object();
        body["metadata"] = metadata ? Json(*metadata) : Json(nullptr);
        return send_and_decode(
            *m_core,
            detail::ApiRequest{ .method = detail::HttpMethod::Post, .path = completion_path(completion_id), .body = std::move(body) },
            options,
            detail::decode_chat_completion
        );
    }

    auto ChatCompletions::List(ChatCompletionListParams const& params, RequestOptions const& options) const -> Result<CursorPage<ChatCompletion>> {
        QueryParameters query;
        add_query(query, "after", params.after);
        add_query(query, "limit", params.limit);
        if (params.metadata) {
            for (auto const& [key, value] : *params.metadata) {
                query.emplace_back(std::format("metadata[{}]", key), value);
            }
        }
        add_query(query, "model", params.model);
        add_query(query, "order", params.order);
        return send_and_decode(
            *m_core,
            detail::ApiRequest{ .method = detail::HttpMethod::Get, .path = std::string{ COMPLETIONS_PATH }, .query = std::move(query) },
            options,
            [](detail::JsonReader const& reader) { return detail::decode_cursor_page(reader, detail::decode_chat_completion); }
        );
    }

    auto ChatCompletions::Delete(std::string_view completion_id, RequestOptions const& options) const -> Result<ChatCompletionDeleted> {
        if (auto valid{ detail::require_id(completion_id, "completion_id") }; !valid) {
            return std::unexpected{ std::move(valid.error()) };
        }
        return send_and_decode(
            *m_core,
            detail::ApiRequest{ .method = detail::HttpMethod::Delete, .path = completion_path(completion_id) },
            options,
            detail::decode_chat_completion_deleted
        );
    }

    auto ChatCompletionMessages::List(
        std::string_view completion_id,
        ChatCompletionMessageListParams const& params,
        RequestOptions const& options
    ) const -> Result<CursorPage<ChatCompletionStoreMessage>> {
        if (auto valid{ detail::require_id(completion_id, "completion_id") }; !valid) {
            return std::unexpected{ std::move(valid.error()) };
        }
        QueryParameters query;
        add_query(query, "after", params.after);
        add_query(query, "limit", params.limit);
        add_query(query, "order", params.order);
        return send_and_decode(
            *m_core,
            detail::ApiRequest{ .method = detail::HttpMethod::Get, .path = completion_path(completion_id) + "/messages", .query = std::move(query) },
            options,
            [](detail::JsonReader const& reader) { return detail::decode_cursor_page(reader, detail::decode_store_message); }
        );
    }

} // namespace moai
