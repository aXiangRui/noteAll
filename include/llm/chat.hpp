#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "LLM-connect/connect.hpp"

// llm —— "给定一段对话，拿回一段文本"。
//
// 分层上的位置：net(connect) 只管字节收发；这一层管协议与语义（JSON、鉴权、
// 把 HTTP 状态翻译成"要不要重试"）；再往上的草稿/队列/确认流程在 workflow 层，
// 不认识 HTTP，也不认识 model 名。
//
// 硬约束：
//   1. 这一层不认识 core（Entry / Store 一概不知），依赖方向 llm -> net + third_party；
//   2. 一个 client 实例只在一个线程里用（内部持有 net::Connect，它是串行语义）；
//      批量并发就每个 worker 建一个实例；
//   3. 错误里绝不回显 Authorization / apiKey。
namespace llm
{

struct Message
{
    enum class Role
    {
        System,
        User,
        Assistant,
    };

    Role        role = Role::User;
    std::string content;
};

struct ChatRequest
{
    std::vector<Message> messages;
    std::string          model;              // 空 = 用 client 配置里的默认值
    int                  maxTokens   = 512;
    double               temperature = 0.2;
};

struct ChatReply
{
    // 模型的原始回复文本。**不要**在这里解析成 summary/tags ——
    // 那是 prompt 层的责任，换模型或换 prompt 时只有那一层要改。
    std::string text;
};

enum class ChatErrorKind
{
    Transport,      // 网络层失败：连不上、超时、TLS 之外的传输问题（可重试）
    Auth,           // 401 / 403：密钥错、没权限（不可重试，重试一万次也一样）
    RateLimited,    // 429：被限流（可重试，但要退避）
    BadResponse,    // 响应不是合法 JSON / 缺字段 / 5xx（可重试）
    Refused,        // 4xx（429 除外）：请求本身有问题，改参数才能解决（不可重试）
    NotConfigured,  // 没配置（比如本地服务没起、apiKey 缺失）（不可重试）
};

// 队列只认这个，**不要**让它去识别 int httpStatus。
constexpr bool isRetryable(ChatErrorKind kind) noexcept
{
    switch (kind)
    {
    case ChatErrorKind::Transport:   return true;
    case ChatErrorKind::RateLimited: return true;
    case ChatErrorKind::BadResponse: return true;
    case ChatErrorKind::Auth:
    case ChatErrorKind::Refused:
    case ChatErrorKind::NotConfigured: return false;
    }
    return false;
}

// 名字故意不叫 toString：任何做 ADL 的库（doctest / fmt）都会在 llm:: 里找到它，
// 拿返回的 char const* 去跟字符串相加，报一个完全无关的"两指针相加"。
constexpr char const* errorName(ChatErrorKind kind) noexcept
{
    switch (kind)
    {
    case ChatErrorKind::Transport:      return "transport";
    case ChatErrorKind::Auth:           return "auth";
    case ChatErrorKind::RateLimited:    return "rate-limited";
    case ChatErrorKind::BadResponse:    return "bad-response";
    case ChatErrorKind::Refused:        return "refused";
    case ChatErrorKind::NotConfigured:  return "not-configured";
    }
    return "unknown";
}

struct ChatError
{
    ChatErrorKind kind       = ChatErrorKind::Transport;
    int           httpStatus = 0;   // 仅 HTTP 实现会填，0 表示没走到 HTTP 响应
    std::string   message;          // 已脱敏：不含 apiKey / Authorization

    [[nodiscard]] bool retryable() const noexcept { return isRetryable(kind); }
};

/**
 * 聊天客户端的接口。
 *
 * 为什么要有接口（而不是直接用 HttpChatClient）：
 *   1. 测试：FakeChatClient 能在毫秒内模拟超时 / 429 / 坏 JSON / 超长文本，
 *      队列的重试与毒丸逻辑全靠它测；
 *   2. 未来若真做"进程内嵌入模型"，它是唯一需要新实现的地方；
 *      但如果本地模型仍走 HTTP（ollama / llama.cpp --server），就**不需要**新类，
 *      换 ChatConfig 即可。
 *
 * 注意：虚析构必须写。M0 的 View 就是漏了这一步（ROADMAP §2.11 第 5 条）。
 */
class IChatClient
{
public:
    virtual ~IChatClient() = default;

    IChatClient()                                       = default;
    IChatClient(IChatClient const&)                     = delete;
    IChatClient& operator=(IChatClient const&)          = delete;
    IChatClient(IChatClient&&)                          = delete;
    IChatClient& operator=(IChatClient&&)               = delete;

    // 阻塞调用，一次一问一答。
    // 返回 expected：网络/协议问题走 error，别抛异常（异常留给编程错误）。
    virtual std::expected<ChatReply, ChatError> complete(ChatRequest const& request) = 0;
};

}  // namespace llm
