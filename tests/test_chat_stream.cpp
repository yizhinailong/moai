/**
 * @file test_chat_stream.cpp
 * @brief Chat Completions streaming: SSE framing, chunk accumulation, and stream failure modes.
 */
#include "fixtures/mock_server.hpp"
import std;
import moai;

namespace {
    using moai::test::Checker;
    using moai::test::MockServer;
    using moai::test::ScriptedResponse;

    /**
     * @brief Build one chunk document.
     */
    auto chunk(std::string_view choices, std::string_view extra = {}) -> std::string {
        return std::format(R"({{"id":"chatcmpl-s","object":"chat.completion.chunk","created":2,"model":"m","choices":{}{}}})", choices, extra);
    }

    /**
     * @brief Frame a payload as one SSE event.
     */
    auto event(std::string_view payload) -> std::string {
        return std::format("data: {}\n\n", payload);
    }

    auto make_client(MockServer const& server, int max_retries = 0) -> moai::Client {
        return moai::Client::Create({ .api_key = "sk-test", .base_url = server.BaseUrl(), .max_retries = max_retries }).value();
    }

    auto params() -> moai::ChatCompletionCreateParams {
        return { .model = "m", .messages = { moai::user_message("hi") }, .stream_options = moai::ChatCompletionStreamOptions{ .include_usage = true } };
    }

    auto test_stream_and_accumulate(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server) };
        auto const first{ event(chunk(R"([{"index":0,"delta":{"role":"assistant","content":""},"finish_reason":null}])", R"(,"obfuscation":"pad")")) };
        server.Enqueue(ScriptedResponse::Sse({
            first.substr(0, 20),
            first.substr(20),
            ": keep-alive\n\n",
            std::format("data: {}\r\n\r\n", chunk(R"([{"index":0,"delta":{"content":"Hel"}}])")),
            event(chunk(R"([{"index":0,"delta":{"content":"lo"}}])")) + event(chunk(R"([{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"lookup","arguments":""}}]}}])")),
            event(chunk(R"([{"index":0,"delta":{"tool_calls":[{"index":1,"id":"call_2","type":"function","function":{"name":"other","arguments":"{\"b\""}}]}}])")) + event(chunk(R"([{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"a\":1}"}}]}}])")),
            event(chunk(R"([{"index":0,"delta":{"tool_calls":[{"index":1,"function":{"arguments":":2}"}}]},"finish_reason":"tool_calls"}])")),
            event(chunk("[]", R"(,"usage":{"prompt_tokens":4,"completion_tokens":5,"total_tokens":9})")),
            "data: [DONE]\n\n",
        }));

        moai::ChatCompletionAccumulator accumulator;
        std::vector<moai::ChatCompletionChunk> chunks;
        auto const result{ client.chat.completions.CreateStream(params(), [&](moai::ChatCompletionChunk const& value) {
            accumulator.Add(value);
            chunks.push_back(value);
            return true;
        }) };
        if (!check.Check(result.has_value(), "stream completes")) {
            std::println("  error: {}", result.error());
            return;
        }
        check.Equal(chunks.size(), std::size_t{ 8 }, "every data event is delivered once");
        check.Check(!chunks.empty() && chunks.front().raw.contains("obfuscation"), "raw keeps obfuscation");

        auto const request{ server.LastRequest() };
        auto const body = moai::Json::parse(request.body);
        check.Check(body.value("stream", false), "stream flag is sent");
        check.Check(body["stream_options"].value("include_usage", false), "stream options are sent");
        check.Equal(request.Header("accept"), std::string{ "text/event-stream" }, "event-stream accept header");

        auto const completion{ accumulator.Take() };
        check.Equal(completion.id, std::string{ "chatcmpl-s" }, "accumulated id");
        check.Equal(completion.object, std::string{ "chat.completion" }, "accumulated object type");
        if (!check.Equal(completion.choices.size(), std::size_t{ 1 }, "one choice")) {
            return;
        }
        auto const& choice{ completion.choices[0] };
        check.Equal(choice.message.role, std::string{ "assistant" }, "role");
        check.Equal(choice.message.content.value_or(""), std::string{ "Hello" }, "content is concatenated");
        check.Equal(choice.finish_reason, std::string{ "tool_calls" }, "finish reason");
        if (check.Equal(choice.message.tool_calls.size(), std::size_t{ 2 }, "tool calls grouped by index")) {
            auto const* call0{ std::get_if<moai::ChatCompletionMessageFunctionToolCall>(&choice.message.tool_calls[0]) };
            auto const* call1{ std::get_if<moai::ChatCompletionMessageFunctionToolCall>(&choice.message.tool_calls[1]) };
            check.Check(call0 && call0->id == "call_1" && call0->function.name == "lookup" && call0->function.arguments == R"({"a":1})", "first tool call");
            check.Check(call1 && call1->id == "call_2" && call1->function.name == "other" && call1->function.arguments == R"({"b":2})", "second tool call");
        }
        check.Equal(completion.usage.value_or(moai::CompletionUsage{}).total_tokens, std::int64_t{ 9 }, "usage from final chunk");
        check.Check(accumulator.Completion().choices.empty(), "Take resets the accumulator");
    }

    auto test_callback_stop(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server) };
        auto const piece{ event(chunk(R"([{"index":0,"delta":{"content":"a"}}])")) };
        server.Enqueue(ScriptedResponse::Sse({ piece, piece, piece, "data: [DONE]\n\n" }));
        int calls{};
        auto const result{ client.chat.completions.CreateStream(params(), [&](moai::ChatCompletionChunk const&) { return ++calls < 1; }) };
        check.Check(!result && result.error().kind == moai::ErrorKind::Cancelled, "returning false cancels the stream");
        check.Equal(calls, 1, "no chunk is delivered after cancellation");
    }

    auto test_stream_errors(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server) };
        auto const ignore{ [](moai::ChatCompletionChunk const&) { return true; } };
        auto const piece{ event(chunk(R"([{"index":0,"delta":{"content":"a"}}])")) };

        server.Enqueue(ScriptedResponse::Sse({ piece, event(R"({"error":{"message":"overloaded","type":"server_error","code":null}})") }, { { "x-request-id", "req_s" } }));
        auto const mid{ client.chat.completions.CreateStream(params(), ignore) };
        if (check.Check(!mid, "error event fails the stream")) {
            check.Equal(mid.error().kind, moai::ErrorKind::Stream, "error event kind");
            check.Equal(mid.error().message, std::string{ "overloaded" }, "error event message");
            check.Equal(mid.error().type.value_or(""), std::string{ "server_error" }, "error event type");
            check.Equal(mid.error().request_id, std::string{ "req_s" }, "error event request ID");
        }

        server.Enqueue(ScriptedResponse::Json(401, R"({"error":{"message":"Incorrect API key","type":"invalid_request_error","code":"invalid_api_key"}})"));
        auto const unauthorized{ client.chat.completions.CreateStream(params(), ignore) };
        if (check.Check(!unauthorized, "HTTP error fails the stream")) {
            check.Equal(unauthorized.error().kind, moai::ErrorKind::Authentication, "HTTP error kind");
            check.Equal(unauthorized.error().message, std::string{ "Incorrect API key" }, "HTTP error body is preserved");
            check.Equal(unauthorized.error().code.value_or(""), std::string{ "invalid_api_key" }, "HTTP error code");
        }

        server.Enqueue(ScriptedResponse::Json(200, R"({"id":"x"})"));
        auto const not_stream{ client.chat.completions.CreateStream(params(), ignore) };
        check.Check(!not_stream && not_stream.error().kind == moai::ErrorKind::Stream && not_stream.error().message.contains("text/event-stream"), "non-SSE reply is a stream error");

        server.Enqueue(ScriptedResponse::Sse({ piece }));
        auto const truncated{ client.chat.completions.CreateStream(params(), ignore) };
        check.Check(!truncated && truncated.error().kind == moai::ErrorKind::Stream && truncated.error().message.contains("[DONE]"), "missing [DONE] is reported");

        server.Enqueue(ScriptedResponse::Sse({ "data: {not json\n\n", "data: [DONE]\n\n" }));
        auto const invalid{ client.chat.completions.CreateStream(params(), ignore) };
        check.Check(!invalid && invalid.error().kind == moai::ErrorKind::Decode, "invalid event JSON is Decode");

        auto const empty{ client.chat.completions.CreateStream(params(), {}) };
        check.Check(!empty && empty.error().kind == moai::ErrorKind::InvalidArgument, "empty callback is rejected");
    }

    auto test_retry_and_exceptions(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server, 2) };
        auto const piece{ event(chunk(R"([{"index":0,"delta":{"content":"a"}}])")) };
        server.Enqueue(ScriptedResponse::Json(500, R"({"error":{"message":"busy"}})", { { "retry-after-ms", "1" } }));
        server.Enqueue(ScriptedResponse::Sse({ piece, "data: [DONE]\n\n" }));
        int calls{};
        auto const retried{ client.chat.completions.CreateStream(params(), [&](moai::ChatCompletionChunk const&) {
            ++calls;
            return true;
        }) };
        check.Check(retried.has_value(), "stream is retried before the first chunk");
        check.Equal(server.RequestCount(), std::size_t{ 2 }, "two attempts");
        check.Equal(calls, 1, "chunks from the successful attempt only");

        server.Enqueue(ScriptedResponse::Sse({ piece, "data: [DONE]\n\n" }));
        auto threw{ false };
        try {
            (void)client.chat.completions.CreateStream(params(), [](moai::ChatCompletionChunk const&) -> bool { throw std::runtime_error{ "consumer failed" }; });
        } catch (std::runtime_error const& error) {
            threw = std::string_view{ error.what() } == "consumer failed";
        }
        check.Check(threw, "callback exceptions propagate");
        check.Equal(server.RequestCount(), std::size_t{ 3 }, "exceptions are not retried");
    }
} // namespace

auto main() -> int {
    try {
        Checker check;
        test_stream_and_accumulate(check);
        test_callback_stop(check);
        test_stream_errors(check);
        test_retry_and_exceptions(check);
        return check.Finish("test_chat_stream");
    } catch (std::exception const& error) {
        std::println("test_chat_stream: unexpected exception: {}", error.what());
        return 1;
    }
}
