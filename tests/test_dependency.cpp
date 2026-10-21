/**
 * @file test_dependency.cpp
 * @brief Verify the moai module and its transitive mcr/curl dependency link and initialize.
 */
import std;
import moai;
import mcr;

/**
 * @brief Initialize an HTTP session without sending a network request.
 * @return Zero on success, otherwise one.
 */
auto main() -> int {
    try {
        auto const runtime{ mcr::GlobalInit::Create() };
        if (!runtime) {
            std::println("mcr initialization: {}", runtime.error().message);
            return 1;
        }
        auto const session{ mcr::Session::Create() };
        if (!session) {
            std::println("mcr session: {}", session.error().message);
            return 1;
        }
        std::println("moai {} / mcr {}: HTTP session ready", moai::VERSION, mcr::VERSION);
        return 0;
    } catch (std::exception const& error) {
        std::println("test_dependency: {}", error.what());
        return 1;
    }
}
