#include "llm/http_chat_client.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <string>
#include <utility>

namespace llm
{

namespace
{

using json = nlohmann::json;

struct Endpoint
{
    std::string   host;
    std::uint16_t port   = 0;
    bool          secure = true;
};

std::expected<Endpoint, ChatError> parseEndpoint(std::string const& baseUrl)
{
    if (baseUrl.empty()) { return std::unexpected(ChatError{ChatErrorKind::NotConfigured, 0, "baseUrl 为空"}); }

    const std::size_t schemeEnd = baseUrl.find("://");
    if (schemeEnd == std::string::npos)
    {
        return std::unexpected(ChatError{ChatErrorKind::NotConfigured, 0,
                                         "baseUrl 缺少 scheme（应形如 https://api.deepseek.com）"});
    }

    std::string scheme = baseUrl.substr(0, schemeEnd);
    for (char& ch : scheme) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }

    bool secure = false;
    if (scheme == "https") { secure = true; }
    else if (scheme != "http")
    {
        return std::unexpected(ChatError{ChatErrorKind::NotConfigured, 0,
                                         "baseUrl 只支持 http / https：" + scheme});
    }

    // 只取 authority；路径由 ChatConfig::path 单独给，免得两处都能改路径。
    std::string rest = baseUrl.substr(schemeEnd + 3);
    const std::size_t slash = rest.find('/');
    if (slash != std::string::npos) { rest = rest.substr(0, slash); }
    if (rest.empty())
    {
        return std::unexpected(ChatError{ChatErrorKind::NotConfigured, 0, "baseUrl 缺少主机名"});
    }

    Endpoint endpoint;
    endpoint.secure = secure;

    const std::size_t colon = rest.rfind(':');
    if (colon == std::string::npos)
    {
        endpoint.host = rest;
        return endpoint;
    }

    endpoint.host = rest.substr(0, colon);
    if (endpoint.host.empty())
    {
        return std::unexpected(ChatError{ChatErrorKind::NotConfigured, 0, "baseUrl 的主机名为空"});
    }

    unsigned port = 0;
    const char* begin = rest.data() + colon + 1;
    const char* end   = rest.data() + rest.size();
    const auto  parsed = std::from_chars(begin, end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end || port == 0 || port > 65535)
    {
        return std::unexpected(ChatError{ChatErrorKind::NotConfigured, 0, "baseUrl 的端口不合法"});
    }
    endpoint.port = static_cast<std::uint16_t>(port);
    return endpoint;
}

char const* roleName(Message::Role role) noexcept
{
    switch (role)
    {
    case Message::Role::System:    return "system";
    case Message::Role::User:      return "user";
    case Message::Role::Assistant: return "assistant";
    }
    return "user";
}

/** ChatRequest → OpenAI 兼容的请求体。 */
std::string buildBody(ChatRequest const& request, std::string const& model)
{
    json messages = json::array();
    for (Message const& message : request.messages)
    {
        messages.push_back({{"role", roleName(message.role)}, {"content", message.content}});
    }

    json body;
    body["model"]       = model;
    body["messages"]    = std::move(messages);
    body["max_tokens"]  = request.maxTokens;
    body["temperature"] = request.temperature;
    body["stream"]      = false;
    return body.dump();
}

/**
 * HTTP 状态码 → 错误分类。
 *
 * 401/403 重试一万次也没用；429 要退避重试；5xx 是服务端抽风，值得重试；
 * 其余 4xx 说明请求本身有问题（prompt 太长、参数错），改代码才能解决。
 */
ChatError errorFromStatus(int status, std::string const& body)
{
    ChatErrorKind kind = ChatErrorKind::BadResponse;
    if (status == 401 || status == 403) { kind = ChatErrorKind::Auth; }
    else if (status == 429) { kind = ChatErrorKind::RateLimited; }
    else if (status >= 500) { kind = ChatErrorKind::BadResponse; }
    else if (status >= 400) { kind = ChatErrorKind::Refused; }

    // 响应体里常带 {"error":{"message":...}}，截一段给日志；不整段塞进错误里。
    std::string detail = body.substr(0, 200);
    return ChatError{kind, status, "HTTP " + std::to_string(status) + ": " + detail};
}

/** net 层的错误 → llm 层的错误。TLS 不归类为可重试。 */
ChatError errorFromNet(net::NetError const& error)
{
    switch (error.code)
    {
    case net::NetErrorCode::Timeout:
    case net::NetErrorCode::Resolve:
    case net::NetErrorCode::Connect:
        return ChatError{ChatErrorKind::Transport, 0, error.message};

    case net::NetErrorCode::Tls:
        // 证书/握手问题重试也没用，按"配置问题"处理，让调用方尽早发现。
        return ChatError{ChatErrorKind::Refused, 0, "TLS/证书：" + error.message};

    case net::NetErrorCode::Protocol:
    case net::NetErrorCode::Moved:
        return ChatError{ChatErrorKind::BadResponse, 0, error.message};
    }
    return ChatError{ChatErrorKind::Transport, 0, error.message};
}

/** 解析 OpenAI 兼容的响应体，抠出 choices[0].message.content。 */
std::expected<ChatReply, ChatError> parseReply(std::string const& body)
{
    json parsed;
    try
    {
        parsed = json::parse(body);
    }
    catch (std::exception const&)
    {
        return std::unexpected(ChatError{ChatErrorKind::BadResponse, 0,
                                         "响应不是合法 JSON：" + body.substr(0, 200)});
    }

    // 有些后端会用 200 返回 {"error":{...}}，不能当成成功。
    if (parsed.contains("error"))
    {
        std::string detail = parsed["error"].dump().substr(0, 200);
        return std::unexpected(ChatError{ChatErrorKind::Refused, 0, "服务端返回 error：" + detail});
    }

    if (!parsed.contains("choices") || !parsed["choices"].is_array() || parsed["choices"].empty())
    {
        return std::unexpected(ChatError{ChatErrorKind::BadResponse, 0, "响应缺少 choices"});
    }

    const json& first = parsed["choices"][0];
    if (!first.contains("message") || !first["message"].contains("content") ||
        !first["message"]["content"].is_string())
    {
        return std::unexpected(ChatError{ChatErrorKind::BadResponse, 0, "响应缺少 message.content"});
    }

    return ChatReply{first["message"]["content"].get<std::string>()};
}

}  // namespace

HttpChatClient::HttpChatClient(ChatConfig config, net::Connect connect)
    : config_(std::move(config)), connect_(std::move(connect))
{
}

HttpChatClient::~HttpChatClient() = default;

std::expected<std::unique_ptr<HttpChatClient>, ChatError> HttpChatClient::create(ChatConfig config)
{
    auto endpoint = parseEndpoint(config.baseUrl);
    if (!endpoint) { return std::unexpected(std::move(endpoint.error())); }

    auto connect = net::Connect::create(endpoint->host, endpoint->port, endpoint->secure, config.transport);
    if (!connect) { return std::unexpected(errorFromNet(connect.error())); }

    // 构造函数是私有的：用 new 而不是 make_unique（后者拿不到访问权）。
    return std::unique_ptr<HttpChatClient>(new HttpChatClient(std::move(config), std::move(*connect)));
}

std::expected<ChatReply, ChatError> HttpChatClient::complete(ChatRequest const& request)
{
    const std::string model = request.model.empty() ? config_.model : request.model;
    if (model.empty())
    {
        return std::unexpected(ChatError{ChatErrorKind::NotConfigured, 0,
                                         "没有指定 model（ChatConfig::model 与 ChatRequest::model 都为空）"});
    }

    net::HeaderList headers{{"Content-Type", "application/json"}};
    if (!config_.apiKey.empty()) { headers.emplace_back("Authorization", "Bearer " + config_.apiKey); }

    const std::string payload = buildBody(request, model);

    auto response = connect_.post(config_.path, payload, headers);
    if (!response) { return std::unexpected(errorFromNet(response.error())); }

    if (response->statusCode < 200 || response->statusCode >= 300)
    {
        return std::unexpected(errorFromStatus(response->statusCode, response->body));
    }

    return parseReply(response->body);
}

}  // namespace llm
