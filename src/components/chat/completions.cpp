/**
 * @file completions.cpp
 * @brief Request construction and response mapping for the Chat Completions resource.
 */
module moai;

import std;
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
        Json body = detail::encode_create_params(params);
        body["stream"] = true;
        detail::StreamEventHandler const handler{ [&on_chunk](Json&& payload) -> Result<void> {
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
        } };
        return m_core->SendStream(
            detail::ApiRequest{ .method = detail::HttpMethod::Post, .path = std::string{ COMPLETIONS_PATH }, .body = std::move(body) },
            options,
            handler
        );
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
