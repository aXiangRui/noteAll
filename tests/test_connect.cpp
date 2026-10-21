#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "LLM-connect/connect.hpp"

// winsock2.h 必须排在 windows.h 之前；这个测试文件不需要 windows.h，所以直接用它。
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

// doctest 失败时要把操作数打印出来。enum class 默认不可打印，而 net:: 里
// 又没有现成的 ostream 支持 —— 给它一个 operator<<，失败信息里才会是
// "resolve" 而不是一串数字。注意别在 net 里叫 toString：那会劫持 doctest 的
// ADL 查找，报出一个完全无关的两指针相加错误。
namespace net
{
inline std::ostream& operator<<(std::ostream& out, NetErrorCode code)
{
    return out << errorName(code);
}
}  // namespace net

// connect —— 网络传输层。这里**不联网**：本机 127.0.0.1 上起一个回环服务器，
// 真的走一遍 TCP + HTTP，但不依赖外网、不依赖任何 API key。
//
// 覆盖到：请求送达、状态码、响应头、响应体、404 不算 error、连不上分类、移动语义。
// 没覆盖：gzip 解压（要 zlib 才能造压缩响应，那部分由手工冒烟脚本验过）。

namespace
{

void ensureWinsock()
{
    // WSAStartup 需要且只需要一次；函数内静态正好保证"一次"和"线程安全"。
    static const int started = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data);
    }();
    (void)started;
}

void closeSocket(SOCKET socket) noexcept
{
    if (socket != INVALID_SOCKET) { ::closesocket(socket); }
}

/**
 * 最小回环 HTTP 服务器。
 *
 * 行为固定：
 *   POST /echo -> 201，body 原样回显，带一个自定义头 X-Test
 *   其它路径   -> 404
 *
 * 故意不做并发、不做 keep-alive：测试要的是"确定"，不是"通用"。
 */
class LoopbackServer
{
public:
    LoopbackServer()
    {
        ensureWinsock();

        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) { return; }

        sockaddr_in address{};
        address.sin_family      = AF_INET;
        // inet_addr 已被标记废弃（C4996），用 inet_pton 代替。
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        address.sin_port        = 0;  // 让系统分配端口，避免和别的测试抢

        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            closeSocket(listener_);
            listener_ = INVALID_SOCKET;
            return;
        }
        if (::listen(listener_, 4) != 0)
        {
            closeSocket(listener_);
            listener_ = INVALID_SOCKET;
            return;
        }

        sockaddr_in bound{};
        int         size = sizeof(bound);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &size) != 0)
        {
            closeSocket(listener_);
            listener_ = INVALID_SOCKET;
            return;
        }
        port_ = ::ntohs(bound.sin_port);

        worker_ = std::jthread([this] { serve(); });
    }

    ~LoopbackServer() { stop(); }

    LoopbackServer(LoopbackServer const&)            = delete;
    LoopbackServer& operator=(LoopbackServer const&) = delete;

    [[nodiscard]] bool ok() const noexcept { return listener_ != INVALID_SOCKET; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    void stop()
    {
        if (listener_ != INVALID_SOCKET)
        {
            closeSocket(listener_);
            listener_ = INVALID_SOCKET;
        }
        if (worker_.joinable()) { worker_.join(); }
    }

private:
    void serve()
    {
        for (;;)
        {
            SOCKET client = ::accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) { return; }  // listener 被关掉 = 该收摊了

            std::string request;
            std::string body;
            if (readRequest(client, request, body)) { sendAll(client, respond(request, body)); }

            ::shutdown(client, SD_SEND);
            closeSocket(client);
        }
    }

    /** 读到 "\r\n\r\n" 为止，再按 Content-Length 把 body 补齐。 */
    static bool readRequest(SOCKET client, std::string& request, std::string& body)
    {
        char        buffer[4096];
        std::size_t headerEnd = std::string::npos;

        while (headerEnd == std::string::npos)
        {
            const int received = ::recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (received <= 0) { return false; }
            request.append(buffer, static_cast<std::size_t>(received));
            headerEnd = request.find("\r\n\r\n");
        }

        const std::size_t bodyStart = headerEnd + 4;

        // 只找 Content-Length，够用了。
        std::size_t length = 0;
        const auto  header = request.find("Content-Length:");
        if (header != std::string::npos && header < headerEnd)
        {
            const std::size_t valueStart = header + std::string("Content-Length:").size();
            const std::size_t valueEnd   = request.find("\r\n", valueStart);
            length = static_cast<std::size_t>(std::stoul(request.substr(valueStart, valueEnd - valueStart)));
        }

        while (request.size() < bodyStart + length)
        {
            const int received = ::recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (received <= 0) { return false; }
            request.append(buffer, static_cast<std::size_t>(received));
        }

        body = request.substr(bodyStart, length);
        return true;
    }

    static std::string respond(std::string const& request, std::string const& body)
    {
        const std::size_t lineEnd = request.find("\r\n");
        const std::string statusLine = request.substr(0, lineEnd == std::string::npos ? request.size() : lineEnd);

        const bool isEcho = statusLine.find(" /echo ") != std::string::npos;
        if (!isEcho)
        {
            const std::string payload = "not found";
            return "HTTP/1.1 404 Not Found\r\n"
                   "Content-Type: text/plain\r\n"
                   "Content-Length: " + std::to_string(payload.size()) + "\r\n"
                   "Connection: close\r\n\r\n" + payload;
        }

        return "HTTP/1.1 201 Created\r\n"
               "Content-Type: application/json\r\n"
               "X-Test: hello-from-local\r\n"
               "Content-Length: " + std::to_string(body.size()) + "\r\n"
               "Connection: close\r\n\r\n" + body;
    }

    static void sendAll(SOCKET client, std::string const& data)
    {
        std::size_t sent = 0;
        while (sent < data.size())
        {
            const int wrote = ::send(client, data.data() + sent,
                                     static_cast<int>(data.size() - sent), 0);
            if (wrote <= 0) { return; }
            sent += static_cast<std::size_t>(wrote);
        }
    }

    SOCKET       listener_ = INVALID_SOCKET;
    std::uint16_t port_    = 0;
    std::jthread worker_;
};

/** 测试用连接参数：直连（别让系统代理劫持 127.0.0.1）+ 短超时。 */
net::ConnectOptions localOptions()
{
    net::ConnectOptions options;
    options.proxyMode      = net::ConnectOptions::ProxyMode::Direct;
    options.receiveTimeout = std::chrono::milliseconds{5000};
    return options;
}

}  // namespace

TEST_CASE("connect：空 host 在建连前就被拒，且分类为 Resolve")
{
    auto created = net::Connect::create("", 0, false);

    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == net::NetErrorCode::Resolve);

    // 错误话术不许是空的：排查时"失败了"三个字等于没说。
    CHECK_FALSE(created.error().message.empty());
}

TEST_CASE("connect：POST 送达、状态码/响应头/响应体都对")
{
    LoopbackServer server;
    REQUIRE(server.ok());

    auto created = net::Connect::create("127.0.0.1", server.port(), false, localOptions());
    REQUIRE(created.has_value());

    const std::string payload = R"({"msg":"你好"})";
    net::HeaderList   headers{{"Content-Type", "application/json"}};

    auto response = created->post("/echo", payload, headers);
    REQUIRE(response.has_value());

    CHECK(response->statusCode == 201);
    CHECK(response->body == payload);  // 服务端原样回显

    bool sawTestHeader = false;
    for (auto const& header : response->headers)
    {
        if (header.first == "X-Test" && header.second == "hello-from-local") { sawTestHeader = true; }
    }
    CHECK(sawTestHeader);
}

TEST_CASE("connect：HTTP 404 是 value，不是 error")
{
    LoopbackServer server;
    REQUIRE(server.ok());

    auto created = net::Connect::create("127.0.0.1", server.port(), false, localOptions());
    REQUIRE(created.has_value());

    auto response = created->get("/nope");
    REQUIRE(response.has_value());
    CHECK(response->statusCode == 404);
    CHECK(response->body == "not found");
}

TEST_CASE("connect：连不上的端口分类为 Connect 或 Timeout，而不是未知")
{
    // 1 号端口基本不可能有人监听；回环上的拒绝连接是立即的，不会挂住。
    auto created = net::Connect::create("127.0.0.1", 1, false, localOptions());

    // 建连阶段也可能失败（比如 WinHttpOpen 挂了），那也算过 —— 重点是分类对不对。
    if (!created)
    {
        CHECK(created.error().code != net::NetErrorCode::Protocol);
        return;
    }

    auto response = created->get("/");
    REQUIRE_FALSE(response.has_value());

    const net::NetErrorCode code = response.error().code;
    CHECK((code == net::NetErrorCode::Connect || code == net::NetErrorCode::Timeout));
    CHECK_FALSE(response.error().message.empty());
}

TEST_CASE("connect：移动之后的实例返回 Moved，而不是崩溃")
{
    LoopbackServer server;
    REQUIRE(server.ok());

    auto created = net::Connect::create("127.0.0.1", server.port(), false, localOptions());
    REQUIRE(created.has_value());

    net::Connect moved = std::move(*created);
    CHECK(moved.valid());

    // 移动后的老对象：必须报 Moved，不能解引用空 impl_。
    auto afterMove = created->get("/");
    REQUIRE_FALSE(afterMove.has_value());
    CHECK(afterMove.error().code == net::NetErrorCode::Moved);
}
