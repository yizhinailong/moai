/**
 * @file chat_basic.cpp
 * @brief Send one chat completion request and print the reply.
 * @note Reads OPENAI_API_KEY and, if set, OPENAI_BASE_URL. Usage: chat_basic [model]
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
    auto const completion{ client->chat.completions.Create({
        .model = model,
        .messages = {
            moai::developer_message("Answer in one sentence."),
            moai::user_message("What is a C++ module?"),
        },
    }) };
    if (!completion) {
        std::println(std::cerr, "{}", completion.error());
        return 1;
    }
    for (auto const& choice : completion->choices) {
        std::println("{}", choice.message.content.value_or(choice.message.refusal.value_or("")));
    }
    if (completion->usage) {
        std::println("tokens: {} prompt + {} completion", completion->usage->prompt_tokens, completion->usage->completion_tokens);
    }
    return 0;
}
