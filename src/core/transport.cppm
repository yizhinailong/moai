/**
 * @file transport.cppm
 * @brief Internal request execution shared by resource components; not re-exported by moai.
 */
export module moai:transport;

import std;
import mcr;
import :config;
import :error;
import :json;

namespace moai::detail {

    /**
     * @brief HTTP methods used by the implemented resources.
     */
    enum class HttpMethod : std::uint8_t {
        Get,    ///< GET without a body.
        Post,   ///< POST with an optional JSON body.
        Delete, ///< DELETE without a body.
    };

    /**
     * @brief Resource-level description of one API call.
     */
    struct ApiRequest {
        HttpMethod method{ HttpMethod::Get }; ///< HTTP method.
        std::string path{};                   ///< Path below the base URL, starting with '/'.
        QueryParameters query{};              ///< Resource query parameters.
        std::optional<Json> body{};           ///< JSON body; only valid for POST.
    };

    /**
     * @brief Successful (2xx) response before resource decoding.
     */
    struct RawResponse {
        long status_code{};       ///< HTTP status.
        mcr::Header headers{};    ///< Final response headers.
        std::string body{};       ///< Response body bytes.
        std::string request_id{}; ///< x-request-id header, if present.
    };

    /**
     * @brief Consumer for each JSON payload of an event stream.
     * @note Returning an Error stops the transfer and becomes the call's result. Exceptions propagate
     * to the caller of SendStream after the transfer stops.
     */
    using StreamEventHandler = std::function<Result<void>(Json&& payload)>;

    /**
     * @brief Immutable client state shared by components and in-flight requests.
     * @note Thread-safe: every request uses its own mcr::Session, and the connection pool is
     * internally synchronized.
     */
    class ClientCore {
    public:
        /**
         * @brief Restrict construction to Create() while permitting std::make_shared.
         */
        class Key {
            friend class ClientCore;
            Key() = default;
        };

        /**
         * @brief Store resolved options; use Create() instead.
         */
        ClientCore(Key, ClientOptions options, mcr::ConnectionPool pool);

        /**
         * @brief Resolve environment fallbacks and validate options.
         * @param options Caller options, copied into the client.
         * @return Shared state, InvalidArgument for a missing API key or invalid option, or
         * RequestSetup when the connection pool cannot be created.
         */
        [[nodiscard]] static auto Create(ClientOptions options) -> Result<std::shared_ptr<ClientCore const>>;

        /**
         * @brief Return the resolved options; api_key and base_url are always set.
         */
        [[nodiscard]] auto Options() const noexcept -> ClientOptions const& { return m_options; }

        /**
         * @brief Return the shared connection pool for coroutine requests.
         * @note Copies share the same connection cache; the returned reference remains valid while
         * the owning shared state is alive.
         */
        [[nodiscard]] auto Pool() const noexcept -> mcr::ConnectionPool const& { return m_pool; }

        /**
         * @brief Execute a request with retries and map non-2xx responses to errors.
         * @param request Resource request.
         * @param options Per-request overrides.
         * @return The 2xx response or the final error.
         */
        [[nodiscard]] auto Send(ApiRequest const& request, RequestOptions const& options) const -> Result<RawResponse>;

        /**
         * @brief Execute a streaming request and deliver each JSON event payload in order.
         * @param request Resource request; the body should already request streaming.
         * @param options Per-request overrides.
         * @param on_event Consumer invoked on the calling thread during the transfer.
         * @return Success after `data: [DONE]`, or the first error. A request is retried only
         * before any event has been delivered.
         */
        [[nodiscard]] auto SendStream(ApiRequest const& request, RequestOptions const& options, StreamEventHandler const& on_event) const -> Result<void>;

    private:
        ClientOptions m_options;    ///< Resolved options.
        mcr::ConnectionPool m_pool; ///< Shared connections and TLS sessions.
    };

    /**
     * @brief Execute a buffered request through mcr's coroutine runtime.
     * @param core Shared client state kept alive by the task.
     * @param request Resource request, owned by the task.
     * @param options Per-request overrides, owned by the task.
     * @return Task yielding the 2xx response or the final error. Retries use non-blocking
     * exponential backoff; cancellation and runtime shutdown stop the wait immediately.
     * @note The task may outlive the Client that created it; state is owned by the task.
     */
    [[nodiscard]] auto send_coro(
        std::shared_ptr<ClientCore const> core,
        ApiRequest request,
        RequestOptions options
    ) -> mcr::Task<Result<RawResponse>>;

    /**
     * @brief Execute a streaming request through mcr's coroutine runtime.
     * @param core Shared client state kept alive by the task.
     * @param request Resource request, owned by the task; the body should request streaming.
     * @param options Per-request overrides, owned by the task.
     * @param on_event Consumer invoked on the coroutine runtime's I/O thread.
     * @return Task yielding success after `data: [DONE]` or the first error.
     * @note Retried only before an event is delivered. The handler must not block the I/O thread.
     */
    [[nodiscard]] auto send_stream_coro(
        std::shared_ptr<ClientCore const> core,
        ApiRequest request,
        RequestOptions options,
        StreamEventHandler on_event
    ) -> mcr::Task<Result<void>>;

    /**
     * @brief Build an error for an HTTP response outside 2xx.
     * @param status_code HTTP status.
     * @param request_id x-request-id value.
     * @param body Response body, used for API error fields and diagnostics.
     * @return Error classified by status.
     */
    [[nodiscard]] auto status_error(long status_code, std::string request_id, std::string_view body) -> Error;

    /**
     * @brief Percent-encode one path segment, such as a resource ID.
     * @param segment Raw segment.
     * @return Segment with every byte outside the RFC 3986 unreserved set encoded.
     */
    [[nodiscard]] auto encode_path_segment(std::string_view segment) -> std::string;

    /**
     * @brief Reject empty resource IDs before they produce a different endpoint path.
     * @param id Resource ID.
     * @param name Parameter name for the diagnostic.
     * @return Success, or InvalidArgument.
     */
    [[nodiscard]] auto require_id(std::string_view id, std::string_view name) -> Result<void>;

    /**
     * @brief Parse a 2xx response body and decode it, converting decoder failures to Decode errors.
     * @param response Response to decode.
     * @param decode Callable receiving a JsonReader for the document root.
     * @return Decoded value or ErrorKind::Decode.
     */
    template <typename Decode>
    [[nodiscard]] auto decode_response(RawResponse const& response, Decode&& decode) -> Result<std::invoke_result_t<Decode&, JsonReader const&>> {
        auto const decode_error{ [&](std::string message) {
            return Error{
                .kind = ErrorKind::Decode,
                .message = std::move(message),
                .status_code = response.status_code,
                .request_id = response.request_id,
                .body = response.body.substr(0, 64 * 1024),
            };
        } };
        auto const document = Json::parse(response.body, nullptr, false);
        if (document.is_discarded()) {
            return std::unexpected{ decode_error("response body is not valid JSON") };
        }
        try {
            return decode(JsonReader{ document });
        } catch (DecodeFailure const& failure) {
            return std::unexpected{ decode_error(failure.what()) };
        } catch (Json::exception const& failure) {
            return std::unexpected{ decode_error(failure.what()) };
        }
    }

} // namespace moai::detail
