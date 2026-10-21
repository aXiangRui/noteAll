#pragma once

#include <memory>
#include <string>

#include "LLM-connect/connect.hpp"
#include "llm/chat.hpp"

// HttpChatClient —— OpenAI 兼容的聊天接口实现。
//
// 一份代码同时服务两个后端（只差配置）：
//   DeepSeek: baseUrl=https://api.deepseek.com      path=/chat/completions     apiKey=<key>
//   ollama  : baseUrl=http://127.0.0.1:11434        path=/v1/chat/completions  apiKey=""（不发 Authorization）
//
// llama.cpp --server 也兼容同一套 /v1/chat/completions。
namespace llm
{

struct ChatConfig
{
    // 形如 "https://api.deepseek.com" 或 "http://127.0.0.1:11434"。
    // 端口缺省时按 scheme 取 443 / 80。
    std::string baseUrl;

    // 请求路径："/chat/completions" 或 "/v1/chat/completions"。
    std::string path = "/chat/completions";

    std::string model;    // 例如 "deepseek-chat" / "llama3.1"
    std::string apiKey;   // 空 = 不发 Authorization（本地服务）

    // 透传给 connect 的传输参数：代理、超时、UA。
    // 本地服务务必设 proxyMode = Direct，否则可能被系统代理劫持。
    net::ConnectOptions transport;
};

class HttpChatClient final : public IChatClient
{
public:
    // 工厂：要解析 baseUrl、要 WinHttpOpen，都会失败 —— 构造函数没有返回值的位子。
    // 返回 unique_ptr 是因为 IChatClient 会通过指针持有（以及 Connect 不可拷贝）。
    static std::expected<std::unique_ptr<HttpChatClient>, ChatError> create(ChatConfig config);

    ~HttpChatClient() override;

    HttpChatClient(HttpChatClient const&)            = delete;
    HttpChatClient& operator=(HttpChatClient const&) = delete;

    std::expected<ChatReply, ChatError> complete(ChatRequest const& request) override;

private:
    HttpChatClient(ChatConfig config, net::Connect connect);

    ChatConfig   config_;
    net::Connect connect_;
};

}  // namespace llm
