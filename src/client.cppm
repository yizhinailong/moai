/**
 * @file client.cppm
 * @brief Unified OpenAI client combining configuration, transport, and resource components.
 */
export module moai:client;

import std;
import :chat_completions;
import :config;
import :error;
import :transport;

export namespace moai {

    /**
     * @brief Entry point of the SDK, equivalent to `OpenAI` in the official Python SDK.
     * @note Configuration is fixed at creation. Copies share the same configuration and connection
     * pool, and all resources may be used concurrently from several threads. A client never shuts
     * down libcurl; an optional `mcr::GlobalInit` guard at the start of `main()` gives deterministic
     * cleanup and must outlive every client and in-flight request.
     *
     * @code
     * auto client = moai::Client::Create();  // reads OPENAI_API_KEY
     * if (!client) {
     *     std::println("{}", client.error());
     * }
     * auto completion = client->chat.completions.Create({
     *     .model = "gpt-5",
     *     .messages = { moai::user_message("Hello") },
     * });
     * @endcode
     */
    class Client {
    public:
        /**
         * @brief Create a client.
         * @param options Configuration; unset credentials and base URL fall back to environment variables.
         * @return Client, or InvalidArgument when no API key is available or an option is invalid.
         */
        [[nodiscard]] static auto Create(ClientOptions options = {}) -> Result<Client> {
            auto core{ detail::ClientCore::Create(std::move(options)) };
            if (!core) {
                return std::unexpected{ std::move(core.error()) };
            }
            return Client{ std::move(*core) };
        }

        Chat chat; ///< Chat resources, such as `chat.completions`.

    private:
        /**
         * @brief Attach all resources to the shared state.
         */
        explicit Client(std::shared_ptr<detail::ClientCore const> core) : chat{ std::move(core) } {}
    };

} // namespace moai
