/**
 * @file test_chat_stored.cpp
 * @brief Stored completions: retrieve, update, list, delete, and message listing.
 */
#include "fixtures/mock_server.hpp"
import std;
import moai;

namespace {
    using moai::test::Checker;
    using moai::test::MockServer;
    using moai::test::ScriptedResponse;

    constexpr std::string_view STORED{ R"({"id":"chatcmpl-1","object":"chat.completion","created":1,"model":"m","metadata":{"k":"v"},"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"ok"}}]})" };

    auto test_retrieve_update_delete(Checker& check) -> void {
        MockServer server;
        auto client{ moai::Client::Create({ .api_key = "sk-test", .base_url = server.BaseUrl(), .max_retries = 0 }).value() };
        auto& completions{ client.chat.completions };

        server.Enqueue(ScriptedResponse::Json(200, std::string{ STORED }));
        auto const retrieved{ completions.Retrieve("chatcmpl-1") };
        check.Check(retrieved && retrieved->id == "chatcmpl-1", "retrieve decodes the completion");
        auto request{ server.LastRequest() };
        check.Equal(request.method, std::string{ "GET" }, "retrieve method");
        check.Equal(request.Path(), std::string{ "/v1/chat/completions/chatcmpl-1" }, "retrieve path");
        check.Check(request.body.empty(), "retrieve has no body");

        server.Enqueue(ScriptedResponse::Json(200, std::string{ STORED }));
        (void)completions.Retrieve("a/b c?");
        check.Equal(server.LastRequest().Path(), std::string{ "/v1/chat/completions/a%2Fb%20c%3F" }, "IDs are percent-encoded as one segment");

        server.Enqueue(ScriptedResponse::Json(200, std::string{ STORED }));
        auto const updated{ completions.Update("chatcmpl-1", std::map<std::string, std::string>{ { "k", "v" } }) };
        check.Check(updated.has_value(), "update succeeds");
        request = server.LastRequest();
        check.Equal(request.method, std::string{ "POST" }, "update method");
        check.Equal(request.Path(), std::string{ "/v1/chat/completions/chatcmpl-1" }, "update path");
        check.Equal(moai::Json::parse(request.body).dump(), std::string{ R"({"metadata":{"k":"v"}})" }, "update body");

        server.Enqueue(ScriptedResponse::Json(200, std::string{ STORED }));
        (void)completions.Update("chatcmpl-1", std::nullopt);
        check.Equal(moai::Json::parse(server.LastRequest().body).dump(), std::string{ R"({"metadata":null})" }, "clearing metadata sends null");

        server.Enqueue(ScriptedResponse::Json(200, R"({"id":"chatcmpl-1","object":"chat.completion.deleted","deleted":true})"));
        auto const deleted{ completions.Delete("chatcmpl-1") };
        check.Check(deleted && deleted->deleted && deleted->id == "chatcmpl-1", "delete decodes the result");
        request = server.LastRequest();
        check.Equal(request.method, std::string{ "DELETE" }, "delete method");
        check.Equal(request.Path(), std::string{ "/v1/chat/completions/chatcmpl-1" }, "delete path");

        auto const count{ server.RequestCount() };
        auto const empty{ completions.Delete("") };
        check.Check(!empty && empty.error().kind == moai::ErrorKind::InvalidArgument, "empty ID is rejected");
        check.Equal(server.RequestCount(), count, "rejected request is not sent");

        server.Enqueue(ScriptedResponse::Json(404, R"({"error":{"message":"No completion found","type":"invalid_request_error"}})"));
        auto const missing{ completions.Retrieve("missing") };
        check.Check(!missing && missing.error().kind == moai::ErrorKind::NotFound, "missing completion is NotFound");
    }

    auto test_list(Checker& check) -> void {
        MockServer server;
        auto client{ moai::Client::Create({ .api_key = "sk-test", .base_url = server.BaseUrl(), .max_retries = 0 }).value() };
        server.Enqueue(ScriptedResponse::Json(200, std::format(R"({{"object":"list","data":[{0},{0}],"first_id":"a","last_id":"b","has_more":true}})", STORED)));
        auto const page{ client.chat.completions.List({
            .after = "x",
            .limit = 2,
            .metadata = std::map<std::string, std::string>{ { "k", "v" } },
            .model = "gpt",
            .order = "desc",
        }) };
        if (check.Check(page.has_value(), "list succeeds")) {
            check.Equal(page->data.size(), std::size_t{ 2 }, "two items");
            check.Equal(page->first_id.value_or(""), std::string{ "a" }, "first_id");
            check.Equal(page->last_id.value_or(""), std::string{ "b" }, "last_id");
            check.Check(page->HasNextPage(), "has next page");
        }
        auto const request{ server.LastRequest() };
        check.Equal(request.method, std::string{ "GET" }, "list method");
        check.Equal(request.Path(), std::string{ "/v1/chat/completions" }, "list path");
        check.Equal(request.Query(), std::string{ "after=x&limit=2&metadata%5Bk%5D=v&model=gpt&order=desc" }, "list query");

        server.Enqueue(ScriptedResponse::Json(200, R"({"object":"list","data":[],"has_more":false})"));
        auto const last{ client.chat.completions.List() };
        check.Check(last && last->data.empty() && !last->HasNextPage(), "empty last page");
        check.Check(server.LastRequest().Query().empty(), "no query without parameters");
    }

    auto test_messages(Checker& check) -> void {
        MockServer server;
        auto client{ moai::Client::Create({ .api_key = "sk-test", .base_url = server.BaseUrl(), .max_retries = 0 }).value() };
        server.Enqueue(ScriptedResponse::Json(200, R"({
            "object": "list", "first_id": "m1", "last_id": "m2", "has_more": false,
            "data": [
                {"id": "m1", "role": "user", "content": "Describe", "name": null,
                 "content_parts": [{"type": "text", "text": "Describe"}, {"type": "image_url", "image_url": {"url": "https://e.com/a.png"}}]},
                {"id": "m2", "role": "tool", "content": "result", "tool_call_id": "call_1", "content_parts": null}
            ]
        })"));
        auto const page{ client.chat.completions.messages.List("chatcmpl-1", { .limit = 2, .order = "asc" }) };
        if (check.Check(page.has_value() && page->data.size() == 2, "message list decodes")) {
            auto const& first{ page->data[0] };
            check.Check(first.content_parts && first.content_parts->size() == 2, "content parts");
            auto const* image{ first.content_parts ? std::get_if<moai::ChatCompletionContentPartImage>(&first.content_parts->at(1)) : nullptr };
            check.Check(image != nullptr && image->image_url.url == "https://e.com/a.png", "image part");
            check.Equal(page->data[1].tool_call_id.value_or(""), std::string{ "call_1" }, "tool_call_id");
            check.Check(!page->data[1].content_parts.has_value(), "null content parts");
        }
        auto const request{ server.LastRequest() };
        check.Equal(request.Path(), std::string{ "/v1/chat/completions/chatcmpl-1/messages" }, "messages path");
        check.Equal(request.Query(), std::string{ "limit=2&order=asc" }, "messages query");
    }
} // namespace

auto main() -> int {
    try {
        Checker check;
        test_retrieve_update_delete(check);
        test_list(check);
        test_messages(check);
        return check.Finish("test_chat_stored");
    } catch (std::exception const& error) {
        std::println("test_chat_stored: unexpected exception: {}", error.what());
        return 1;
    }
}
