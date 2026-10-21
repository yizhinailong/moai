/**
 * @file global_init.cppm
 * @brief Optional process-wide guard for deterministic curl and runtime cleanup.
 */
export module moai:global_init;

import std;
import mcr;
import :error;

export namespace moai {

    /**
     * @brief RAII guard that initializes curl and shuts down coroutine and thread-pool runtimes.
     * @note Create the first guard before other threads use curl, and destroy the last guard outside
     * mcr runtime threads after every client, response, and task has been released. The last guard
     * permanently cleans up the process-wide runtimes; creating another guard afterwards fails.
     * A Client never holds this guard.
     * @code
     * auto guard = moai::GlobalInit::Create();
     * if (!guard) {
     *     std::println("{}", guard.error());
     * }
     * @endcode
     */
    class GlobalInit {
    public:
        /**
         * @brief Acquire a reference to curl's global initialization.
         * @return Guard, or RequestSetup when curl initialization fails or the process-wide
         * resources were already cleaned up.
         */
        [[nodiscard]] static auto Create() -> Result<GlobalInit> {
            auto guard{ mcr::GlobalInit::Create() };
            if (!guard) {
                return std::unexpected{ Error{
                    .kind = ErrorKind::RequestSetup,
                    .message = guard.error().message.empty() ? std::string{ "curl global initialization failed" } : guard.error().message,
                    .transport_code = guard.error().code,
                } };
            }
            return GlobalInit{ std::move(*guard) };
        }

        GlobalInit(GlobalInit const&) = delete;
        auto operator=(GlobalInit const&) -> GlobalInit& = delete;
        GlobalInit(GlobalInit&&) noexcept = default;
        auto operator=(GlobalInit&&) noexcept -> GlobalInit& = default;
        ~GlobalInit() = default;

    private:
        /**
         * @brief Take ownership of an mcr guard after its reference has been counted.
         */
        explicit GlobalInit(mcr::GlobalInit guard) : m_guard{ std::move(guard) } {}

        mcr::GlobalInit m_guard; ///< Underlying reference-counted guard.
    };

} // namespace moai
