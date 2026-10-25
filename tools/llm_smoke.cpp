// llm_smoke —— 手动验证「llm 层能不能真的发一条消息」。
//
// 为什么单独做个工具、而不是塞进 note：
//   * note 目前是纯记录器（不链 llm），别为了测试把网络代码接进主线；
//   * 这个工具就是给人手动跑的：一条短请求，打印状态、回复、耗时、错误分类。
//
// 用法（key **不要**写在命令行里，会进 shell 历史和进程列表）：
//
//     set NOTE_LLM_KEY=sk-xxxx                      # 或者 PowerShell: $env:NOTE_LLM_KEY="sk-xxxx"
//     llm_smoke                                     # 默认打 DeepSeek
//     llm_smoke --base http://127.0.0.1:11434 --path /v1/chat/completions --model llama3.1 --no-auth
//
// 输出示例：
//     [ok] 1.42s  reply: 2
//
// 退出码：0 = 成功；2 = 失败（错误分类会打在 stderr 上）

#include "llm/chat.hpp"
#include "llm/http_chat_client.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
# define WIN32_LEAN_AND_MEAN
# define NOMINMAX
# include <windows.h>   // SetConsoleOutputCP：不设它中文输出会是乱码
#endif

namespace
{

struct Args
{
    std::string base    = "https://api.deepseek.com";
    std::string path    = "/chat/completions";
    std::string model   = "deepseek-chat";
    std::string prompt  = "用一句话回答：1+1 等于几？只输出答案。";
    bool        noAuth  = false;
    bool        insecure = false;
};

bool hasFlag(int argc, char** argv, char const* name)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], name) == 0)
        {
            return true;
        }
    }
    return false;
}

std::string valueOf(int argc, char** argv, char const* name, std::string fallback)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], name) == 0)
        {
            return argv[i + 1];
        }
    }
    return fallback;
}

std::string envOr(char const* name, std::string fallback)
{
    if (char const* value = std::getenv(name); value != nullptr && *value != '\0')
    {
        return value;
    }
    return fallback;
}

/** 去掉首尾空白与成对的引号 —— 从文件里 Get-Content 出来的 key 常常带着这些。 */
std::string cleanKey(std::string key)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; };

    std::size_t begin = 0;
    while (begin < key.size() && isSpace(key[begin])) { ++begin; }
    std::size_t end = key.size();
    while (end > begin && isSpace(key[end - 1])) { --end; }
    key = key.substr(begin, end - begin);

    if (key.size() >= 2 && (key.front() == '"' || key.front() == '\'') && key.back() == key.front())
    {
        key = key.substr(1, key.size() - 2);
    }
    return key;
}

}  // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    // 控制台默认不是 UTF-8，不设这行中文输出会变成「璇锋眰」这种乱码。
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    Args args;
    args.base   = valueOf(argc, argv, "--base", args.base);
    args.path   = valueOf(argc, argv, "--path", args.path);
    args.model  = valueOf(argc, argv, "--model", args.model);
    args.prompt = valueOf(argc, argv, "--prompt", args.prompt);
    args.noAuth = hasFlag(argc, argv, "--no-auth");

    std::string apiKey = args.noAuth ? std::string{} : cleanKey(envOr("NOTE_LLM_KEY", ""));

    llm::ChatConfig config;
    config.baseUrl = args.base;
    config.path    = args.path;
    config.model   = args.model;
    config.apiKey  = apiKey;

    // 本地服务（ollama / llama.cpp）通常是 http，而且不该被系统代理劫持。
    if (args.base.rfind("http://", 0) == 0 || hasFlag(argc, argv, "--direct"))
    {
        config.transport.proxyMode = net::ConnectOptions::ProxyMode::Direct;
    }
    config.transport.receiveTimeout = std::chrono::milliseconds{60000};

    auto client = llm::HttpChatClient::create(std::move(config));
    if (!client)
    {
        std::cerr << "[fail] 建 client 失败: [" << llm::errorName(client.error().kind) << "] "
                  << client.error().message << '\n';
        return 2;
    }

    std::cout << "请求: base=" << args.base << " path=" << args.path << " model=" << args.model
              << " auth=" << (args.noAuth ? "no"
                                          : (apiKey.empty() ? "缺 key" : "bearer(" + std::to_string(apiKey.size()) + " 字符)"))
              << "\n";

    if (!args.noAuth && apiKey.empty())
    {
        std::cerr << "[warn] 没读到环境变量 NOTE_LLM_KEY —— 这次请求不会带 Authorization，401 是预期的。\n"
                     "       请在**同一个** PowerShell 窗口里先执行：\n"
                     "         $env:NOTE_LLM_KEY = (Get-Content .secrets\\deepseek.key -Raw).Trim()\n"
                     "       确认读到了：  if (-not $env:NOTE_LLM_KEY) { \"没读到\" } else { \"读到 \" + $env:NOTE_LLM_KEY.Length + \" 字符\" }\n";
    }
    std::cout.flush();

    llm::ChatRequest request;
    request.messages.push_back({ llm::Message::Role::User, args.prompt });
    request.maxTokens   = 16;      // 测试用，越短越省钱
    request.temperature = 0.0;

    const auto started = std::chrono::steady_clock::now();
    auto       reply   = (*client)->complete(request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started);

    if (!reply)
    {
        std::cerr << "[fail] " << elapsed.count() << "ms  [" << llm::errorName(reply.error().kind) << "]"
                  << " retryable=" << (reply.error().retryable() ? "yes" : "no")
                  << " http=" << reply.error().httpStatus << "\n"
                  << "       " << reply.error().message << '\n';
        return 2;
    }

    std::cout << "[ok] " << elapsed.count() << "ms  reply: " << reply->text << '\n';
    return 0;
}
