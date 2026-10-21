/**
 * @file config.cppm
 * @brief Client-wide options, per-request overrides, and their defaults.
 */
export module moai:config;

import std;
import :json;

export namespace moai {

    inline constexpr std::string_view DEFAULT_BASE_URL{ "https://api.openai.com/v1" }; ///< Official API root.
    inline constexpr std::chrono::milliseconds DEFAULT_TIMEOUT{ 600'000 };             ///< Whole-request limit.
    inline constexpr std::chrono::milliseconds DEFAULT_CONNECT_TIMEOUT{ 5'000 };       ///< Connection establishment limit.
    inline constexpr int DEFAULT_MAX_RETRIES{ 2 };                                     ///< Retries after the first attempt.

    /**
     * @brief Additional request headers; names are matched case-insensitively when merged.
     */
    using Headers = std::map<std::string, std::string>;

    /**
     * @brief Additional query parameters in their transmitted order.
     */
    using QueryParameters = std::vector<std::pair<std::string, std::string>>;

    /**
     * @brief Configuration copied into a Client when it is created.
     * @note Unset credentials and the base URL fall back to OPENAI_API_KEY, OPENAI_BASE_URL,
     * OPENAI_ORG_ID, and OPENAI_PROJECT_ID. `timeout` limits a whole transfer, including the time
     * spent reading a stream, unlike the official Python SDK's per-read timeout.
     */
    struct ClientOptions {
        std::optional<std::string> api_key{};                                 ///< Bearer token; required unless OPENAI_API_KEY is set.
        std::optional<std::string> organization{};                            ///< Sent as OpenAI-Organization.
        std::optional<std::string> project{};                                 ///< Sent as OpenAI-Project.
        std::optional<std::string> base_url{};                                ///< API root, for example a local test server.
        std::chrono::milliseconds timeout{ DEFAULT_TIMEOUT };                 ///< Whole-request limit; zero disables it.
        std::chrono::milliseconds connect_timeout{ DEFAULT_CONNECT_TIMEOUT }; ///< Connection limit.
        int max_retries{ DEFAULT_MAX_RETRIES };                               ///< Retries for retryable failures; zero disables them.
        Headers default_headers{};                                            ///< Headers added to every request.
        QueryParameters default_query{};                                      ///< Query parameters added to every request.
        std::map<std::string, std::string> proxies{};                         ///< Scheme to proxy URL, for example {"https", "http://127.0.0.1:7890"}.
    };

    /**
     * @brief Overrides applied to a single request.
     * @note Fields are copied into the request; nothing is borrowed after the call returns.
     */
    struct RequestOptions {
        std::optional<std::chrono::milliseconds> timeout{}; ///< Replaces ClientOptions::timeout.
        std::optional<int> max_retries{};                   ///< Replaces ClientOptions::max_retries.
        Headers extra_headers{};                            ///< Headers that override defaults with the same name.
        QueryParameters extra_query{};                      ///< Query parameters appended after defaults.
        std::optional<Json> extra_body{};                   ///< Object whose members are merged into the JSON body, overriding typed fields.
        std::stop_token stop_token{};                       ///< Requests cancellation of the transfer and any retry wait.
    };

} // namespace moai
