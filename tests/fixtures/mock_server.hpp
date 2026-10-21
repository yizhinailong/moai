/**
 * @file mock_server.hpp
 * @brief Scriptable loopback HTTP server and assertion helpers for moai tests.
 * @note Responses are served in FIFO order from a script; every request is recorded. The server
 * binds an ephemeral port on 127.0.0.1, so tests need no external network or API key.
 */
#pragma once
#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

import std;

namespace moai::test {

#ifdef _WIN32
    using NativeSocket = SOCKET;
    using SocketLength = int;
    inline constexpr NativeSocket INVALID_NATIVE_SOCKET{ INVALID_SOCKET };
#else
    using NativeSocket = int;
    using SocketLength = socklen_t;
    inline constexpr NativeSocket INVALID_NATIVE_SOCKET{ -1 };
#endif

    /**
     * @brief Close a socket on every exit path.
     */
    class Socket {
    public:
        explicit Socket(NativeSocket handle) : m_handle{ handle } {}

        Socket(Socket const&) = delete;
        auto operator=(Socket const&) -> Socket& = delete;

        ~Socket() { Close(); }

        [[nodiscard]] auto Handle() const noexcept -> NativeSocket { return m_handle; }

        auto Close() noexcept -> void {
            if (m_handle != INVALID_NATIVE_SOCKET) {
#ifdef _WIN32
                closesocket(m_handle);
#else
                close(m_handle);
#endif
                m_handle = INVALID_NATIVE_SOCKET;
            }
        }

    private:
        NativeSocket m_handle;
    };

    /**
     * @brief Wait up to 20 ms for a socket to become readable so loops can observe stop requests.
     */
    inline auto readable(NativeSocket socket) -> bool {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(socket, &set);
        timeval timeout{ 0, 20'000 };
#ifdef _WIN32
        auto const result{ select(0, &set, nullptr, nullptr, &timeout) };
#else
        auto const result{ select(socket + 1, &set, nullptr, nullptr, &timeout) };
#endif
        return result > 0;
    }

    /**
     * @brief Send all bytes; returns false when the peer has gone away.
     */
    inline auto send_all(NativeSocket socket, std::string_view bytes) -> bool {
        while (!bytes.empty()) {
#ifdef MSG_NOSIGNAL
            constexpr int FLAGS{ MSG_NOSIGNAL };
#else
            constexpr int FLAGS{ 0 };
#endif
            auto const sent{ send(socket, bytes.data(), static_cast<int>(bytes.size()), FLAGS) };
            if (sent <= 0) {
                return false;
            }
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        }
        return true;
    }

    /**
     * @brief Lowercase ASCII text.
     */
    inline auto lowercase(std::string_view text) -> std::string {
        std::string result{ text };
        for (auto& character : result) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        return result;
    }

    /**
     * @brief Request received by the server.
     */
    struct RecordedRequest {
        std::string method{};                         ///< Request method.
        std::string target{};                         ///< Raw request target, including the query.
        std::map<std::string, std::string> headers{}; ///< Headers with lowercase names.
        std::string body{};                           ///< Request body.

        /**
         * @brief Return a header value, or an empty string.
         */
        [[nodiscard]] auto Header(std::string_view name) const -> std::string {
            auto const found{ headers.find(lowercase(name)) };
            return found == headers.end() ? std::string{} : found->second;
        }

        /**
         * @brief Return the path without the query.
         */
        [[nodiscard]] auto Path() const -> std::string { return target.substr(0, target.find('?')); }

        /**
         * @brief Return the raw query without '?', or an empty string.
         */
        [[nodiscard]] auto Query() const -> std::string {
            auto const mark{ target.find('?') };
            return mark == std::string::npos ? std::string{} : target.substr(mark + 1);
        }
    };

    /**
     * @brief Scripted reply to one request.
     */
    struct ScriptedResponse {
        int status{ 200 };                                          ///< HTTP status.
        std::vector<std::pair<std::string, std::string>> headers{}; ///< Extra headers.
        std::vector<std::string> chunks{};                          ///< Body pieces.
        bool chunked{};                                             ///< Use chunked transfer encoding, flushing each piece.
        std::chrono::milliseconds delay{};                          ///< Wait before replying.
        std::chrono::milliseconds chunk_delay{};                    ///< Wait between chunked pieces.
        bool drop{};                                                ///< Close the connection without replying.

        /**
         * @brief JSON reply.
         */
        [[nodiscard]] static auto Json(int status, std::string body, std::vector<std::pair<std::string, std::string>> headers = {}) -> ScriptedResponse {
            headers.emplace_back("Content-Type", "application/json");
            return ScriptedResponse{ .status = status, .headers = std::move(headers), .chunks = { std::move(body) } };
        }

        /**
         * @brief Event-stream reply; each element is sent as its own chunk.
         */
        [[nodiscard]] static auto Sse(std::vector<std::string> pieces, std::vector<std::pair<std::string, std::string>> headers = {}) -> ScriptedResponse {
            headers.emplace_back("Content-Type", "text/event-stream");
            return ScriptedResponse{ .status = 200, .headers = std::move(headers), .chunks = std::move(pieces), .chunked = true };
        }
    };

    /**
     * @brief Loopback HTTP/1.1 server with keep-alive connections.
     */
    class MockServer {
    public:
        MockServer() {
#ifdef _WIN32
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
                throw std::runtime_error{ "WSAStartup failed" };
            }
#endif
            m_listener = std::make_unique<Socket>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (m_listener->Handle() == INVALID_NATIVE_SOCKET) {
                throw std::runtime_error{ "socket failed" };
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(m_listener->Handle(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(m_listener->Handle(), 16) != 0) {
                throw std::runtime_error{ "bind/listen failed" };
            }
            SocketLength length{ sizeof(address) };
            if (getsockname(m_listener->Handle(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
                throw std::runtime_error{ "getsockname failed" };
            }
            m_port = ntohs(address.sin_port);
            m_acceptor = std::jthread{ [this](std::stop_token stop) { acceptLoop(stop); } };
        }

        MockServer(MockServer const&) = delete;
        auto operator=(MockServer const&) -> MockServer& = delete;

        ~MockServer() {
            m_acceptor.request_stop();
            if (m_acceptor.joinable()) {
                m_acceptor.join();
            }
            {
                std::scoped_lock lock{ m_mutex };
                for (auto& client : m_clients) {
                    client.request_stop();
                }
            }
            m_clients.clear();
            m_listener.reset();
#ifdef _WIN32
            WSACleanup();
#endif
        }

        /**
         * @brief Base URL to use as ClientOptions::base_url.
         */
        [[nodiscard]] auto BaseUrl() const -> std::string { return std::format("http://127.0.0.1:{}/v1", m_port); }

        /**
         * @brief Append a reply to the script.
         */
        auto Enqueue(ScriptedResponse response) -> void {
            std::scoped_lock lock{ m_mutex };
            m_script.push_back(std::move(response));
        }

        /**
         * @brief Return a copy of every recorded request.
         */
        [[nodiscard]] auto Requests() const -> std::vector<RecordedRequest> {
            std::scoped_lock lock{ m_mutex };
            return m_requests;
        }

        /**
         * @brief Return the number of recorded requests.
         */
        [[nodiscard]] auto RequestCount() const -> std::size_t {
            std::scoped_lock lock{ m_mutex };
            return m_requests.size();
        }

        /**
         * @brief Return the most recent request; throws when none was received.
         */
        [[nodiscard]] auto LastRequest() const -> RecordedRequest {
            std::scoped_lock lock{ m_mutex };
            if (m_requests.empty()) {
                throw std::runtime_error{ "no request recorded" };
            }
            return m_requests.back();
        }

    private:
        auto acceptLoop(std::stop_token const& stop) -> void {
            while (!stop.stop_requested()) {
                if (!readable(m_listener->Handle())) {
                    continue;
                }
                auto client{ std::make_shared<Socket>(accept(m_listener->Handle(), nullptr, nullptr)) };
                if (client->Handle() == INVALID_NATIVE_SOCKET) {
                    continue;
                }
                std::scoped_lock lock{ m_mutex };
                m_clients.emplace_back([this, client](std::stop_token client_stop) { serve(client, client_stop); });
            }
        }

        /**
         * @brief Receive more bytes; returns false on close or stop.
         */
        static auto receive(NativeSocket socket, std::string& pending, std::stop_token const& stop) -> bool {
            while (!stop.stop_requested()) {
                if (!readable(socket)) {
                    continue;
                }
                std::array<char, 4096> buffer{};
                auto const count{ recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0) };
                if (count <= 0) {
                    return false;
                }
                pending.append(buffer.data(), static_cast<std::size_t>(count));
                return true;
            }
            return false;
        }

        /**
         * @brief Sleep in small steps so teardown is not delayed.
         */
        static auto pause(std::chrono::milliseconds duration, std::stop_token const& stop) -> void {
            auto const deadline{ std::chrono::steady_clock::now() + duration };
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
            }
        }

        auto serve(std::shared_ptr<Socket> const& client, std::stop_token const& stop) -> void {
            std::string pending;
            while (!stop.stop_requested()) {
                std::size_t head_end{};
                while ((head_end = pending.find("\r\n\r\n")) == std::string::npos) {
                    if (!receive(client->Handle(), pending, stop)) {
                        return;
                    }
                }
                RecordedRequest request;
                std::istringstream head{ pending.substr(0, head_end) };
                std::string line;
                std::getline(head, line);
                std::istringstream request_line{ line };
                std::string version;
                request_line >> request.method >> request.target >> version;
                while (std::getline(head, line)) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    auto const colon{ line.find(':') };
                    if (colon == std::string::npos) {
                        continue;
                    }
                    auto value{ line.substr(colon + 1) };
                    value.erase(0, value.find_first_not_of(' '));
                    request.headers[lowercase(line.substr(0, colon))] = value;
                }
                pending.erase(0, head_end + 4);
                std::size_t const length{ request.headers.contains("content-length") ? std::stoull(request.headers["content-length"]) : 0 };
                while (pending.size() < length) {
                    if (!receive(client->Handle(), pending, stop)) {
                        return;
                    }
                }
                request.body = pending.substr(0, length);
                pending.erase(0, length);

                ScriptedResponse response;
                {
                    std::scoped_lock lock{ m_mutex };
                    m_requests.push_back(std::move(request));
                    if (m_script.empty()) {
                        response = ScriptedResponse::Json(500, R"({"error":{"message":"no scripted response","type":"test"}})");
                    } else {
                        response = std::move(m_script.front());
                        m_script.pop_front();
                    }
                }
                pause(response.delay, stop);
                if (response.drop) {
                    client->Close();
                    return;
                }
                std::string head_text{ std::format("HTTP/1.1 {} Scripted\r\n", response.status) };
                for (auto const& [name, value] : response.headers) {
                    head_text += std::format("{}: {}\r\n", name, value);
                }
                if (response.chunked) {
                    head_text += "Transfer-Encoding: chunked\r\n\r\n";
                    if (!send_all(client->Handle(), head_text)) {
                        return;
                    }
                    for (auto const& piece : response.chunks) {
                        pause(response.chunk_delay, stop);
                        if (piece.empty()) {
                            continue;
                        }
                        if (!send_all(client->Handle(), std::format("{:x}\r\n{}\r\n", piece.size(), piece))) {
                            return;
                        }
                    }
                    if (!send_all(client->Handle(), "0\r\n\r\n")) {
                        return;
                    }
                } else {
                    std::string body;
                    for (auto const& piece : response.chunks) {
                        body += piece;
                    }
                    head_text += std::format("Content-Length: {}\r\n\r\n", body.size());
                    if (!send_all(client->Handle(), head_text + body)) {
                        return;
                    }
                }
            }
        }

        std::unique_ptr<Socket> m_listener;
        std::uint16_t m_port{};
        mutable std::mutex m_mutex;
        std::deque<ScriptedResponse> m_script;
        std::vector<RecordedRequest> m_requests;
        std::vector<std::jthread> m_clients;
        std::jthread m_acceptor;
    };

    /**
     * @brief Collect assertion failures and report them at exit.
     */
    class Checker {
    public:
        /**
         * @brief Record a failure when condition is false.
         */
        auto Check(bool condition, std::string_view description, std::source_location location = std::source_location::current()) -> bool {
            if (!condition) {
                ++m_failures;
                std::println("FAIL {}:{}: {}", location.file_name(), location.line(), description);
            }
            return condition;
        }

        /**
         * @brief Record a failure when two values differ, printing both.
         */
        template <typename T, typename U>
        auto Equal(T const& actual, U const& expected, std::string_view description, std::source_location location = std::source_location::current()) -> bool {
            if (!(actual == expected)) {
                ++m_failures;
                if constexpr (std::formattable<T, char> && std::formattable<U, char>) {
                    std::println("FAIL {}:{}: {}\n  actual:   {}\n  expected: {}", location.file_name(), location.line(), description, actual, expected);
                } else {
                    std::println("FAIL {}:{}: {}", location.file_name(), location.line(), description);
                }
                return false;
            }
            return true;
        }

        /**
         * @brief Print a summary and return the process exit code.
         */
        [[nodiscard]] auto Finish(std::string_view name) const -> int {
            if (m_failures == 0) {
                std::println("{}: all checks passed", name);
                return 0;
            }
            std::println("{}: {} check(s) failed", name, m_failures);
            return 1;
        }

    private:
        int m_failures{};
    };

} // namespace moai::test
