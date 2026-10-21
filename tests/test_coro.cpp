/**
 * @file test_coro.cpp
 * @brief Coroutine Chat Completions: results, retry backoff, cancellation, ownership, and streaming.
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

    constexpr std::string_view COMPLETION{ R"({"id":"chatcmpl-c","object":"chat.completion","created":1,"model":"m","choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"ok"}}]})" };
    constexpr std::string_view FAILURE{ R"({"error":{"message":"failure"}})" };

    auto make_client(MockServer const& server, int max_retries = 0) -> moai::Client {
        return moai::Client::Create({ .api_key = "sk-test", .base_url = server.BaseUrl(), .max_retries = max_retries }).value();
    }

    auto params() -> moai::ChatCompletionCreateParams {
        return { .model = "m", .messages = { moai::user_message("hi") } };
    }

    auto event(std::string_view payload) -> std::string {
        return std::format("data: {}\n\n", payload);
    }

    auto chunk(std::string_view choices) -> std::string {
        return std::format(R"({{"id":"chatcmpl-c","object":"chat.completion.chunk","created":2,"model":"m","choices":{}}})", choices);
    }

    auto test_success_and_error(Checker& check) -> void {
        {
            MockServer server;
            auto client{ make_client(server) };
            server.Enqueue(ScriptedResponse::Json(200, std::string{ COMPLETION }));
            auto result{ mcr::sync_wait(client.chat.completions.CreateCoro(params())) };
            check.Check(result.has_value(), "coroutine completion succeeds");
            check.Equal(result ? result->id : std::string{}, std::string{ "chatcmpl-c" }, "coroutine completion id");
            check.Equal(server.RequestCount(), std::size_t{ 1 }, "one coroutine request");
        }
        {
            MockServer server;
            auto client{ make_client(server) };
            server.Enqueue(ScriptedResponse::Json(401, std::string{ FAILURE }));
            auto result{ mcr::sync_wait(client.chat.completions.CreateCoro(params())) };
            check.Check(!result && result.error().kind == moai::ErrorKind::Authentication, "coroutine HTTP status maps to an error");
        }
    }

    auto test_retry_backoff(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server, 2) };
        server.Enqueue(ScriptedResponse::Json(503, std::string{ FAILURE }, { { "retry-after-ms", "150" } }));
        server.Enqueue(ScriptedResponse::Json(200, std::string{ COMPLETION }));
        auto const start{ std::chrono::steady_clock::now() };
        auto result{ mcr::sync_wait(client.chat.completions.CreateCoro(params())) };
        auto const elapsed{ std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start) };
        check.Check(result.has_value(), "retryable coroutine status is retried");
        check.Equal(server.RequestCount(), std::size_t{ 2 }, "one coroutine retry");
        check.Check(elapsed >= 100ms, std::format("retry backoff waits through the coroutine timer (took {})", elapsed));
        auto const requests{ server.Requests() };
        check.Equal(requests[0].body, requests[1].body, "coroutine retry replays the same body");
    }

    auto test_cancellation(Checker& check) -> void {
        {
            MockServer server;
            auto client{ make_client(server) };
            auto task{ client.chat.completions.CreateCoro(params()) };
            auto source{ task.GetStopSource() };
            source.request_stop();
            auto result{ mcr::sync_wait(std::move(task)) };
            check.Check(!result && result.error().kind == moai::ErrorKind::Cancelled, "pre-stopped coroutine is cancelled");
            check.Equal(server.RequestCount(), std::size_t{ 0 }, "cancelled coroutine sends no request");
        }
        {
            MockServer server;
            auto client{ make_client(server) };
            server.Enqueue(ScriptedResponse{ .status = 200, .chunks = { std::string{ COMPLETION } }, .delay = 5s });
            auto task{ client.chat.completions.CreateCoro(params()) };
            auto source{ task.GetStopSource() };
            task.Start();
            std::jthread canceller{ [&source] {
                std::this_thread::sleep_for(200ms);
                source.request_stop();
            } };
            auto const start{ std::chrono::steady_clock::now() };
            auto result{ mcr::sync_wait(std::move(task)) };
            auto const elapsed{ std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start) };
            check.Check(!result && result.error().kind == moai::ErrorKind::Cancelled, "stop during transfer cancels the coroutine");
            check.Check(elapsed < 3s, std::format("coroutine transfer is aborted (took {})", elapsed));
        }
        {
            MockServer server;
            auto client{ make_client(server, 2) };
            server.Enqueue(ScriptedResponse::Json(429, std::string{ FAILURE }, { { "retry-after", "30" } }));
            auto task{ client.chat.completions.CreateCoro(params()) };
            auto source{ task.GetStopSource() };
            task.Start();
            std::jthread canceller{ [&source] {
                std::this_thread::sleep_for(200ms);
                source.request_stop();
            } };
            auto const start{ std::chrono::steady_clock::now() };
            auto result{ mcr::sync_wait(std::move(task)) };
            auto const elapsed{ std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start) };
            check.Check(!result && result.error().kind == moai::ErrorKind::Cancelled, "stop interrupts the coroutine retry wait");
            check.Check(elapsed < 3s, std::format("coroutine backoff is interrupted (took {})", elapsed));
            check.Equal(server.RequestCount(), std::size_t{ 1 }, "no coroutine attempt after cancellation");
        }
    }

    auto test_client_ownership(Checker& check) -> void {
        MockServer server;
        auto task{ make_client(server).chat.completions.CreateCoro(params()) };
        server.Enqueue(ScriptedResponse::Json(200, std::string{ COMPLETION }));
        auto result{ mcr::sync_wait(std::move(task)) };
        check.Check(result.has_value(), "task keeps the client state alive after the Client is destroyed");
    }

    auto test_streaming(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server) };
        server.Enqueue(ScriptedResponse::Sse({
            event(chunk(R"([{"index":0,"delta":{"content":"Hel"}}])")),
            event(chunk(R"([{"index":0,"delta":{"content":"lo"}}])")),
            "data: [DONE]\n\n",
        }));
        std::vector<std::string> contents;
        auto result{ mcr::sync_wait(client.chat.completions.CreateStreamCoro(params(), [&](moai::ChatCompletionChunk const& value) {
            if (!value.choices.empty()) {
                contents.push_back(value.choices[0].delta.content.value_or(""));
            }
            return true;
        })) };
        if (!check.Check(result.has_value(), "coroutine stream completes")) {
            std::println("  error: {}", result.error());
            return;
        }
        check.Equal(contents.size(), std::size_t{ 2 }, "every coroutine chunk is delivered");
        check.Check(contents.size() == 2 && contents[0] == "Hel" && contents[1] == "lo", "coroutine chunk contents are preserved");
        auto const request = moai::Json::parse(server.LastRequest().body);
        check.Check(request.value("stream", false), "coroutine stream sets the stream flag");
    }

    auto test_stream_cancellation(Checker& check) -> void {
        {
            MockServer server;
            auto client{ make_client(server) };
            auto result{ mcr::sync_wait(client.chat.completions.CreateStreamCoro(params(), {})) };
            check.Check(!result && result.error().kind == moai::ErrorKind::InvalidArgument, "empty coroutine stream callback is rejected");
            check.Equal(server.RequestCount(), std::size_t{ 0 }, "rejected coroutine stream sends no request");
        }
        {
            MockServer server;
            auto client{ make_client(server) };
            server.Enqueue(ScriptedResponse::Sse({
                event(chunk(R"([{"index":0,"delta":{"content":"first"}}])")),
                event(chunk(R"([{"index":0,"delta":{"content":"second"}}])")),
                "data: [DONE]\n\n",
            }));
            std::size_t delivered{};
            auto result{ mcr::sync_wait(client.chat.completions.CreateStreamCoro(params(), [&](moai::ChatCompletionChunk const&) {
                ++delivered;
                return false;
            })) };
            check.Check(!result && result.error().kind == moai::ErrorKind::Cancelled, "false from the coroutine chunk callback cancels the stream");
            check.Equal(delivered, std::size_t{ 1 }, "no chunk is delivered after the callback stops the stream");
        }
    }
} // namespace

auto main() -> int {
    try {
        Checker check;
        test_success_and_error(check);
        test_retry_backoff(check);
        test_cancellation(check);
        test_client_ownership(check);
        test_streaming(check);
        test_stream_cancellation(check);
        auto const cleanup{ mcr::Coro::Cleanup() };
        if (!cleanup) {
            std::println("test_coro: coroutine cleanup failed: {}", cleanup.error().message);
        }
        return check.Finish("test_coro");
    } catch (std::exception const& error) {
        std::println("test_coro: unexpected exception: {}", error.what());
        return 1;
    }
}
