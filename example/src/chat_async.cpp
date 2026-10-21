/**
 * @file chat_async.cpp
 * @brief Submit two chat completions to mcr's thread pool and print both replies.
 * @note Reads OPENAI_API_KEY and, if set, OPENAI_BASE_URL. Usage: chat_async [model]
 */
import std;
import mcr;
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
    moai::ChatCompletionCreateParams const params{
        .model = model,
        .messages = { moai::user_message("Name one C++23 feature in one sentence.") },
    };
    auto first{ client->chat.completions.CreateAsync(params) };
    auto second{ client->chat.completions.CreateAsync(params) };
    if (!first || !second) {
        std::println(std::cerr, "{}", !first ? first.error() : second.error());
        return 1;
    }
    for (auto* future : { &*first, &*second }) {
        auto completion{ future->get() };
        if (!completion) {
            std::println(std::cerr, "{}", completion.error());
            return 1;
        }
        std::println("{}", completion->choices.at(0).message.content.value_or(""));
    }
    return 0;
}
