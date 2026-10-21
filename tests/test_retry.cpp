/**
 * @file test_retry.cpp
 * @brief Retry policy, timeouts, and cancellation through std::stop_token.
 */
#include "fixtures/mock_server.hpp"
import std;
import mcr;
import moai;

namespace {
    using moai::test::Checker;
    using moai::test::MockServer;
    using moai::test::ScriptedResponse;
    using namespace std::chrono_literals;

    constexpr std::string_view COMPLETION{ R"({"id":"chatcmpl-1","object":"chat.completion","created":1,"model":"m","choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"ok"}}]})" };
    constexpr std::string_view FAILURE{ R"({"error":{"message":"failure"}})" };

    auto make_client(MockServer const& server, int max_retries) -> moai::Client {
        return moai::Client::Create({ .api_key = "sk-test", .base_url = server.BaseUrl(), .max_retries = max_retries }).value();
    }

    auto params() -> moai::ChatCompletionCreateParams {
        return { .model = "m", .messages = { moai::user_message("hi") } };
    }

    /**
     * @brief Return the elapsed time of a callable.
     */
    template <typename F>
    auto timed(F&& action) -> std::pair<std::invoke_result_t<F&>, std::chrono::milliseconds> {
        auto const start{ std::chrono::steady_clock::now() };
        auto result{ action() };
        return { std::move(result), std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start) };
    }

    auto test_status_retries(Checker& check) -> void {
        {
            MockServer server;
            auto client{ make_client(server, 2) };
            server.Enqueue(ScriptedResponse::Json(429, std::string{ FAILURE }, { { "retry-after-ms", "1" } }));
            server.Enqueue(ScriptedResponse::Json(200, std::string{ COMPLETION }));
            auto const result{ client.chat.completions.Create(params()) };
            check.Check(result.has_value(), "429 is retried");
            check.Equal(server.RequestCount(), std::size_t{ 2 }, "one retry");
            auto const requests{ server.Requests() };
            check.Equal(requests[0].body, requests[1].body, "retry replays the same body");
        }
        {
            MockServer server;
            auto client{ make_client(server, 2) };
            for (int index{}; index < 3; ++index) {
                server.Enqueue(ScriptedResponse::Json(500, std::string{ FAILURE }, { { "retry-after", "0.001" } }));
            }
            auto const result{ client.chat.completions.Create(params()) };
            check.Check(!result && result.error().kind == moai::ErrorKind::InternalServer, "last error is returned");
            check.Equal(server.RequestCount(), std::size_t{ 3 }, "max_retries + 1 attempts");
        }
        {
            MockServer server;
            auto client{ make_client(server, 2) };
            server.Enqueue(ScriptedResponse::Json(500, std::string{ FAILURE }, { { "x-should-retry", "false" } }));
            auto const result{ client.chat.completions.Create(params()) };
            check.Check(!result, "x-should-retry false fails");
            check.Equal(server.RequestCount(), std::size_t{ 1 }, "x-should-retry false prevents retries");
        }
        {
            MockServer server;
            auto client{ make_client(server, 2) };
            server.Enqueue(ScriptedResponse::Json(400, std::string{ FAILURE }, { { "x-should-retry", "true" }, { "retry-after-ms", "1" } }));
            server.Enqueue(ScriptedResponse::Json(400, std::string{ FAILURE }));
            auto const result{ client.chat.completions.Create(params()) };
            check.Check(!result && result.error().kind == moai::ErrorKind::BadRequest, "plain 400 fails");
            check.Equal(server.RequestCount(), std::size_t{ 2 }, "x-should-retry true forces one retry, plain 400 does not retry");
        }
        {
            MockServer server;
            auto client{ make_client(server, 2) };
            server.Enqueue(ScriptedResponse::Json(503, std::string{ FAILURE }));
            auto const result{ client.chat.completions.Create(params(), { .max_retries = 0 }) };
            check.Check(!result, "per-request max_retries = 0 fails");
            check.Equal(server.RequestCount(), std::size_t{ 1 }, "per-request max_retries overrides the client");
        }
    }

    auto test_connection_retry(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server, 1) };
        server.Enqueue(ScriptedResponse{ .drop = true });
        server.Enqueue(ScriptedResponse::Json(200, std::string{ COMPLETION }));
        auto const [result, elapsed]{ timed([&] { return client.chat.completions.Create(params()); }) };
        check.Check(result.has_value(), "dropped connection is retried");
        check.Check(elapsed >= 300ms && elapsed < 2s, std::format("backoff is about 0.5 s (took {})", elapsed));
    }

    auto test_timeout(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server, 0) };
        server.Enqueue(ScriptedResponse{ .status = 200, .chunks = { std::string{ COMPLETION } }, .delay = 2s });
        auto const [result, elapsed]{ timed([&] { return client.chat.completions.Create(params(), { .timeout = 200ms }); }) };
        check.Check(!result && result.error().kind == moai::ErrorKind::Timeout, "slow response times out");
        check.Check(!result && result.error().transport_code == mcr::ErrorCode::OPERATION_TIMEDOUT, "transport code is kept");
        check.Check(elapsed < 1500ms, std::format("timeout is enforced (took {})", elapsed));
    }

    auto test_cancellation(Checker& check) -> void {
        {
            MockServer server;
            auto client{ make_client(server, 2) };
            std::stop_source source;
            source.request_stop();
            auto const result{ client.chat.completions.Create(params(), { .stop_token = source.get_token() }) };
            check.Check(!result && result.error().kind == moai::ErrorKind::Cancelled, "stopped token cancels before sending");
            check.Equal(server.RequestCount(), std::size_t{ 0 }, "no request is sent");
        }
        {
            MockServer server;
            auto client{ make_client(server, 2) };
            server.Enqueue(ScriptedResponse::Json(503, std::string{ FAILURE }, { { "retry-after", "30" } }));
            std::stop_source source;
            std::jthread canceller{ [&source] {
                std::this_thread::sleep_for(200ms);
                source.request_stop();
            } };
            auto const [result, elapsed]{ timed([&] { return client.chat.completions.Create(params(), { .stop_token = source.get_token() }); }) };
            check.Check(!result && result.error().kind == moai::ErrorKind::Cancelled, "stop during retry wait cancels");
            check.Check(elapsed < 3s, std::format("retry wait is interrupted (took {})", elapsed));
            check.Equal(server.RequestCount(), std::size_t{ 1 }, "no further attempt after cancellation");
        }
        {
            MockServer server;
            auto client{ make_client(server, 2) };
            server.Enqueue(ScriptedResponse{ .status = 200, .chunks = { std::string{ COMPLETION } }, .delay = 5s });
            std::stop_source source;
            std::jthread canceller{ [&source] {
                std::this_thread::sleep_for(200ms);
                source.request_stop();
            } };
            auto const [result, elapsed]{ timed([&] { return client.chat.completions.Create(params(), { .stop_token = source.get_token() }); }) };
            check.Check(!result && result.error().kind == moai::ErrorKind::Cancelled, "stop during transfer cancels");
            check.Check(elapsed < 3s, std::format("transfer is aborted (took {})", elapsed));
            check.Equal(server.RequestCount(), std::size_t{ 1 }, "cancelled transfer is not retried");
        }
    }
} // namespace

auto main() -> int {
    try {
        Checker check;
        test_status_retries(check);
        test_connection_retry(check);
        test_timeout(check);
        test_cancellation(check);
        return check.Finish("test_retry");
    } catch (std::exception const& error) {
        std::println("test_retry: unexpected exception: {}", error.what());
        return 1;
    }
}
