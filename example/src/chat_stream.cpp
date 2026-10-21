/**
 * @file chat_stream.cpp
 * @brief Stream a chat completion, printing text as it arrives.
 * @note Reads OPENAI_API_KEY and, if set, OPENAI_BASE_URL. Usage: chat_stream [model]
 */
import std;
import moai;

/**
 * @brief Run the example.
 * @return Zero on success.
 */
auto main(int argc, char** argv) -> int {
    std::string const model{ argc > 1 ? argv[1] : "gpt-5-mini" };
    auto client{ moai::Client::Create() };
    if (!client) {
        std::println(std::cerr, "{}", client.error());
        return 1;
    }
    moai::ChatCompletionAccumulator accumulator;
    auto const result{ client->chat.completions.CreateStream(
        {
            .model = model,
            .messages = { moai::user_message("Write a haiku about compilers.") },
            .stream_options = moai::ChatCompletionStreamOptions{ .include_usage = true },
        },
        [&accumulator](moai::ChatCompletionChunk const& chunk) {
            accumulator.Add(chunk);
            for (auto const& choice : chunk.choices) {
                std::print("{}", choice.delta.content.value_or(""));
            }
            std::cout.flush();
            return true;
        }
    ) };
    std::println("");
    if (!result) {
        std::println(std::cerr, "{}", result.error());
        return 1;
    }
    auto const& completion{ accumulator.Completion() };
    if (completion.usage) {
        std::println("tokens: {} total", completion.usage->total_tokens);
    }
    return 0;
}
