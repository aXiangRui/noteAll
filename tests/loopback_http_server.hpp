#pragma once

// 测试专用的最小回环 HTTP 服务器：真的走 TCP + HTTP，但不联网。
//
// 行为由调用方给的 handler 决定：handler 收到 (method, target, body)，
// 返回**完整的原始响应**（含状态行），这样测试要造 429 / 坏 JSON / 超长正文
// 都只是改一个 lambda 的事。

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

namespace testutil
{

inline void ensureWinsock()
{
    // WSAStartup 需要且只需要一次；函数内静态同时保证"一次"和"线程安全"。
    static const int started = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data);
    }();
    (void)started;
}

class LoopbackHttpServer
{
public:
    using Handler = std::function<std::string(std::string_view method,
                                              std::string_view target,
                                              std::string_view headers,
                                              std::string_view body)>;

    explicit LoopbackHttpServer(Handler handler) : handler_(std::move(handler))
    {
        ensureWinsock();

        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) { return; }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        address.sin_port = 0;  // 系统分配端口，测试之间不会互相抢

        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 4) != 0)
        {
            closeListener();
            return;
        }

        sockaddr_in bound{};
        int         size = sizeof(bound);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &size) != 0)
        {
            closeListener();
            return;
        }
        port_ = ::ntohs(bound.sin_port);

        worker_ = std::jthread([this] { serve(); });
    }

    ~LoopbackHttpServer() { stop(); }

    LoopbackHttpServer(LoopbackHttpServer const&)            = delete;
    LoopbackHttpServer& operator=(LoopbackHttpServer const&) = delete;

    [[nodiscard]] bool          ok() const noexcept { return listener_ != INVALID_SOCKET; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    void stop()
    {
        closeListener();
        if (worker_.joinable()) { worker_.join(); }
    }

private:
    void closeListener() noexcept
    {
        if (listener_ != INVALID_SOCKET)
        {
            ::closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
    }

    void serve()
    {
        for (;;)
        {
            SOCKET client = ::accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) { return; }  // listener 关了 = 收摊

            std::string method;
            std::string target;
            std::string headers;
            std::string body;
            if (readRequest(client, method, target, headers, body))
            {
                sendAll(client, handler_(method, target, headers, body));
            }

            ::shutdown(client, SD_SEND);
            ::closesocket(client);
        }
    }

    static bool readRequest(SOCKET client, std::string& method, std::string& target,
                            std::string& headers, std::string& body)
    {
        char        buffer[4096];
        std::string raw;
        std::size_t headerEnd = std::string::npos;

        while (headerEnd == std::string::npos)
        {
            const int received = ::recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (received <= 0) { return false; }
            raw.append(buffer, static_cast<std::size_t>(received));
            headerEnd = raw.find("\r\n\r\n");
        }

        const std::string_view head(raw.data(), headerEnd);
        headers = std::string(head);

        // 请求行："POST /path HTTP/1.1"
        const std::size_t firstSpace  = head.find(' ');
        const std::size_t secondSpace = head.find(' ', firstSpace == std::string_view::npos ? 0 : firstSpace + 1);
        if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos) { return false; }
        method = std::string(head.substr(0, firstSpace));
        target = std::string(head.substr(firstSpace + 1, secondSpace - firstSpace - 1));

        std::size_t contentLength = 0;
        const std::size_t lengthAt = head.find("Content-Length:");
        if (lengthAt != std::string_view::npos)
        {
            const std::size_t valueStart = lengthAt + std::string_view("Content-Length:").size();
            const std::size_t valueEnd   = head.find("\r\n", valueStart);
            const std::string_view text  = head.substr(valueStart, valueEnd - valueStart);
            contentLength = static_cast<std::size_t>(std::stoul(std::string(text)));
        }

        const std::size_t bodyStart = headerEnd + 4;
        while (raw.size() < bodyStart + contentLength)
        {
            const int received = ::recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (received <= 0) { return false; }
            raw.append(buffer, static_cast<std::size_t>(received));
        }

        body = raw.substr(bodyStart, contentLength);
        return true;
    }

    static void sendAll(SOCKET client, std::string const& data)
    {
        std::size_t sent = 0;
        while (sent < data.size())
        {
            const int wrote = ::send(client, data.data() + sent, static_cast<int>(data.size() - sent), 0);
            if (wrote <= 0) { return; }
            sent += static_cast<std::size_t>(wrote);
        }
    }

    Handler       handler_;
    SOCKET        listener_ = INVALID_SOCKET;
    std::uint16_t port_     = 0;
    std::jthread  worker_;
};

/** 造一个完整响应：状态行 + 头 + 正文。 */
inline std::string httpResponse(int status, std::string const& reason, std::string const& body,
                                std::string const& extraHeaders = {})
{
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" +
           "Content-Type: application/json\r\n" +
           "Content-Length: " + std::to_string(body.size()) + "\r\n" +
           extraHeaders +
           "Connection: close\r\n\r\n" + body;
}

}  // namespace testutil
