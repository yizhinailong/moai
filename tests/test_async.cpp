/**
 * @file test_async.cpp
 * @brief Thread-pool Chat Completions: results, errors, cancellation, concurrency, and streaming.
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

    constexpr std::string_view COMPLETION{ R"({"id":"chatcmpl-a","object":"chat.completion","created":1,"model":"m","choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"ok"}}]})" };
    constexpr std::string_view FAILURE{ R"({"error":{"message":"failure","type":"invalid_request_error","code":"bad"}})" };

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
        return std::format(R"({{"id":"chatcmpl-a","object":"chat.completion.chunk","created":2,"model":"m","choices":{}}})", choices);
    }

    auto test_success_and_error(Checker& check) -> void {
        {
            MockServer server;
            auto client{ make_client(server) };
            server.Enqueue(ScriptedResponse::Json(200, std::string{ COMPLETION }));
            auto future{ client.chat.completions.CreateAsync(params()) };
            if (!check.Check(future.has_value(), "async task is submitted")) {
                std::println("  error: {}", future.error());
                return;
            }
            auto result{ future->get() };
            check.Check(result.has_value(), "async completion succeeds");
            check.Equal(result ? result->id : std::string{}, std::string{ "chatcmpl-a" }, "async completion id");
            check.Equal(server.RequestCount(), std::size_t{ 1 }, "one asynchronous request");
            auto const recorded{ server.LastRequest() };
            auto const body = moai::Json::parse(recorded.body);
            check.Equal(body.value("model", std::string{}), std::string{ "m" }, "request body is sent");
        }
        {
            MockServer server;
            auto client{ make_client(server) };
            server.Enqueue(ScriptedResponse::Json(401, std::string{ FAILURE }));
            auto future{ client.chat.completions.CreateAsync(params()) };
            check.Check(future.has_value(), "failing async task is submitted");
            auto result{ future ? future->get() : moai::Result<moai::ChatCompletion>{ std::unexpected{ future.error() } } };
            check.Check(!result && result.error().kind == moai::ErrorKind::Authentication, "async HTTP status maps to an error");
            check.Check(!result && result.error().request_id.empty(), "request id stays empty without the header");
        }
    }

    auto test_cancellation(Checker& check) -> void {
        {
            MockServer server;
            auto client{ make_client(server) };
            std::stop_source source;
            source.request_stop();
            auto future{ client.chat.completions.CreateAsync(params(), { .stop_token = source.get_token() }) };
            check.Check(future.has_value(), "cancelled async task is submitted");
            auto result{ future ? future->get() : moai::Result<moai::ChatCompletion>{ std::unexpected{ future.error() } } };
            check.Check(!result && result.error().kind == moai::ErrorKind::Cancelled, "pre-stopped token cancels the async task");
            check.Equal(server.RequestCount(), std::size_t{ 0 }, "cancelled async task sends no request");
        }
        {
            MockServer server;
            auto client{ make_client(server) };
            server.Enqueue(ScriptedResponse{ .status = 200, .chunks = { std::string{ COMPLETION } }, .delay = 5s });
            std::stop_source source;
            auto future{ client.chat.completions.CreateAsync(params(), { .stop_token = source.get_token() }) };
            if (!check.Check(future.has_value(), "task is submitted before cancellation")) {
                return;
            }
            std::jthread canceller{ [&source] {
                std::this_thread::sleep_for(200ms);
                source.request_stop();
            } };
            auto const start{ std::chrono::steady_clock::now() };
            auto result{ future->get() };
            auto const elapsed{ std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start) };
            check.Check(!result && result.error().kind == moai::ErrorKind::Cancelled, "stop during transfer cancels the async task");
            check.Check(elapsed < 3s, std::format("async transfer is aborted (took {})", elapsed));
        }
    }

    auto test_concurrency(Checker& check) -> void {
        constexpr int COUNT{ 4 };
        MockServer server;
        auto client{ make_client(server) };
        for (int index{}; index < COUNT; ++index) {
            server.Enqueue(ScriptedResponse::Json(200, std::string{ COMPLETION }));
        }
        std::vector<std::future<moai::Result<moai::ChatCompletion>>> futures;
        for (int index{}; index < COUNT; ++index) {
            auto future{ client.chat.completions.CreateAsync(params()) };
            if (!check.Check(future.has_value(), "concurrent task is submitted")) {
                return;
            }
            futures.push_back(std::move(*future));
        }
        bool succeeded{ true };
        for (auto& future : futures) {
            auto result{ future.get() };
            succeeded = succeeded && result.has_value();
        }
        check.Check(succeeded, "concurrent async completions succeed");
        check.Equal(server.RequestCount(), static_cast<std::size_t>(COUNT), "every concurrent request is sent");
    }

    auto test_streaming(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server) };
        server.Enqueue(ScriptedResponse::Sse({
            event(chunk(R"([{"index":0,"delta":{"content":"Hel"}}])")),
            event(chunk(R"([{"index":0,"delta":{"content":"lo"}}])")),
            "data: [DONE]\n\n",
        }));

        std::mutex mutex;
        std::vector<std::string> contents;
        auto future{ client.chat.completions.CreateStreamAsync(params(), [&](moai::ChatCompletionChunk const& value) {
            std::scoped_lock lock{ mutex };
            if (!value.choices.empty()) {
                contents.push_back(value.choices[0].delta.content.value_or(""));
            }
            return true;
        }) };
        if (!check.Check(future.has_value(), "streaming async task is submitted")) {
            std::println("  error: {}", future.error());
            return;
        }
        auto result{ future->get() };
        if (!check.Check(result.has_value(), "streaming async task completes")) {
            std::println("  error: {}", result.error());
            return;
        }
        std::scoped_lock lock{ mutex };
        check.Equal(contents.size(), std::size_t{ 2 }, "every streamed chunk is delivered");
        check.Check(contents.size() == 2 && contents[0] == "Hel" && contents[1] == "lo", "chunk contents are preserved");
        auto const recorded{ server.LastRequest() };
        auto const request = moai::Json::parse(recorded.body);
        check.Check(request.value("stream", false), "async stream sets the stream flag");
    }

    auto test_empty_callback(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server) };
        auto future{ client.chat.completions.CreateStreamAsync(params(), {}) };
        check.Check(!future && future.error().kind == moai::ErrorKind::InvalidArgument, "empty async stream callback is rejected");
        check.Equal(server.RequestCount(), std::size_t{ 0 }, "rejected stream sends no request");
    }
} // namespace

auto main() -> int {
    try {
        Checker check;
        test_success_and_error(check);
        test_cancellation(check);
        test_concurrency(check);
        test_streaming(check);
        test_empty_callback(check);
        return check.Finish("test_async");
    } catch (std::exception const& error) {
        std::println("test_async: unexpected exception: {}", error.what());
        return 1;
    }
}
