/**
 * @file test_client.cpp
 * @brief Client configuration: environment fallbacks, headers, query parameters, and request overrides.
 */
#include <stdlib.h>

#include "fixtures/mock_server.hpp"
import std;
import moai;

namespace {
    using moai::test::Checker;
    using moai::test::MockServer;
    using moai::test::ScriptedResponse;

    constexpr std::string_view COMPLETION{ R"({"id":"chatcmpl-1","object":"chat.completion","created":1,"model":"m","choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"ok"}}]})" };

    /**
     * @brief Set or remove an environment variable for this process.
     */
    auto set_env(char const* name, std::string const& value) -> void {
#ifdef _WIN32
        (void)_putenv_s(name, value.c_str());
#else
        if (value.empty()) {
            (void)unsetenv(name);
        } else {
            (void)setenv(name, value.c_str(), 1);
        }
#endif
    }

    /**
     * @brief Remove every variable read by Client::Create.
     */
    auto clear_env() -> void {
        for (auto const* name : { "OPENAI_API_KEY", "OPENAI_BASE_URL", "OPENAI_ORG_ID", "OPENAI_PROJECT_ID" }) {
            set_env(name, "");
        }
    }

    auto minimal_params() -> moai::ChatCompletionCreateParams {
        return { .model = "m", .messages = { moai::user_message("hi") } };
    }

    auto test_missing_key(Checker& check) -> void {
        clear_env();
        auto const client{ moai::Client::Create() };
        if (check.Check(!client, "creating a client without a key fails")) {
            check.Equal(client.error().kind, moai::ErrorKind::InvalidArgument, "missing key is InvalidArgument");
            check.Check(client.error().message.contains("API key"), "message mentions the API key");
        }
        auto const negative{ moai::Client::Create({ .api_key = "sk-test", .max_retries = -1 }) };
        check.Check(!negative && negative.error().kind == moai::ErrorKind::InvalidArgument, "negative max_retries is rejected");
    }

    auto test_environment(Checker& check) -> void {
        MockServer server;
        clear_env();
        set_env("OPENAI_API_KEY", "sk-env");
        set_env("OPENAI_BASE_URL", server.BaseUrl() + "/");
        set_env("OPENAI_ORG_ID", "org-env");
        set_env("OPENAI_PROJECT_ID", "proj-env");
        auto client{ moai::Client::Create() };
        clear_env();
        if (!check.Check(client.has_value(), "client from environment")) {
            return;
        }
        server.Enqueue(ScriptedResponse::Json(200, std::string{ COMPLETION }));
        auto const completion{ client->chat.completions.Create(minimal_params()) };
        check.Check(completion.has_value(), "request through environment configuration succeeds");
        auto const request{ server.LastRequest() };
        check.Equal(request.method, std::string{ "POST" }, "method");
        check.Equal(request.Path(), std::string{ "/v1/chat/completions" }, "trailing slash of base URL is trimmed");
        check.Equal(request.Header("authorization"), std::string{ "Bearer sk-env" }, "API key from OPENAI_API_KEY");
        check.Equal(request.Header("openai-organization"), std::string{ "org-env" }, "organization from OPENAI_ORG_ID");
        check.Equal(request.Header("openai-project"), std::string{ "proj-env" }, "project from OPENAI_PROJECT_ID");
        check.Equal(request.Header("content-type"), std::string{ "application/json" }, "JSON content type");
        check.Equal(request.Header("accept"), std::string{ "application/json" }, "JSON accept header");
        check.Check(request.Header("user-agent").starts_with("moai/"), "moai user agent");
    }

    auto test_explicit_options(Checker& check) -> void {
        MockServer server;
        clear_env();
        set_env("OPENAI_API_KEY", "sk-env");
        auto client{ moai::Client::Create({
            .api_key = "sk-explicit",
            .organization = "org-1",
            .base_url = server.BaseUrl(),
            .default_headers = { { "X-Team", "core" }, { "X-Override", "default" } },
            .default_query = { { "api-version", "1" } },
        }) };
        clear_env();
        if (!check.Check(client.has_value(), "client from explicit options")) {
            return;
        }
        server.Enqueue(ScriptedResponse::Json(200, std::string{ COMPLETION }));
        auto const completion{ client->chat.completions.Create(
            minimal_params(),
            { .extra_headers = { { "x-override", "request" } }, .extra_query = { { "trace", "a b" } } }
        ) };
        check.Check(completion.has_value(), "request with overrides succeeds");
        auto const request{ server.LastRequest() };
        check.Equal(request.Header("authorization"), std::string{ "Bearer sk-explicit" }, "explicit key wins over the environment");
        check.Equal(request.Header("openai-organization"), std::string{ "org-1" }, "explicit organization");
        check.Check(request.Header("openai-project").empty(), "no project header when unset");
        check.Equal(request.Header("x-team"), std::string{ "core" }, "default header");
        check.Equal(request.Header("x-override"), std::string{ "request" }, "per-request header overrides default case-insensitively");
        check.Equal(request.Query(), std::string{ "api-version=1&trace=a%20b" }, "default and extra query parameters, encoded");
    }

    auto test_extra_body(Checker& check) -> void {
        MockServer server;
        auto client{ moai::Client::Create({ .api_key = "sk-test", .base_url = server.BaseUrl() }) };
        if (!check.Check(client.has_value(), "client")) {
            return;
        }
        server.Enqueue(ScriptedResponse::Json(200, std::string{ COMPLETION }));
        auto params{ minimal_params() };
        params.temperature = 1.0;
        auto const completion{ client->chat.completions.Create(params, { .extra_body = moai::Json{ { "temperature", 0.5 }, { "vendor_option", true } } }) };
        check.Check(completion.has_value(), "request with extra_body succeeds");
        auto const body = moai::Json::parse(server.LastRequest().body);
        check.Equal(body["temperature"].get<double>(), 0.5, "extra_body overrides typed fields");
        check.Equal(body["vendor_option"].get<bool>(), true, "extra_body adds new fields");

        auto const invalid{ client->chat.completions.Create(params, { .extra_body = moai::Json::array() }) };
        check.Check(!invalid && invalid.error().kind == moai::ErrorKind::InvalidArgument, "non-object extra_body is rejected");
        check.Equal(server.RequestCount(), std::size_t{ 1 }, "rejected request is not sent");
    }

    auto test_error_format(Checker& check) -> void {
        moai::Error const error{ .kind = moai::ErrorKind::Authentication, .message = "bad key", .status_code = 401, .request_id = "req_1" };
        check.Equal(std::format("{}", error), std::string{ "Authentication (HTTP 401, request req_1): bad key" }, "error formatting");
        check.Check(error.IsStatusError(), "401 is a status error");
        moai::Error const transport{ .kind = moai::ErrorKind::Timeout, .message = "timed out" };
        check.Equal(std::format("{}", transport), std::string{ "Timeout: timed out" }, "transport error formatting");
        check.Check(!transport.IsStatusError(), "timeout is not a status error");
    }
} // namespace

auto main() -> int {
    try {
        Checker check;
        test_missing_key(check);
        test_environment(check);
        test_explicit_options(check);
        test_extra_body(check);
        test_error_format(check);
        return check.Finish("test_client");
    } catch (std::exception const& error) {
        std::println("test_client: unexpected exception: {}", error.what());
        return 1;
    }
}
