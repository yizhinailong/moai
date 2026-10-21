/**
 * @file test_helpers.cpp
 * @brief Structured output and tool argument helpers.
 */
#include "fixtures/mock_server.hpp"
import std;
import moai;

namespace {
    using moai::test::Checker;

    /**
     * @brief Build a one-choice completion with the given finish reason and content.
     */
    auto completion_with(std::optional<std::string> content, std::string finish_reason) -> moai::ChatCompletion {
        moai::ChatCompletion completion;
        completion.choices.push_back(moai::ChatCompletion::Choice{
            .finish_reason = std::move(finish_reason),
            .index = 0,
            .message = moai::ChatCompletionMessage{ .role = "assistant", .content = std::move(content) },
        });
        return completion;
    }

    auto test_structured_output(Checker& check) -> void {
        auto const completion{ completion_with(R"({"answer":42,"tags":["a","b"]})", "stop") };
        auto const parsed{ moai::parse_structured_output(completion) };
        if (check.Check(parsed.has_value(), "valid structured output is parsed")) {
            check.Equal((*parsed)["answer"].get<int>(), 42, "member value");
            check.Equal((*parsed)["tags"].size(), std::size_t{ 2 }, "array member");
        }

        auto const length{ moai::parse_structured_output(completion_with(R"({"answer":42)", "length")) };
        check.Check(!length && length.error().kind == moai::ErrorKind::LengthFinishReason, "length finish reason is reported");
        auto const filtered{ moai::parse_structured_output(completion_with(R"({"answer":42})", "content_filter")) };
        check.Check(!filtered && filtered.error().kind == moai::ErrorKind::ContentFilterFinishReason, "content filter finish reason is reported");

        auto const empty{ moai::parse_structured_output(completion_with(std::nullopt, "stop")) };
        check.Check(!empty && empty.error().kind == moai::ErrorKind::Decode, "missing content is Decode");

        auto const invalid{ moai::parse_structured_output(completion_with("not json", "stop")) };
        if (check.Check(!invalid && invalid.error().kind == moai::ErrorKind::Decode, "invalid JSON is Decode")) {
            check.Check(invalid.error().body.contains("not json"), "offending content is kept in body");
        }

        auto const out_of_range{ moai::parse_structured_output(completion, 1) };
        check.Check(!out_of_range && out_of_range.error().kind == moai::ErrorKind::InvalidArgument, "out-of-range choice is InvalidArgument");

        auto const no_choices{ moai::parse_structured_output(moai::ChatCompletion{}) };
        check.Check(!no_choices && no_choices.error().kind == moai::ErrorKind::InvalidArgument, "completion without choices is InvalidArgument");
    }

    auto test_finish_reason(Checker& check) -> void {
        check.Check(moai::check_finish_reason(completion_with("x", "stop").choices[0]).has_value(), "stop passes");
        check.Check(moai::check_finish_reason(completion_with("x", "tool_calls").choices[0]).has_value(), "tool_calls passes");
        check.Check(moai::check_finish_reason(completion_with("x", "function_call").choices[0]).has_value(), "other reasons pass");

        auto const length{ moai::check_finish_reason(completion_with("x", "length").choices[0]) };
        check.Check(!length && length.error().kind == moai::ErrorKind::LengthFinishReason, "length is rejected");
        check.Check(!length && !length.error().IsStatusError(), "finish-reason errors are not HTTP errors");
        auto const filtered{ moai::check_finish_reason(completion_with("x", "content_filter").choices[0]) };
        check.Check(!filtered && filtered.error().kind == moai::ErrorKind::ContentFilterFinishReason, "content_filter is rejected");
    }

    auto test_tool_arguments(Checker& check) -> void {
        moai::ChatCompletionMessageFunctionToolCall const call{
            .id = "call_1",
            .function = moai::ChatCompletionMessageFunctionToolCall::Function{ .name = "lookup", .arguments = R"({"city":"Paris","days":2})" },
        };
        auto const parsed{ moai::parse_tool_arguments(call) };
        if (check.Check(parsed.has_value(), "valid arguments are parsed")) {
            check.Equal((*parsed)["city"].get<std::string>(), std::string{ "Paris" }, "argument value");
            check.Equal((*parsed)["days"].get<int>(), 2, "numeric argument");
        }

        moai::ChatCompletionMessageFunctionToolCall const truncated{
            .id = "call_2",
            .function = moai::ChatCompletionMessageFunctionToolCall::Function{ .name = "lookup", .arguments = R"({"city":"Par)" },
        };
        auto const invalid{ moai::parse_tool_arguments(truncated) };
        if (check.Check(!invalid && invalid.error().kind == moai::ErrorKind::Decode, "truncated arguments are Decode")) {
            check.Check(invalid.error().message.contains("lookup"), "function name is named in the message");
            check.Check(invalid.error().body.contains("Par"), "raw arguments are kept in body");
        }

        moai::ChatCompletionMessageFunctionToolCall const empty{
            .id = "call_3",
            .function = moai::ChatCompletionMessageFunctionToolCall::Function{ .name = "ping" },
        };
        auto const missing{ moai::parse_tool_arguments(empty) };
        check.Check(!missing && missing.error().kind == moai::ErrorKind::Decode, "empty arguments are Decode");
    }
} // namespace

auto main() -> int {
    try {
        Checker check;
        test_structured_output(check);
        test_finish_reason(check);
        test_tool_arguments(check);
        return check.Finish("test_helpers");
    } catch (std::exception const& error) {
        std::println("test_helpers: unexpected exception: {}", error.what());
        return 1;
    }
}
