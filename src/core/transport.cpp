/**
 * @file transport.cpp
 * @brief mcr request execution, retry policy, error mapping, and SSE stream handling.
 */
module moai;

import std;
import mcr;
import :config;
import :error;
import :json;
import :transport;
import :version;

namespace moai::detail {

    namespace {

        constexpr std::size_t MAX_ERROR_BODY{ 64 * 1024 };             ///< Bytes of an error body kept in Error::body.
        constexpr std::size_t MAX_BUFFERED_STREAM_BODY{ 1024 * 1024 }; ///< Bytes of a non-SSE stream reply buffered for errors.
        constexpr double INITIAL_RETRY_DELAY{ 0.5 };                   ///< Seconds before the first retry.
        constexpr double MAX_RETRY_DELAY{ 8.0 };                       ///< Upper bound of the exponential backoff.
        constexpr double MAX_RETRY_AFTER{ 60.0 };                      ///< Largest server-requested delay that is honoured.

        /**
         * @brief Read an environment variable, treating empty values as unset.
         * @param name Variable name.
         * @return Value, or nullopt.
         */
        auto environment(char const* name) -> std::optional<std::string> {
#if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
            char const* value{ std::getenv(name) };
#if defined(__clang__)
    #pragma clang diagnostic pop
#endif
            if (value == nullptr || *value == '\0') {
                return std::nullopt;
            }
            return std::string{ value };
        }

        /**
         * @brief Compare ASCII text case-insensitively.
         */
        auto iequals(std::string_view left, std::string_view right) -> bool {
            return std::ranges::equal(left, right, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            });
        }

        /**
         * @brief Remove leading and trailing ASCII whitespace.
         */
        auto trim(std::string_view text) -> std::string_view {
            auto const first{ text.find_first_not_of(" \t\r\n") };
            if (first == std::string_view::npos) {
                return {};
            }
            auto const last{ text.find_last_not_of(" \t\r\n") };
            return text.substr(first, last - first + 1);
        }

        /**
         * @brief Look up a header value.
         */
        auto header_value(mcr::Header const& headers, std::string_view name) -> std::optional<std::string_view> {
            auto const found{ headers.find(name) };
            if (found == headers.end()) {
                return std::nullopt;
            }
            return std::string_view{ found->second };
        }

        /**
         * @brief Build a RequestSetup error from an mcr operation error.
         */
        auto setup_error(mcr::Error const& error) -> Error {
            return Error{
                .kind = ErrorKind::RequestSetup,
                .message = error.message.empty() ? std::string{ "request setup failed" } : error.message,
                .transport_code = error.code,
            };
        }

        /**
         * @brief Classify a transport error stored in mcr::Response::error.
         * @param error Transport error.
         * @param cancelled Whether moai requested cancellation.
         */
        auto transport_error(mcr::Error const& error, bool cancelled) -> Error {
            auto kind{ ErrorKind::Connection };
            if (cancelled) {
                kind = ErrorKind::Cancelled;
            } else if (error.code == mcr::ErrorCode::OPERATION_TIMEDOUT) {
                kind = ErrorKind::Timeout;
            }
            return Error{
                .kind = kind,
                .message = cancelled ? std::string{ "request cancelled" } : error.message.empty() ? std::string{ "transport failed" } :
                                                                                                    error.message,
                .transport_code = error.code,
            };
        }

        /**
         * @brief Build the cancellation error returned before or between attempts.
         */
        auto cancelled_error() -> Error {
            return Error{ .kind = ErrorKind::Cancelled, .message = "request cancelled" };
        }

        /**
         * @brief Copy API error fields from a decoded error document.
         * @param error Destination.
         * @param document Response or stream payload.
         */
        auto fill_api_error(Error& error, Json const& document) -> void {
            if (!document.is_object()) {
                return;
            }
            auto const text{ [](Json const& value) -> std::optional<std::string> {
                if (value.is_null()) {
                    return std::nullopt;
                }
                return value.is_string() ? value.get<std::string>() : value.dump();
            } };
            auto const found{ document.find("error") };
            if (found != document.end() && found->is_object()) {
                if (auto const message{ found->find("message") }; message != found->end() && !message->is_null()) {
                    error.message = text(*message).value_or(std::string{});
                }
                for (auto const& [key, target] : { std::pair{ "type", &error.type }, std::pair{ "code", &error.code }, std::pair{ "param", &error.param } }) {
                    if (auto const member{ found->find(key) }; member != found->end()) {
                        *target = text(*member);
                    }
                }
                return;
            }
            if (found != document.end() && found->is_string()) {
                error.message = found->get<std::string>();
                return;
            }
            if (auto const detail{ document.find("detail") }; detail != document.end() && !detail->is_null()) {
                error.message = text(*detail).value_or(std::string{});
            }
        }

        /**
         * @brief Decide whether a non-2xx response may be retried.
         * @note x-should-retry overrides the default status list.
         */
        auto should_retry(long status_code, mcr::Header const& headers) -> bool {
            if (auto const value{ header_value(headers, "x-should-retry") }) {
                if (iequals(trim(*value), "true")) {
                    return true;
                }
                if (iequals(trim(*value), "false")) {
                    return false;
                }
            }
            return status_code == 408 || status_code == 409 || status_code == 429 || status_code >= 500;
        }

        /**
         * @brief Parse a non-negative decimal header value.
         */
        auto parse_seconds(std::string_view text) -> std::optional<double> {
            text = trim(text);
            double value{};
            auto const [end, error]{ std::from_chars(text.data(), text.data() + text.size(), value) };
            if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
                return std::nullopt;
            }
            return value;
        }

        /**
         * @brief Compute the wait before the next attempt.
         * @param retry Zero-based number of retries already performed.
         * @param headers Headers of the failed response, or null after transport errors.
         * @return Server-requested delay when reasonable, otherwise jittered exponential backoff.
         */
        auto retry_delay(int retry, mcr::Header const* headers) -> std::chrono::duration<double> {
            if (headers != nullptr) {
                std::optional<double> requested;
                if (auto const milliseconds{ header_value(*headers, "retry-after-ms") }) {
                    if (auto const value{ parse_seconds(*milliseconds) }) {
                        requested = *value / 1000.0;
                    }
                }
                if (!requested) {
                    if (auto const seconds{ header_value(*headers, "retry-after") }) {
                        requested = parse_seconds(*seconds);
                    }
                }
                if (requested && *requested > 0.0 && *requested <= MAX_RETRY_AFTER) {
                    return std::chrono::duration<double>{ *requested };
                }
            }
            thread_local std::mt19937 generator{ std::random_device{}() };
            std::uniform_real_distribution<double> distribution{ 0.0, 1.0 };
            auto const backoff{ std::min(INITIAL_RETRY_DELAY * std::pow(2.0, retry), MAX_RETRY_DELAY) };
            return std::chrono::duration<double>{ backoff * (1.0 - 0.25 * distribution(generator)) };
        }

        /**
         * @brief Sleep until the delay elapses or cancellation is requested.
         * @return False when cancelled.
         */
        auto wait_for_retry(std::chrono::duration<double> delay, std::stop_token const& token) -> bool {
            std::mutex mutex;
            std::condition_variable_any ready;
            std::unique_lock lock{ mutex };
            (void)ready.wait_for(lock, token, std::chrono::duration_cast<std::chrono::milliseconds>(delay), [] { return false; });
            return !token.stop_requested();
        }

        /**
         * @brief Mutable state of one streaming attempt, owned by the attempt's stack frame.
         */
        struct StreamState {
            StreamEventHandler const* handler{}; ///< Consumer of decoded payloads.
            long status_code{};                  ///< Status of the latest header block.
            bool event_stream{};                 ///< Whether the latest header block declared text/event-stream.
            std::string content_type;            ///< Content-Type of the latest header block.
            bool done{};                         ///< Whether `data: [DONE]` was received.
            std::size_t delivered{};             ///< Events passed to the handler.
            std::string body;                    ///< Buffered body when the reply is not an event stream.
            std::optional<Error> failure;        ///< Error that stopped the transfer from inside a callback.
            mcr::ServerSentEventParser parser;   ///< Parser for this attempt's bytes.

            /**
             * @brief Track the status line and Content-Type of each header block.
             * @param line One raw header line including its terminator.
             */
            auto OnHeader(std::string_view line) -> void {
                if (line.starts_with("HTTP/")) {
                    status_code = 0;
                    event_stream = false;
                    content_type.clear();
                    auto const space{ line.find(' ') };
                    if (space != std::string_view::npos) {
                        auto const digits{ line.substr(space + 1, 3) };
                        long code{};
                        if (std::from_chars(digits.data(), digits.data() + digits.size(), code).ec == std::errc{}) {
                            status_code = code;
                        }
                    }
                    return;
                }
                auto const colon{ line.find(':') };
                if (colon != std::string_view::npos && iequals(trim(line.substr(0, colon)), "content-type")) {
                    content_type = trim(line.substr(colon + 1));
                    std::string lowered{ content_type };
                    std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    event_stream = lowered.starts_with("text/event-stream");
                }
            }

            /**
             * @brief Feed body bytes to the SSE parser, or buffer them when the reply is not a stream.
             * @return False to abort the transfer.
             */
            auto OnBody(std::string_view bytes) -> bool {
                if (status_code < 200 || status_code >= 300 || !event_stream) {
                    auto const room{ MAX_BUFFERED_STREAM_BODY - std::min(body.size(), MAX_BUFFERED_STREAM_BODY) };
                    body.append(bytes.substr(0, room));
                    return true;
                }
                return parser.Parse(bytes, [this](mcr::ServerSentEvent&& event) { return onEvent(std::move(event)); });
            }

        private:
            /**
             * @brief Decode one event and pass it on.
             */
            auto onEvent(mcr::ServerSentEvent&& event) -> bool {
                if (done) {
                    return true;
                }
                if (event.data == "[DONE]") {
                    done = true;
                    return true;
                }
                auto payload = Json::parse(event.data, nullptr, false);
                if (payload.is_discarded()) {
                    failure = Error{ .kind = ErrorKind::Decode, .message = "stream event is not valid JSON", .body = event.data.substr(0, MAX_ERROR_BODY) };
                    return false;
                }
                if (payload.is_object() && payload.contains("error") && !payload["error"].is_null()) {
                    Error error{ .kind = ErrorKind::Stream, .message = "stream reported an error", .body = event.data.substr(0, MAX_ERROR_BODY) };
                    fill_api_error(error, payload);
                    failure = std::move(error);
                    return false;
                }
                ++delivered;
                if (auto handled{ (*handler)(std::move(payload)) }; !handled) {
                    failure = std::move(handled.error());
                    return false;
                }
                return true;
            }
        };

        /**
         * @brief Outcome of a single attempt.
         */
        struct Attempt {
            Result<RawResponse> result{};         ///< Response or error of this attempt.
            bool retryable{};                     ///< Whether another attempt is permitted.
            std::optional<mcr::Header> headers{}; ///< Response headers used for Retry-After.
        };

        /**
         * @brief Apply an mcr setter result.
         */
        auto check(mcr::Result<void> const& status) -> Result<void> {
            if (!status) {
                return std::unexpected{ setup_error(status.error()) };
            }
            return {};
        }

    } // namespace

    ClientCore::ClientCore(Key, ClientOptions options, mcr::ConnectionPool pool) : m_options{ std::move(options) }, m_pool{ std::move(pool) } {}

    auto ClientCore::Create(ClientOptions options) -> Result<std::shared_ptr<ClientCore const>> {
        if (!options.api_key) {
            options.api_key = environment("OPENAI_API_KEY");
        }
        if (!options.api_key || options.api_key->empty()) {
            return std::unexpected{ Error{ .kind = ErrorKind::InvalidArgument, .message = "missing API key: set ClientOptions::api_key or OPENAI_API_KEY" } };
        }
        if (!options.base_url) {
            options.base_url = environment("OPENAI_BASE_URL").value_or(std::string{ DEFAULT_BASE_URL });
        }
        while (options.base_url->ends_with('/')) {
            options.base_url->pop_back();
        }
        if (options.base_url->empty()) {
            return std::unexpected{ Error{ .kind = ErrorKind::InvalidArgument, .message = "base_url must not be empty" } };
        }
        if (!options.organization) {
            options.organization = environment("OPENAI_ORG_ID");
        }
        if (!options.project) {
            options.project = environment("OPENAI_PROJECT_ID");
        }
        if (options.max_retries < 0) {
            return std::unexpected{ Error{ .kind = ErrorKind::InvalidArgument, .message = "max_retries must not be negative" } };
        }
        auto pool = mcr::ConnectionPool::Create();
        if (!pool) {
            return std::unexpected{ setup_error(pool.error()) };
        }
        return std::make_shared<ClientCore const>(Key{}, std::move(options), std::move(*pool));
    }

    namespace {

        /**
         * @brief Serialize the request body, merging RequestOptions::extra_body.
         * @return Body text, nullopt for requests without a body, or InvalidArgument.
         */
        auto serialize_body(ApiRequest const& request, RequestOptions const& options) -> Result<std::optional<std::string>> {
            if (options.extra_body && !options.extra_body->is_object()) {
                return std::unexpected{ Error{ .kind = ErrorKind::InvalidArgument, .message = "extra_body must be a JSON object" } };
            }
            if (request.method != HttpMethod::Post) {
                if (request.body || (options.extra_body && !options.extra_body->empty())) {
                    return std::unexpected{ Error{ .kind = ErrorKind::InvalidArgument, .message = "only POST requests may carry a JSON body" } };
                }
                return std::optional<std::string>{};
            }
            Json body = request.body.value_or(Json::object());
            if (options.extra_body) {
                body.update(*options.extra_body);
            }
            try {
                return std::optional<std::string>{ body.dump() };
            } catch (Json::exception const& failure) {
                return std::unexpected{ Error{ .kind = ErrorKind::InvalidArgument, .message = std::format("cannot serialize request body: {}", failure.what()) } };
            }
        }

        /**
         * @brief Combine default, resource, and per-request query parameters in transmission order.
         */
        auto build_parameters(ClientOptions const& config, ApiRequest const& request, RequestOptions const& options) -> mcr::Parameters {
            mcr::Parameters parameters;
            for (auto const* list : { &config.default_query, &request.query, &options.extra_query }) {
                for (auto const& [key, value] : *list) {
                    parameters.Add(mcr::Parameter{ key, value });
                }
            }
            return parameters;
        }

        /**
         * @brief Build the final request headers, applying default then per-request overrides.
         * @param stream Whether the request expects an event stream.
         */
        auto build_headers(ClientOptions const& config, RequestOptions const& options, bool has_body, bool stream) -> mcr::Header {
            mcr::Header headers;
            headers["Authorization"] = "Bearer " + *config.api_key;
            if (config.organization) {
                headers["OpenAI-Organization"] = *config.organization;
            }
            if (config.project) {
                headers["OpenAI-Project"] = *config.project;
            }
            headers["Accept"] = stream ? "text/event-stream" : "application/json";
            if (has_body) {
                headers["Content-Type"] = "application/json";
            }
            headers["User-Agent"] = std::format("moai/{}", VERSION);
            for (auto const* extra : { &config.default_headers, &options.extra_headers }) {
                for (auto const& [name, value] : *extra) {
                    headers.insert_or_assign(name, value);
                }
            }
            return headers;
        }

        /**
         * @brief Build the absolute request URL.
         */
        auto build_url(ClientOptions const& config, ApiRequest const& request) -> mcr::Url {
            return mcr::Url(*config.base_url + request.path);
        }

        /**
         * @brief Configure a fresh session for one attempt.
         */
        auto configure(
            mcr::Session& session,
            ClientOptions const& config,
            mcr::ConnectionPool const& pool,
            ApiRequest const& request,
            RequestOptions const& options,
            std::optional<std::string> const& body,
            bool stream
        ) -> Result<void> {
            if (auto status{ check(session.SetUrl(build_url(config, request))) }; !status) {
                return status;
            }
            if (auto status{ check(session.SetParameters(build_parameters(config, request, options))) }; !status) {
                return status;
            }
            if (auto status{ check(session.SetHeader(build_headers(config, options, body.has_value(), stream))) }; !status) {
                return status;
            }
            if (auto status{ check(session.SetTimeout(mcr::options::Timeout(options.timeout.value_or(config.timeout)))) }; !status) {
                return status;
            }
            if (auto status{ check(session.SetConnectTimeout(mcr::options::ConnectTimeout(config.connect_timeout))) }; !status) {
                return status;
            }
            if (auto status{ check(session.SetConnectionPool(pool)) }; !status) {
                return status;
            }
            if (!config.proxies.empty()) {
                if (auto status{ check(session.SetProxies(mcr::options::Proxies(config.proxies))) }; !status) {
                    return status;
                }
            }
            if (body) {
                if (auto status{ check(session.SetBody(mcr::Body(*body))) }; !status) {
                    return status;
                }
            }
            return {};
        }

        /**
         * @brief Run one attempt; stream is null for buffered requests.
         */
        auto attempt(
            ClientOptions const& config,
            mcr::ConnectionPool const& pool,
            ApiRequest const& request,
            RequestOptions const& options,
            std::optional<std::string> const& body,
            StreamState* stream
        ) -> Attempt {
            if (options.stop_token.stop_requested()) {
                return Attempt{ .result = std::unexpected{ cancelled_error() } };
            }
            auto created = mcr::Session::Create();
            if (!created) {
                return Attempt{ .result = std::unexpected{ setup_error(created.error()) } };
            }
            auto& session{ **created };
            if (auto configured{ configure(session, config, pool, request, options, body, stream != nullptr) }; !configured) {
                return Attempt{ .result = std::unexpected{ std::move(configured.error()) } };
            }

            auto const cancelled{ std::make_shared<std::atomic_bool>(false) };
            if (options.stop_token.stop_possible()) {
                if (auto status{ check(session.SetCancellationParam(cancelled)) }; !status) {
                    return Attempt{ .result = std::unexpected{ std::move(status.error()) } };
                }
            }
            std::stop_callback const on_stop{ options.stop_token, [cancelled] { cancelled->store(true); } };

            if (stream != nullptr) {
                auto status{ check(session.SetHeaderCallback(mcr::HeaderCallback{ [stream](std::string_view line, std::intptr_t) {
                    stream->OnHeader(line);
                    return true;
                } })) };
                if (status) {
                    status = check(session.SetWriteCallback(mcr::WriteCallback{ [stream](std::string_view bytes, std::intptr_t) { return stream->OnBody(bytes); } }));
                }
                if (!status) {
                    return Attempt{ .result = std::unexpected{ std::move(status.error()) } };
                }
            }

            auto performed = [&] {
                switch (request.method) {
                    case HttpMethod::Post:
                        return session.Post();
                    case HttpMethod::Delete:
                        return session.Delete();
                    case HttpMethod::Get:
                        break;
                }
                return session.Get();
            }();
            if (!performed) {
                return Attempt{ .result = std::unexpected{ setup_error(performed.error()) } };
            }

            auto& response{ *performed };
            auto request_id{ std::string{ header_value(response.header, "x-request-id").value_or(std::string_view{}) } };
            if (stream != nullptr && stream->failure) {
                auto failure{ std::move(*stream->failure) };
                failure.status_code = response.status_code;
                failure.request_id = std::move(request_id);
                return Attempt{ .result = std::unexpected{ std::move(failure) } };
            }
            if (response.error) {
                auto const was_cancelled{ cancelled->load() };
                auto error{ transport_error(response.error, was_cancelled) };
                error.request_id = std::move(request_id);
                auto const retryable{ !was_cancelled && (stream == nullptr || stream->delivered == 0) };
                return Attempt{ .result = std::unexpected{ std::move(error) }, .retryable = retryable };
            }
            if (response.status_code < 200 || response.status_code >= 300) {
                auto const& text{ stream != nullptr ? stream->body : response.text };
                return Attempt{
                    .result = std::unexpected{ status_error(response.status_code, std::move(request_id), text) },
                    .retryable = should_retry(response.status_code, response.header),
                    .headers = std::move(response.header),
                };
            }
            if (stream != nullptr) {
                if (!stream->event_stream) {
                    return Attempt{ .result = std::unexpected{ Error{
                                        .kind = ErrorKind::Stream,
                                        .message = std::format("expected text/event-stream, got '{}'", stream->content_type),
                                        .status_code = response.status_code,
                                        .request_id = std::move(request_id),
                                        .body = stream->body.substr(0, MAX_ERROR_BODY),
                                    } } };
                }
                if (!stream->done) {
                    return Attempt{ .result = std::unexpected{ Error{
                                        .kind = ErrorKind::Stream,
                                        .message = "stream ended before [DONE]",
                                        .status_code = response.status_code,
                                        .request_id = std::move(request_id),
                                    } } };
                }
            }
            return Attempt{ .result = RawResponse{
                                .status_code = response.status_code,
                                .headers = std::move(response.header),
                                .body = std::move(response.text),
                                .request_id = std::move(request_id),
                            } };
        }

        /**
         * @brief Repeat attempts according to the retry policy.
         * @param make_stream Creates fresh stream state for each attempt, or null for buffered requests.
         */
        auto execute(
            ClientOptions const& config,
            mcr::ConnectionPool const& pool,
            ApiRequest const& request,
            RequestOptions const& options,
            StreamEventHandler const* handler
        ) -> Result<RawResponse> {
            auto body{ serialize_body(request, options) };
            if (!body) {
                return std::unexpected{ std::move(body.error()) };
            }
            auto const max_retries{ options.max_retries.value_or(config.max_retries) };
            for (int retry{};; ++retry) {
                std::optional<StreamState> stream;
                if (handler != nullptr) {
                    stream.emplace();
                    stream->handler = handler;
                }
                auto outcome{ attempt(config, pool, request, options, *body, stream ? &*stream : nullptr) };
                if (outcome.result || !outcome.retryable || retry >= max_retries) {
                    return std::move(outcome.result);
                }
                auto const delay{ retry_delay(retry, outcome.headers ? &*outcome.headers : nullptr) };
                if (!wait_for_retry(delay, options.stop_token)) {
                    return std::unexpected{ cancelled_error() };
                }
            }
        }

        /**
         * @brief Build one coroutine request with the same options as the synchronous path.
         * @param header_callback Optional header consumer used by streaming attempts.
         * @param write_callback Optional body consumer used by streaming attempts.
         * @return mcr task yielding the raw response or a setup error.
         */
        template <mcr::Method M>
        auto coro_request_once(
            ClientOptions const& config,
            mcr::ConnectionPool const& pool,
            ApiRequest const& request,
            RequestOptions const& options,
            std::optional<std::string> const& body,
            std::stop_token const& token,
            mcr::HeaderCallback const* header_callback = nullptr,
            mcr::WriteCallback const* write_callback = nullptr
        ) -> mcr::Task<mcr::Result<mcr::Response>> {
            auto task{ [&] {
                auto url{ build_url(config, request) };
                auto parameters{ build_parameters(config, request, options) };
                auto headers{ build_headers(config, options, body.has_value(), header_callback != nullptr) };
                auto timeout{ mcr::options::Timeout(options.timeout.value_or(config.timeout)) };
                auto connect_timeout{ mcr::options::ConnectTimeout(config.connect_timeout) };
                auto proxies{ mcr::options::Proxies(config.proxies) };
                auto owned_pool{ pool };
                if constexpr (M == mcr::Method::Post) {
                    auto owned_body{ mcr::Body(*body) };
                    if (header_callback != nullptr) {
                        return mcr::request_coro<M>(
                            std::move(url),
                            std::move(parameters),
                            std::move(headers),
                            timeout,
                            connect_timeout,
                            std::move(owned_pool),
                            std::move(proxies),
                            std::move(owned_body),
                            *header_callback,
                            *write_callback
                        );
                    }
                    return mcr::request_coro<M>(
                        std::move(url),
                        std::move(parameters),
                        std::move(headers),
                        timeout,
                        connect_timeout,
                        std::move(owned_pool),
                        std::move(proxies),
                        std::move(owned_body)
                    );
                } else {
                    if (header_callback != nullptr) {
                        return mcr::request_coro<M>(
                            std::move(url),
                            std::move(parameters),
                            std::move(headers),
                            timeout,
                            connect_timeout,
                            std::move(owned_pool),
                            std::move(proxies),
                            *header_callback,
                            *write_callback
                        );
                    }
                    return mcr::request_coro<M>(
                        std::move(url),
                        std::move(parameters),
                        std::move(headers),
                        timeout,
                        connect_timeout,
                        std::move(owned_pool),
                        std::move(proxies)
                    );
                }
            }() };
            // Propagate cancellation of the caller's task to the child request task.
            auto source{ task.GetStopSource() };
            std::stop_callback propagate{ token, [source]() mutable { source.request_stop(); } };
            co_return co_await std::move(task);
        }

        /**
         * @brief Map a coroutine response to the retry-aware attempt outcome.
         */
        auto coro_outcome(mcr::Result<mcr::Response>&& result, std::stop_token const& token) -> Attempt {
            if (!result) {
                return Attempt{ .result = std::unexpected{ setup_error(result.error()) } };
            }
            auto& response{ *result };
            auto request_id{ std::string{ header_value(response.header, "x-request-id").value_or(std::string_view{}) } };
            if (response.error) {
                auto const cancelled{ token.stop_requested() || response.error.code == mcr::ErrorCode::ABORTED_BY_CALLBACK };
                auto error{ transport_error(response.error, cancelled) };
                error.request_id = std::move(request_id);
                return Attempt{ .result = std::unexpected{ std::move(error) }, .retryable = !cancelled };
            }
            if (response.status_code < 200 || response.status_code >= 300) {
                auto const retryable{ should_retry(response.status_code, response.header) };
                return Attempt{
                    .result = std::unexpected{ status_error(response.status_code, std::move(request_id), response.text) },
                    .retryable = retryable,
                    .headers = std::move(response.header),
                };
            }
            return Attempt{ .result = RawResponse{
                                .status_code = response.status_code,
                                .headers = std::move(response.header),
                                .body = std::move(response.text),
                                .request_id = std::move(request_id),
                            } };
        }

        /**
         * @brief Map a coroutine streaming response, preserving buffered non-SSE error bodies.
         */
        auto coro_stream_outcome(mcr::Result<mcr::Response>&& result, StreamState& stream, std::stop_token const& token) -> Attempt {
            if (!result) {
                return Attempt{ .result = std::unexpected{ setup_error(result.error()) } };
            }
            auto& response{ *result };
            auto request_id{ std::string{ header_value(response.header, "x-request-id").value_or(std::string_view{}) } };
            if (stream.failure) {
                auto failure{ std::move(*stream.failure) };
                failure.status_code = response.status_code;
                failure.request_id = std::move(request_id);
                return Attempt{ .result = std::unexpected{ std::move(failure) } };
            }
            if (response.error) {
                auto const cancelled{ token.stop_requested() || response.error.code == mcr::ErrorCode::ABORTED_BY_CALLBACK };
                auto error{ transport_error(response.error, cancelled) };
                error.request_id = std::move(request_id);
                auto const retryable{ !cancelled && stream.delivered == 0 };
                return Attempt{ .result = std::unexpected{ std::move(error) }, .retryable = retryable };
            }
            if (response.status_code < 200 || response.status_code >= 300) {
                auto const retryable{ should_retry(response.status_code, response.header) };
                return Attempt{
                    .result = std::unexpected{ status_error(response.status_code, std::move(request_id), stream.body) },
                    .retryable = retryable,
                    .headers = std::move(response.header),
                };
            }
            if (!stream.event_stream) {
                return Attempt{ .result = std::unexpected{ Error{
                                    .kind = ErrorKind::Stream,
                                    .message = std::format("expected text/event-stream, got '{}'", stream.content_type),
                                    .status_code = response.status_code,
                                    .request_id = std::move(request_id),
                                    .body = stream.body.substr(0, MAX_ERROR_BODY),
                                } } };
            }
            if (!stream.done) {
                return Attempt{ .result = std::unexpected{ Error{
                                    .kind = ErrorKind::Stream,
                                    .message = "stream ended before [DONE]",
                                    .status_code = response.status_code,
                                    .request_id = std::move(request_id),
                                } } };
            }
            return Attempt{ .result = RawResponse{
                                .status_code = response.status_code,
                                .headers = std::move(response.header),
                                .body = std::move(response.text),
                                .request_id = std::move(request_id),
                            } };
        }

        /**
         * @brief Run one buffered attempt through the coroutine runtime.
         */
        auto coro_attempt(
            ClientOptions const& config,
            mcr::ConnectionPool const& pool,
            ApiRequest const& request,
            RequestOptions const& options,
            std::optional<std::string> const& body,
            std::stop_token const& token
        ) -> mcr::Task<Attempt> {
            std::optional<mcr::Result<mcr::Response>> response;
            switch (request.method) {
                case HttpMethod::Post:
                    response.emplace(co_await coro_request_once<mcr::Method::Post>(config, pool, request, options, body, token));
                    break;
                case HttpMethod::Delete:
                    response.emplace(co_await coro_request_once<mcr::Method::Delete>(config, pool, request, options, body, token));
                    break;
                case HttpMethod::Get:
                    response.emplace(co_await coro_request_once<mcr::Method::Get>(config, pool, request, options, body, token));
                    break;
            }
            co_return coro_outcome(std::move(*response), token);
        }

        /**
         * @brief Run one streaming attempt through the coroutine runtime.
         */
        auto coro_stream_attempt(
            ClientOptions const& config,
            mcr::ConnectionPool const& pool,
            ApiRequest const& request,
            RequestOptions const& options,
            std::optional<std::string> const& body,
            StreamState& stream,
            std::stop_token const& token
        ) -> mcr::Task<Attempt> {
            mcr::HeaderCallback header_callback{ [&stream](std::string_view line, std::intptr_t) {
                stream.OnHeader(line);
                return true;
            } };
            mcr::WriteCallback write_callback{ [&stream](std::string_view bytes, std::intptr_t) { return stream.OnBody(bytes); } };
            std::optional<mcr::Result<mcr::Response>> response;
            switch (request.method) {
                case HttpMethod::Post:
                    response.emplace(
                        co_await coro_request_once<mcr::Method::Post>(config, pool, request, options, body, token, &header_callback, &write_callback)
                    );
                    break;
                case HttpMethod::Delete:
                    response.emplace(
                        co_await coro_request_once<mcr::Method::Delete>(config, pool, request, options, body, token, &header_callback, &write_callback)
                    );
                    break;
                case HttpMethod::Get:
                    response.emplace(
                        co_await coro_request_once<mcr::Method::Get>(config, pool, request, options, body, token, &header_callback, &write_callback)
                    );
                    break;
            }
            co_return coro_stream_outcome(std::move(*response), stream, token);
        }

    } // namespace

    auto ClientCore::Send(ApiRequest const& request, RequestOptions const& options) const -> Result<RawResponse> {
        return execute(m_options, m_pool, request, options, nullptr);
    }

    auto ClientCore::SendStream(ApiRequest const& request, RequestOptions const& options, StreamEventHandler const& on_event) const -> Result<void> {
        auto result{ execute(m_options, m_pool, request, options, &on_event) };
        if (!result) {
            return std::unexpected{ std::move(result.error()) };
        }
        return {};
    }

    auto send_coro(std::shared_ptr<ClientCore const> core, ApiRequest request, RequestOptions options) -> mcr::Task<Result<RawResponse>> {
        auto body{ serialize_body(request, options) };
        if (!body) {
            co_return std::unexpected{ std::move(body.error()) };
        }
        auto const max_retries{ options.max_retries.value_or(core->Options().max_retries) };
        auto bridge{ std::stop_source{} };
        auto const task_token{ co_await mcr::this_coro::stop_token };
        std::stop_callback task_stop{ task_token, [&bridge] { bridge.request_stop(); } };
        std::stop_callback option_stop{ options.stop_token, [&bridge] { bridge.request_stop(); } };
        if (bridge.get_token().stop_requested()) {
            co_return std::unexpected{ cancelled_error() };
        }
        for (int retry{};; ++retry) {
            auto outcome{ co_await coro_attempt(core->Options(), core->Pool(), request, options, *body, bridge.get_token()) };
            if (outcome.result || !outcome.retryable || retry >= max_retries) {
                co_return std::move(outcome.result);
            }
            auto const delay{ retry_delay(retry, outcome.headers ? &*outcome.headers : nullptr) };
            auto waited{ co_await mcr::Coro::ScheduleAfter(std::chrono::duration_cast<std::chrono::milliseconds>(delay), bridge.get_token()) };
            if (!waited) {
                if (waited.error().code == mcr::ErrorCode::ABORTED_BY_CALLBACK) {
                    co_return std::unexpected{ cancelled_error() };
                }
                co_return std::unexpected{ setup_error(waited.error()) };
            }
        }
    }

    auto send_stream_coro(
        std::shared_ptr<ClientCore const> core,
        ApiRequest request,
        RequestOptions options,
        StreamEventHandler on_event
    ) -> mcr::Task<Result<void>> {
        auto body{ serialize_body(request, options) };
        if (!body) {
            co_return std::unexpected{ std::move(body.error()) };
        }
        auto const max_retries{ options.max_retries.value_or(core->Options().max_retries) };
        auto bridge{ std::stop_source{} };
        auto const task_token{ co_await mcr::this_coro::stop_token };
        std::stop_callback task_stop{ task_token, [&bridge] { bridge.request_stop(); } };
        std::stop_callback option_stop{ options.stop_token, [&bridge] { bridge.request_stop(); } };
        if (bridge.get_token().stop_requested()) {
            co_return std::unexpected{ cancelled_error() };
        }
        for (int retry{};; ++retry) {
            StreamState stream;
            stream.handler = &on_event;
            auto outcome{ co_await coro_stream_attempt(core->Options(), core->Pool(), request, options, *body, stream, bridge.get_token()) };
            if (outcome.result) {
                co_return Result<void>{};
            }
            if (!outcome.retryable || retry >= max_retries) {
                co_return std::unexpected{ std::move(outcome.result.error()) };
            }
            auto const delay{ retry_delay(retry, outcome.headers ? &*outcome.headers : nullptr) };
            auto waited{ co_await mcr::Coro::ScheduleAfter(std::chrono::duration_cast<std::chrono::milliseconds>(delay), bridge.get_token()) };
            if (!waited) {
                if (waited.error().code == mcr::ErrorCode::ABORTED_BY_CALLBACK) {
                    co_return std::unexpected{ cancelled_error() };
                }
                co_return std::unexpected{ setup_error(waited.error()) };
            }
        }
    }

    auto status_error(long status_code, std::string request_id, std::string_view body) -> Error {
        Error error{
            .kind = error_kind_from_status(status_code),
            .status_code = status_code,
            .request_id = std::move(request_id),
            .body = std::string{ body.substr(0, MAX_ERROR_BODY) },
        };
        fill_api_error(error, Json::parse(body, nullptr, false));
        if (error.message.empty()) {
            auto const text{ trim(body.substr(0, 512)) };
            error.message = text.empty() ? std::format("HTTP {}", status_code) : std::format("HTTP {}: {}", status_code, text);
        }
        return error;
    }

    auto encode_path_segment(std::string_view segment) -> std::string {
        constexpr std::string_view HEX{ "0123456789ABCDEF" };
        std::string result;
        result.reserve(segment.size());
        for (auto const character : segment) {
            auto const byte{ static_cast<unsigned char>(character) };
            auto const unreserved{ (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-' || character == '.' || character == '_' || character == '~' };
            if (unreserved) {
                result += character;
            } else {
                result += '%';
                result += HEX[byte >> 4];
                result += HEX[byte & 0x0F];
            }
        }
        return result;
    }

    auto require_id(std::string_view id, std::string_view name) -> Result<void> {
        if (id.empty()) {
            return std::unexpected{ Error{ .kind = ErrorKind::InvalidArgument, .message = std::format("{} must not be empty", name) } };
        }
        return {};
    }

} // namespace moai::detail
