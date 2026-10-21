/**
 * @file test_conversation.cpp
 * @brief Conversation history maintenance, trimming, and JSON round-trips.
 */
#include "fixtures/mock_server.hpp"
import std;
import moai;

namespace {
    using moai::test::Checker;

    /**
     * @brief Build a completion with one assistant choice.
     */
    auto completion_with(std::string content, std::string finish_reason = "stop") -> moai::ChatCompletion {
        moai::ChatCompletion completion;
        completion.id = "chatcmpl-1";
        completion.model = "m";
        completion.choices.push_back(moai::ChatCompletion::Choice{
            .finish_reason = std::move(finish_reason),
            .index = 0,
            .message = moai::ChatCompletionMessage{ .role = "assistant", .content = std::move(content) },
        });
        return completion;
    }

    /**
     * @brief Build an assistant request message that requested one function call.
     */
    auto tool_call_message() -> moai::ChatCompletionMessageParam {
        return moai::ChatCompletionAssistantMessageParam{
            .tool_calls = {
                moai::ChatCompletionMessageFunctionToolCall{
                    .id = "call_1",
                    .function = moai::ChatCompletionMessageFunctionToolCall::Function{ .name = "lookup", .arguments = R"({"q":"x"})" },
                },
            },
        };
    }

    auto test_basic_history(Checker& check) -> void {
        moai::Conversation conversation;
        check.Check(conversation.Empty(), "new conversation is empty");
        check.Equal(conversation.Size(), std::size_t{ 0 }, "new conversation has no messages");

        conversation.Add(moai::system_message("be brief"));
        conversation.Add(moai::user_message("hi"));
        auto const& messages{ conversation.Messages() };
        if (check.Equal(messages.size(), std::size_t{ 2 }, "messages are stored in order")) {
            check.Check(std::holds_alternative<moai::ChatCompletionSystemMessageParam>(messages[0]), "first message is the system message");
            check.Check(std::holds_alternative<moai::ChatCompletionUserMessageParam>(messages[1]), "second message is the user message");
        }

        conversation.Clear();
        check.Check(conversation.Empty(), "Clear removes every message");
    }

    auto test_add_completion(Checker& check) -> void {
        moai::Conversation conversation;
        auto const simple{ completion_with("hello") };
        check.Check(conversation.Add(simple).has_value(), "assistant message is appended");
        if (check.Equal(conversation.Size(), std::size_t{ 1 }, "one message was appended")) {
            auto const* assistant{ std::get_if<moai::ChatCompletionAssistantMessageParam>(&conversation.Messages()[0]) };
            if (check.Check(assistant != nullptr, "appended message is an assistant message")) {
                check.Check(assistant->content.has_value(), "assistant content is kept");
                check.Equal(std::get<std::string>(*assistant->content), std::string{ "hello" }, "assistant text");
            }
        }

        moai::ChatCompletion tool_completion;
        tool_completion.choices.push_back(moai::ChatCompletion::Choice{
            .finish_reason = "tool_calls",
            .index = 0,
            .message = moai::ChatCompletionMessage{
                .role = "assistant",
                .content = std::nullopt,
                .tool_calls = {
                    moai::ChatCompletionMessageFunctionToolCall{
                        .id = "call_1",
                        .function = moai::ChatCompletionMessageFunctionToolCall::Function{ .name = "lookup", .arguments = R"({"q":"x"})" },
                    },
                },
            },
        });
        check.Check(conversation.Add(tool_completion).has_value(), "tool-call message is appended");
        conversation.AddToolResult("call_1", R"({"ok":true})");
        if (check.Equal(conversation.Size(), std::size_t{ 3 }, "tool result is appended")) {
            auto const* tool{ std::get_if<moai::ChatCompletionToolMessageParam>(&conversation.Messages()[2]) };
            if (check.Check(tool != nullptr, "tool result has the tool role")) {
                check.Equal(tool->tool_call_id, std::string{ "call_1" }, "tool result references the call");
                check.Equal(std::get<std::string>(tool->content), std::string{ R"({"ok":true})" }, "tool output is kept");
            }
        }

        auto const out_of_range{ conversation.Add(simple, 1) };
        check.Check(!out_of_range && out_of_range.error().kind == moai::ErrorKind::InvalidArgument, "out-of-range choice index is rejected");
        check.Equal(conversation.Size(), std::size_t{ 3 }, "rejected completion does not change the history");
    }

    auto test_trimming(Checker& check) -> void {
        moai::Conversation conversation;
        conversation.Add(moai::developer_message("instructions"));
        conversation.Add(moai::user_message("first"));
        conversation.Add(moai::assistant_message("answer"));
        conversation.Add(moai::user_message("second"));
        conversation.SetMessageLimit(3);
        if (check.Equal(conversation.Size(), std::size_t{ 3 }, "limit trims to three messages")) {
            check.Check(std::holds_alternative<moai::ChatCompletionDeveloperMessageParam>(conversation.Messages()[0]), "developer message is preserved");
            check.Check(std::holds_alternative<moai::ChatCompletionAssistantMessageParam>(conversation.Messages()[1]), "second-oldest turn remains");
        }

        conversation.SetMessageLimit(1);
        if (check.Equal(conversation.Size(), std::size_t{ 1 }, "trimming stops at the instructions")) {
            check.Check(std::holds_alternative<moai::ChatCompletionDeveloperMessageParam>(conversation.Messages()[0]), "instructions survive further trimming");
        }
        conversation.Add(moai::user_message("third"));
        check.Equal(conversation.Size(), std::size_t{ 1 }, "a conversation above the limit keeps only instructions");
        check.Check(conversation.MessageLimit() == std::optional<std::size_t>{ 1 }, "limit is reported");
    }

    auto test_tool_turn_trimming(Checker& check) -> void {
        moai::Conversation conversation;
        conversation.Add(moai::system_message("sys"));
        conversation.Add(moai::user_message("use the tool"));
        conversation.Add(tool_call_message());
        conversation.AddToolResult("call_1", "result");
        conversation.Add(moai::user_message("thanks"));
        conversation.SetMessageLimit(4);
        check.Equal(conversation.Size(), std::size_t{ 4 }, "oldest user message is trimmed first");
        check.Check(std::holds_alternative<moai::ChatCompletionAssistantMessageParam>(conversation.Messages()[1]), "tool-call assistant remains");

        conversation.SetMessageLimit(2);
        check.Equal(conversation.Size(), std::size_t{ 2 }, "tool turn is removed together");
        check.Check(std::holds_alternative<moai::ChatCompletionSystemMessageParam>(conversation.Messages()[0]), "system message remains");
        check.Check(
            std::holds_alternative<moai::ChatCompletionUserMessageParam>(conversation.Messages()[1]),
            "no orphaned tool message is left behind"
        );
    }

    auto test_json_round_trip(Checker& check) -> void {
        moai::Conversation conversation;
        conversation.Add(moai::developer_message("instructions"));
        conversation.Add(moai::user_message(std::vector<moai::ChatCompletionContentPart>{
            moai::ChatCompletionContentPartText{ .text = "look" },
            moai::ChatCompletionContentPartImage{ .image_url = { .url = "https://example.com/x.png", .detail = "high" } },
        }));
        conversation.Add(tool_call_message());
        conversation.AddToolResult("call_1", R"({"ok":true})");

        auto const json = conversation.ToJson();
        if (!check.Check(json.is_array() && json.size() == 4, "export is an array of four messages")) {
            return;
        }
        check.Equal(json[0]["role"].get<std::string>(), std::string{ "developer" }, "developer role is encoded");
        check.Equal(json[1]["content"][1]["type"].get<std::string>(), std::string{ "image_url" }, "multimodal content keeps its type");
        check.Equal(json[2]["tool_calls"][0]["function"]["name"].get<std::string>(), std::string{ "lookup" }, "tool call is encoded");
        check.Equal(json[3]["tool_call_id"].get<std::string>(), std::string{ "call_1" }, "tool result references the call");

        auto const restored{ moai::Conversation::FromJson(json) };
        if (check.Check(restored.has_value(), "exported history can be imported")) {
            check.Equal(restored->ToJson(), json, "import/export round-trip is stable");
            check.Equal(restored->Size(), std::size_t{ 4 }, "imported history has four messages");
        }

        auto const not_an_array{ moai::Conversation::FromJson(moai::Json::object()) };
        check.Check(!not_an_array && not_an_array.error().kind == moai::ErrorKind::Decode, "non-array history is Decode");
        auto const unknown_role{ moai::Conversation::FromJson(moai::Json::parse(R"([{"role":"narrator","content":"hi"}])")) };
        check.Check(!unknown_role && unknown_role.error().kind == moai::ErrorKind::Decode, "unknown role is Decode");
        check.Check(unknown_role.error().message.contains("narrator"), "unknown role is named in the message");
        auto const missing_content{ moai::Conversation::FromJson(moai::Json::parse(R"([{"role":"user"}])")) };
        check.Check(!missing_content && missing_content.error().kind == moai::ErrorKind::Decode, "missing content is Decode");
        auto const bad_part{ moai::Conversation::FromJson(moai::Json::parse(R"([{"role":"user","content":[{"type":"video"}]}])")) };
        check.Check(!bad_part && bad_part.error().kind == moai::ErrorKind::Decode, "unsupported content part is Decode");
    }
} // namespace

auto main() -> int {
    try {
        Checker check;
        test_basic_history(check);
        test_add_completion(check);
        test_trimming(check);
        test_tool_turn_trimming(check);
        test_json_round_trip(check);
        return check.Finish("test_conversation");
    } catch (std::exception const& error) {
        std::println("test_conversation: unexpected exception: {}", error.what());
        return 1;
    }
}
