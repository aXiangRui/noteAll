#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <ostream>
#include <string>

#include "llm/chat.hpp"
#include "llm/http_chat_client.hpp"
#include "loopback_http_server.hpp"

// llm 层 —— 协议与错误分类。测试同样不联网：本机回环服务器扮演
// "OpenAI 兼容后端"，用请求体里的标记词决定返回什么。
//
// 覆盖：正常回复 / Bearer 鉴权 / 429 可重试 / 401 不可重试 / 坏 JSON /
//       200 带 error 字段 / 缺 model 不发请求 / baseUrl 解析失败。

namespace llm
{
// doctest 打印操作数要用，且不能叫 toString（ADL 劫持会报"两指针相加"）。
inline std::ostream& operator<<(std::ostream& out, ChatErrorKind kind)
{
    return out << errorName(kind);
}
}  // namespace llm

namespace
{

using testutil::httpResponse;
using testutil::LoopbackHttpServer;

/** 造一个指向回环服务器的配置：本地服务，直连（别让系统代理劫持）。 */
llm::ChatConfig localConfig(std::uint16_t port, std::string apiKey = {})
{
    llm::ChatConfig config;
    config.baseUrl = "http://127.0.0.1:" + std::to_string(port);
    config.path    = "/v1/chat/completions";
    config.model   = "test-model";
    config.apiKey  = std::move(apiKey);

    config.transport.proxyMode      = net::ConnectOptions::ProxyMode::Direct;
    config.transport.receiveTimeout = std::chrono::milliseconds{5000};
    return config;
}

llm::ChatRequest requestWith(std::string const& content)
{
    llm::ChatRequest request;
    request.messages.push_back({llm::Message::Role::System, "你是笔记助手"});
    request.messages.push_back({llm::Message::Role::User, content});
    return request;
}

}  // namespace

TEST_CASE("llm：正常回复被解出来，请求体是 OpenAI 形状")
{
    std::string seenBody;
    std::string seenTarget;

    LoopbackHttpServer server([&](std::string_view method, std::string_view target,
                                  std::string_view /*headers*/, std::string_view body) {
        CHECK(method == "POST");
        seenTarget = std::string(target);
        seenBody   = std::string(body);
        return httpResponse(200, "OK",
                            R"({"choices":[{"message":{"role":"assistant","content":"草稿：与老张确认排期"}}]})");
    });
    REQUIRE(server.ok());

    auto client = llm::HttpChatClient::create(localConfig(server.port()));
    REQUIRE(client.has_value());

    auto reply = (*client)->complete(requestWith("下午跟老张聊排期"));
    REQUIRE(reply.has_value());
    CHECK(reply->text == "草稿：与老张确认排期");

    CHECK(seenTarget == "/v1/chat/completions");
    // 请求体里该有的东西：model、两条 messages、stream=false。
    CHECK(seenBody.find("\"model\":\"test-model\"") != std::string::npos);
    CHECK(seenBody.find("\"role\":\"system\"") != std::string::npos);
    CHECK(seenBody.find("\"role\":\"user\"") != std::string::npos);
    CHECK(seenBody.find("\"stream\":false") != std::string::npos);
    // 没配 apiKey 就不许发 Authorization。
    CHECK(seenBody.find("Authorization") == std::string::npos);
}

TEST_CASE("llm：配了 apiKey 就发 Bearer")
{
    std::string seenHeaders;

    LoopbackHttpServer server([&](std::string_view, std::string_view,
                                  std::string_view headers, std::string_view) {
        seenHeaders = std::string(headers);
        return httpResponse(200, "OK",
                            R"({"choices":[{"message":{"content":"ok"}}]})");
    });
    REQUIRE(server.ok());

    auto client = llm::HttpChatClient::create(localConfig(server.port(), "sk-test-123"));
    REQUIRE(client.has_value());

    auto reply = (*client)->complete(requestWith("hi"));
    REQUIRE(reply.has_value());
    CHECK(seenHeaders.find("Authorization: Bearer sk-test-123") != std::string::npos);
}

TEST_CASE("llm：429 归类为限流且可重试")
{
    LoopbackHttpServer server([](std::string_view, std::string_view, std::string_view, std::string_view) {
        return httpResponse(429, "Too Many Requests", R"({"error":{"message":"slow down"}})");
    });
    REQUIRE(server.ok());

    auto client = llm::HttpChatClient::create(localConfig(server.port()));
    REQUIRE(client.has_value());

    auto reply = (*client)->complete(requestWith("hi"));
    REQUIRE_FALSE(reply.has_value());
    CHECK(reply.error().kind == llm::ChatErrorKind::RateLimited);
    CHECK(reply.error().httpStatus == 429);
    CHECK(reply.error().retryable());
}

TEST_CASE("llm：401 归类为鉴权失败且不可重试")
{
    LoopbackHttpServer server([](std::string_view, std::string_view, std::string_view, std::string_view) {
        return httpResponse(401, "Unauthorized", R"({"error":{"message":"invalid key"}})");
    });
    REQUIRE(server.ok());

    auto client = llm::HttpChatClient::create(localConfig(server.port(), "wrong"));
    REQUIRE(client.has_value());

    auto reply = (*client)->complete(requestWith("hi"));
    REQUIRE_FALSE(reply.has_value());
    CHECK(reply.error().kind == llm::ChatErrorKind::Auth);
    CHECK_FALSE(reply.error().retryable());
}

TEST_CASE("llm：200 但不是 JSON，归类为坏响应且可重试")
{
    LoopbackHttpServer server([](std::string_view, std::string_view, std::string_view, std::string_view) {
        return httpResponse(200, "OK", "this is not json at all");
    });
    REQUIRE(server.ok());

    auto client = llm::HttpChatClient::create(localConfig(server.port()));
    REQUIRE(client.has_value());

    auto reply = (*client)->complete(requestWith("hi"));
    REQUIRE_FALSE(reply.has_value());
    CHECK(reply.error().kind == llm::ChatErrorKind::BadResponse);
    CHECK(reply.error().retryable());
}

TEST_CASE("llm：200 但带 error 字段，按服务端拒绝处理")
{
    LoopbackHttpServer server([](std::string_view, std::string_view, std::string_view, std::string_view) {
        return httpResponse(200, "OK", R"({"error":{"message":"model overloaded"}})");
    });
    REQUIRE(server.ok());

    auto client = llm::HttpChatClient::create(localConfig(server.port()));
    REQUIRE(client.has_value());

    auto reply = (*client)->complete(requestWith("hi"));
    REQUIRE_FALSE(reply.has_value());
    CHECK(reply.error().kind == llm::ChatErrorKind::Refused);
    CHECK_FALSE(reply.error().retryable());
}

TEST_CASE("llm：没 model 就报 NotConfigured，且一个请求都不发")
{
    std::atomic<int> requests{0};

    LoopbackHttpServer server([&](std::string_view, std::string_view, std::string_view, std::string_view) {
        ++requests;
        return httpResponse(200, "OK", R"({"choices":[{"message":{"content":"不该到这里"}}]})");
    });
    REQUIRE(server.ok());

    auto config = localConfig(server.port());
    config.model.clear();

    auto client = llm::HttpChatClient::create(config);
    REQUIRE(client.has_value());

    auto reply = (*client)->complete(requestWith("hi"));
    REQUIRE_FALSE(reply.has_value());
    CHECK(reply.error().kind == llm::ChatErrorKind::NotConfigured);
    CHECK(requests.load() == 0);  // 先校验再发请求，别浪费一次往返
}

TEST_CASE("llm：baseUrl 不合法在建连前就失败")
{
    auto badScheme = llm::HttpChatClient::create([] {
        llm::ChatConfig config;
        config.baseUrl = "api.deepseek.com";  // 缺 scheme
        config.model   = "x";
        return config;
    }());
    REQUIRE_FALSE(badScheme.has_value());
    CHECK(badScheme.error().kind == llm::ChatErrorKind::NotConfigured);

    auto badPort = llm::HttpChatClient::create([] {
        llm::ChatConfig config;
        config.baseUrl = "http://127.0.0.1:70000";
        config.model   = "x";
        return config;
    }());
    REQUIRE_FALSE(badPort.has_value());
    CHECK(badPort.error().kind == llm::ChatErrorKind::NotConfigured);
}
