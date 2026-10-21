/**
 * @file chat_coro.cpp
 * @brief Run one chat completion through mcr's coroutine runtime.
 * @note Reads OPENAI_API_KEY and, if set, OPENAI_BASE_URL. Usage: chat_coro [model]
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
    // The last guard shuts down coroutine and thread-pool runtimes before curl.
    auto guard{ moai::GlobalInit::Create() };
    if (!guard) {
        std::println(std::cerr, "{}", guard.error());
        return 1;
    }
    auto client{ moai::Client::Create() };
    if (!client) {
        std::println(std::cerr, "{}", client.error());
        return 1;
    }
    auto task{ client->chat.completions.CreateCoro({
        .model = model,
        .messages = { moai::user_message("Name one C++23 feature in one sentence.") },
    }) };
    auto completion{ mcr::sync_wait(std::move(task)) };
    if (!completion) {
        std::println(std::cerr, "{}", completion.error());
        return 1;
    }
    std::println("{}", completion->choices.at(0).message.content.value_or(""));
    return 0;
}
