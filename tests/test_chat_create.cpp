/**
 * @file test_chat_create.cpp
 * @brief Chat Completions create: request serialization, response decoding, and HTTP error mapping.
 */
#include "fixtures/mock_server.hpp"
import std;
import moai;

namespace {
    using moai::test::Checker;
    using moai::test::MockServer;
    using moai::test::ScriptedResponse;

    constexpr std::string_view MINIMAL_COMPLETION{ R"({"id":"chatcmpl-1","object":"chat.completion","created":1,"model":"m","choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"ok"}}]})" };

    auto make_client(MockServer const& server, int max_retries = 0) -> moai::Client {
        return moai::Client::Create({ .api_key = "sk-test", .base_url = server.BaseUrl(), .max_retries = max_retries }).value();
    }

    auto test_full_serialization(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server) };
        server.Enqueue(ScriptedResponse::Json(200, std::string{ MINIMAL_COMPLETION }));
        moai::ChatCompletionCreateParams const params{
            .model = "gpt-test",
            .messages = {
                moai::developer_message("Be brief."),
                moai::ChatCompletionSystemMessageParam{ .content = std::vector<moai::ChatCompletionContentPartText>{ { .text = "sys" } }, .name = "ops" },
                moai::ChatCompletionUserMessageParam{ .content = std::vector<moai::ChatCompletionContentPart>{
                                                          moai::ChatCompletionContentPartText{ .text = "Describe" },
                                                          moai::ChatCompletionContentPartImage{ .image_url = { .url = "https://example.com/a.png", .detail = "high" } },
                                                          moai::ChatCompletionContentPartInputAudio{ .input_audio = { .data = "AAAA", .format = "wav" } },
                                                          moai::ChatCompletionContentPartFile{ .file = { .file_id = "file-1" } },
                                                      } },
                moai::ChatCompletionAssistantMessageParam{
                    .tool_calls = {
                        moai::ChatCompletionMessageFunctionToolCall{ .id = "call_1", .function = { .name = "lookup", .arguments = R"({"q":"x"})" } },
                        moai::ChatCompletionMessageCustomToolCall{ .id = "call_2", .custom = { .name = "sql", .input = "SELECT 1" } },
                    },
                    .audio = moai::ChatCompletionAssistantMessageParam::Audio{ .id = "audio_1" },
                },
                moai::tool_message("call_1", "result"),
            },
            .audio = moai::ChatCompletionAudioParam{ .format = "wav", .voice = "alloy" },
            .frequency_penalty = 0.5,
            .logit_bias = std::map<std::string, int>{ { "50256", -100 } },
            .logprobs = true,
            .max_completion_tokens = 256,
            .max_tokens = 128,
            .metadata = std::map<std::string, std::string>{ { "team", "core" } },
            .modalities = std::vector<std::string>{ "text", "audio" },
            .n = 2,
            .parallel_tool_calls = false,
            .prediction = moai::ChatCompletionPredictionContent{ .content = "draft" },
            .presence_penalty = -0.5,
            .prompt_cache_key = "cache-1",
            .reasoning_effort = "low",
            .response_format = moai::ResponseFormatJsonSchema{ .json_schema = { .name = "answer", .schema = moai::Json{ { "type", "object" } }, .strict = true } },
            .safety_identifier = "user-hash",
            .seed = 7,
            .service_tier = "flex",
            .stop = std::vector<std::string>{ "END" },
            .store = true,
            .temperature = 0.25,
            .tool_choice = moai::ChatCompletionNamedToolChoice{ .function = { .name = "lookup" } },
            .tools = {
                moai::ChatCompletionFunctionTool{ .function = { .name = "lookup", .description = "Find", .parameters = moai::Json{ { "type", "object" } }, .strict = true } },
                moai::ChatCompletionCustomTool{ .custom = { .name = "sql", .format = moai::CustomToolInputFormatGrammar{ .grammar = { .definition = R"(start: "SELECT")", .syntax = "lark" } } } },
            },
            .top_logprobs = 3,
            .top_p = 0.9,
            .user = "legacy",
            .verbosity = "low",
            .web_search_options = moai::WebSearchOptions{ .search_context_size = "low", .user_location = moai::WebSearchOptions::UserLocation{ .approximate = { .country = "CN" } } },
        };
        auto const result{ client.chat.completions.Create(params) };
        check.Check(result.has_value(), "create with every parameter succeeds");

        auto const expected = moai::Json::parse(R"json({
            "model": "gpt-test",
            "messages": [
                {"role": "developer", "content": "Be brief."},
                {"role": "system", "content": [{"type": "text", "text": "sys"}], "name": "ops"},
                {"role": "user", "content": [
                    {"type": "text", "text": "Describe"},
                    {"type": "image_url", "image_url": {"url": "https://example.com/a.png", "detail": "high"}},
                    {"type": "input_audio", "input_audio": {"data": "AAAA", "format": "wav"}},
                    {"type": "file", "file": {"file_id": "file-1"}}
                ]},
                {"role": "assistant", "tool_calls": [
                    {"id": "call_1", "type": "function", "function": {"name": "lookup", "arguments": "{\"q\":\"x\"}"}},
                    {"id": "call_2", "type": "custom", "custom": {"name": "sql", "input": "SELECT 1"}}
                ], "audio": {"id": "audio_1"}},
                {"role": "tool", "content": "result", "tool_call_id": "call_1"}
            ],
            "audio": {"format": "wav", "voice": "alloy"},
            "frequency_penalty": 0.5,
            "logit_bias": {"50256": -100},
            "logprobs": true,
            "max_completion_tokens": 256,
            "max_tokens": 128,
            "metadata": {"team": "core"},
            "modalities": ["text", "audio"],
            "n": 2,
            "parallel_tool_calls": false,
            "prediction": {"type": "content", "content": "draft"},
            "presence_penalty": -0.5,
            "prompt_cache_key": "cache-1",
            "reasoning_effort": "low",
            "response_format": {"type": "json_schema", "json_schema": {"name": "answer", "schema": {"type": "object"}, "strict": true}},
            "safety_identifier": "user-hash",
            "seed": 7,
            "service_tier": "flex",
            "stop": ["END"],
            "store": true,
            "temperature": 0.25,
            "tool_choice": {"type": "function", "function": {"name": "lookup"}},
            "tools": [
                {"type": "function", "function": {"name": "lookup", "description": "Find", "parameters": {"type": "object"}, "strict": true}},
                {"type": "custom", "custom": {"name": "sql", "format": {"type": "grammar", "grammar": {"definition": "start: \"SELECT\"", "syntax": "lark"}}}}
            ],
            "top_logprobs": 3,
            "top_p": 0.9,
            "user": "legacy",
            "verbosity": "low",
            "web_search_options": {"search_context_size": "low", "user_location": {"type": "approximate", "approximate": {"country": "CN"}}}
        })json");
        auto const actual = moai::Json::parse(server.LastRequest().body);
        check.Equal(actual.dump(), expected.dump(), "serialized request body");
    }

    auto test_minimal_serialization(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server) };
        server.Enqueue(ScriptedResponse::Json(200, std::string{ MINIMAL_COMPLETION }));
        server.Enqueue(ScriptedResponse::Json(200, std::string{ MINIMAL_COMPLETION }));
        auto const minimal{ client.chat.completions.Create({ .model = "m", .messages = { moai::user_message("hi") } }) };
        check.Check(minimal.has_value(), "minimal create succeeds");
        check.Equal(
            moai::Json::parse(server.LastRequest().body).dump(),
            std::string{ R"({"messages":[{"content":"hi","role":"user"}],"model":"m"})" },
            "unset optional fields are omitted"
        );

        auto const variants{ client.chat.completions.Create({
            .model = "m",
            .messages = { moai::assistant_message("prev"), moai::user_message({ moai::ChatCompletionContentPartText{ .text = "x" } }) },
            .response_format = moai::ResponseFormatJsonObject{},
            .stop = "STOP",
            .tool_choice = "auto",
        }) };
        check.Check(variants.has_value(), "create with string variants succeeds");
        auto const body = moai::Json::parse(server.LastRequest().body);
        check.Equal(body["stop"].dump(), std::string{ R"("STOP")" }, "single stop string");
        check.Equal(body["tool_choice"].dump(), std::string{ R"("auto")" }, "tool_choice mode string");
        check.Equal(body["response_format"].dump(), std::string{ R"({"type":"json_object"})" }, "json_object format");
        check.Equal(body["messages"][0].dump(), std::string{ R"({"content":"prev","role":"assistant"})" }, "assistant text message");
        check.Check(!body.contains("stream"), "Create never sends stream");
    }

    auto test_decoding(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server) };
        server.Enqueue(ScriptedResponse::Json(200, R"({
            "id": "chatcmpl-1", "object": "chat.completion", "created": 1700000000, "model": "gpt-test",
            "service_tier": "default", "system_fingerprint": "fp_1", "extra_field": {"nested": true},
            "choices": [
                {"index": 0, "finish_reason": "tool_calls",
                 "logprobs": {"content": [{"token": "Hi", "logprob": -0.5, "bytes": [72, 105], "top_logprobs": [{"token": "Hi", "logprob": -0.5, "bytes": null}]}], "refusal": null},
                 "message": {"role": "assistant", "content": null, "refusal": null,
                    "annotations": [{"type": "url_citation", "url_citation": {"start_index": 1, "end_index": 5, "title": "T", "url": "https://e.com"}}],
                    "audio": {"id": "audio_1", "data": "UklG", "expires_at": 1700003600, "transcript": "hi"},
                    "tool_calls": [
                        {"id": "call_1", "type": "function", "function": {"name": "lookup", "arguments": "{}"}},
                        {"id": "call_2", "type": "custom", "custom": {"name": "sql", "input": "SELECT 1"}}
                    ]}},
                {"index": 1, "finish_reason": "stop", "logprobs": null, "message": {"role": "assistant", "content": "Hello", "unknown": 1}}
            ],
            "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15,
                "prompt_tokens_details": {"cached_tokens": 4, "audio_tokens": 0},
                "completion_tokens_details": {"reasoning_tokens": 2, "audio_tokens": 0, "accepted_prediction_tokens": 0, "rejected_prediction_tokens": 1}}
        })",
                                              { { "x-request-id", "req_ok" } }));
        auto const result{ client.chat.completions.Create({ .model = "m", .messages = { moai::user_message("hi") } }) };
        if (!check.Check(result.has_value(), "decode full completion")) {
            std::println("  error: {}", result.error());
            return;
        }
        auto const& completion{ *result };
        check.Equal(completion.id, std::string{ "chatcmpl-1" }, "id");
        check.Equal(completion.created, std::int64_t{ 1700000000 }, "created");
        check.Equal(completion.service_tier.value_or(""), std::string{ "default" }, "service tier");
        check.Equal(completion.system_fingerprint.value_or(""), std::string{ "fp_1" }, "fingerprint");
        check.Check(completion.raw.contains("extra_field"), "raw keeps unmodelled fields");
        if (!check.Equal(completion.choices.size(), std::size_t{ 2 }, "two choices")) {
            return;
        }
        auto const& first{ completion.choices[0] };
        check.Equal(first.finish_reason, std::string{ "tool_calls" }, "finish reason");
        check.Check(!first.message.content.has_value(), "null content stays empty");
        check.Equal(first.message.annotations.size(), std::size_t{ 1 }, "annotation");
        check.Equal(first.message.annotations.at(0).url_citation.end_index, std::int64_t{ 5 }, "annotation end index");
        check.Equal(first.message.audio.value_or(moai::ChatCompletionAudio{}).transcript, std::string{ "hi" }, "audio transcript");
        if (check.Equal(first.message.tool_calls.size(), std::size_t{ 2 }, "tool calls")) {
            auto const* function{ std::get_if<moai::ChatCompletionMessageFunctionToolCall>(&first.message.tool_calls[0]) };
            check.Check(function != nullptr && function->function.name == "lookup" && function->id == "call_1", "function tool call");
            auto const* custom{ std::get_if<moai::ChatCompletionMessageCustomToolCall>(&first.message.tool_calls[1]) };
            check.Check(custom != nullptr && custom->custom.input == "SELECT 1", "custom tool call");
        }
        auto const& logprobs{ first.logprobs.value_or(moai::ChatCompletionChoiceLogprobs{}) };
        check.Check(logprobs.content && logprobs.content->size() == 1 && logprobs.content->at(0).bytes == std::vector<std::int64_t>{ 72, 105 }, "token logprob bytes");
        check.Check(logprobs.content && !logprobs.content->at(0).top_logprobs.at(0).bytes.has_value(), "null bytes stay empty");
        check.Check(!logprobs.refusal.has_value(), "null refusal logprobs");
        check.Equal(completion.choices[1].message.content.value_or(""), std::string{ "Hello" }, "second choice content");
        auto const usage{ completion.usage.value_or(moai::CompletionUsage{}) };
        check.Equal(usage.total_tokens, std::int64_t{ 15 }, "total tokens");
        check.Equal(usage.prompt_tokens_details.value_or(moai::CompletionUsage::PromptTokensDetails{}).cached_tokens.value_or(-1), std::int64_t{ 4 }, "cached tokens");
        check.Equal(usage.completion_tokens_details.value_or(moai::CompletionUsage::CompletionTokensDetails{}).reasoning_tokens.value_or(-1), std::int64_t{ 2 }, "reasoning tokens");

        auto const param{ moai::assistant_message(first.message) };
        auto const* assistant{ std::get_if<moai::ChatCompletionAssistantMessageParam>(&param) };
        check.Check(assistant != nullptr && assistant->tool_calls.size() == 2 && !assistant->content && assistant->audio && assistant->audio->id == "audio_1", "assistant_message keeps tool calls and audio");
    }

    auto test_decode_errors(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server) };
        server.Enqueue(ScriptedResponse::Json(200, R"({"id": "x", "choices": "oops"})", { { "x-request-id", "req_bad" } }));
        server.Enqueue(ScriptedResponse::Json(200, "not json"));
        server.Enqueue(ScriptedResponse::Json(200, R"({"choices": [{"index": 0, "message": {"tool_calls": [{"id": "c", "type": "future"}]}}]})"));
        moai::ChatCompletionCreateParams const params{ .model = "m", .messages = { moai::user_message("hi") } };

        auto const wrong_type{ client.chat.completions.Create(params) };
        if (check.Check(!wrong_type, "type mismatch fails")) {
            check.Equal(wrong_type.error().kind, moai::ErrorKind::Decode, "type mismatch is Decode");
            check.Check(wrong_type.error().message.contains("choices"), "message names the field");
            check.Equal(wrong_type.error().request_id, std::string{ "req_bad" }, "decode error keeps the request ID");
        }
        auto const invalid{ client.chat.completions.Create(params) };
        check.Check(!invalid && invalid.error().kind == moai::ErrorKind::Decode, "invalid JSON is Decode");
        auto const unknown{ client.chat.completions.Create(params) };
        check.Check(!unknown && unknown.error().message.contains("future"), "unknown tool call type is reported");
    }

    auto test_status_errors(Checker& check) -> void {
        MockServer server;
        auto client{ make_client(server) };
        std::vector<std::pair<int, moai::ErrorKind>> const cases{
            { 400, moai::ErrorKind::BadRequest },
            { 401, moai::ErrorKind::Authentication },
            { 403, moai::ErrorKind::PermissionDenied },
            { 404, moai::ErrorKind::NotFound },
            { 409, moai::ErrorKind::Conflict },
            { 422, moai::ErrorKind::UnprocessableEntity },
            { 429, moai::ErrorKind::RateLimit },
            { 500, moai::ErrorKind::InternalServer },
            { 503, moai::ErrorKind::InternalServer },
            { 418, moai::ErrorKind::Status },
        };
        moai::ChatCompletionCreateParams const params{ .model = "m", .messages = { moai::user_message("hi") } };
        for (auto const& [status, kind] : cases) {
            server.Enqueue(ScriptedResponse::Json(status, R"({"error": {"message": "failure", "type": "invalid_request_error", "code": "bad_value", "param": "model"}})", { { "x-request-id", "req_err" } }));
            auto const result{ client.chat.completions.Create(params) };
            if (!check.Check(!result, std::format("HTTP {} fails", status))) {
                continue;
            }
            auto const& error{ result.error() };
            check.Equal(error.kind, kind, std::format("HTTP {} kind", status));
            check.Equal(error.status_code, static_cast<long>(status), "status code");
            check.Equal(error.message, std::string{ "failure" }, "API message");
            check.Equal(error.type.value_or(""), std::string{ "invalid_request_error" }, "API type");
            check.Equal(error.code.value_or(""), std::string{ "bad_value" }, "API code");
            check.Equal(error.param.value_or(""), std::string{ "model" }, "API param");
            check.Equal(error.request_id, std::string{ "req_err" }, "request ID");
            check.Check(error.IsStatusError(), "status error predicate");
        }
        check.Equal(server.RequestCount(), cases.size(), "no retries with max_retries = 0");

        server.Enqueue(ScriptedResponse::Json(404, R"({"detail": "Not found"})"));
        auto const detail{ client.chat.completions.Create(params) };
        check.Check(!detail && detail.error().message == "Not found", "detail body is used as message");

        server.Enqueue(ScriptedResponse{ .status = 502, .headers = { { "Content-Type", "text/plain" } }, .chunks = { "Bad gateway" } });
        auto const plain{ client.chat.completions.Create(params) };
        if (check.Check(!plain, "plain-text error fails")) {
            check.Equal(plain.error().message, std::string{ "HTTP 502: Bad gateway" }, "plain-text error message");
            check.Equal(plain.error().body, std::string{ "Bad gateway" }, "raw error body");
        }
    }
} // namespace

auto main() -> int {
    try {
        Checker check;
        test_full_serialization(check);
        test_minimal_serialization(check);
        test_decoding(check);
        test_decode_errors(check);
        test_status_errors(check);
        return check.Finish("test_chat_create");
    } catch (std::exception const& error) {
        std::println("test_chat_create: unexpected exception: {}", error.what());
        return 1;
    }
}
