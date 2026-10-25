// llm_smoke —— 手动验证「llm 层能不能真的发一条消息」。
//
// 为什么单独做个工具、而不是塞进 note：
//   * note 目前是纯记录器（不链 llm），别为了测试把网络代码接进主线；
//   * 这个工具就是给人手动跑的：一条短请求，打印状态、回复、耗时、错误分类。
//
// 用法（key **不要**写在命令行里，会进 shell 历史和进程列表）：
//
//     $env:NOTE_LLM_KEY = (Get-Content .secrets\deepseek.key -Raw).Trim()
//     llm_smoke                                     # 最短请求，验证连通与鉴权
//     llm_smoke --draft "明天下午三点跟老张开会"      # 跑 M2 的草稿提示词
//     llm_smoke --base http://127.0.0.1:11434 --path /v1/chat/completions --model llama3.1 --no-auth --direct
//
// 退出码：0 = 成功；1 = 用法错误；2 = 失败（错误分类打在 stderr）

#include "llm/chat.hpp"
#include "llm/http_chat_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
# define WIN32_LEAN_AND_MEAN
# define NOMINMAX
# include <windows.h>   // SetConsoleOutputCP / GetConsoleProcessList
#endif

namespace
{

struct Args
{
    std::string base   = "https://api.deepseek.com";
    std::string path   = "/chat/completions";
    std::string model  = "deepseek-chat";
    std::string prompt = "用一句话回答：1+1 等于几？只输出答案。";
    bool        noAuth = false;
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

/** 去掉首尾空白与成对的引号 —— 从文件 Get-Content 出来的 key 常常带着这些。 */
std::string cleanKey(std::string key)
{
    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
    };

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

/**
 * 是不是"双击启动"（控制台里只有自己一个进程）。
 *
 * 命令行工具双击运行时，进程一结束窗口就没了，看着像闪退。
 * 在终端里跑时，控制台里至少还有 shell 陪着 → 不暂停，免得脚本里卡住。
 */
bool launchedByDoubleClick()
{
#ifdef _WIN32
    DWORD       list[2] = {};
    const DWORD count   = ::GetConsoleProcessList(list, 2);
    return count <= 1;
#else
    return false;
#endif
}

void printUsage()
{
    std::cout <<
        "llm_smoke —— 验证 llm 层能不能真的发出一条消息\n"
        "\n"
        "先在**同一个** PowerShell 窗口里设 key：\n"
        "  $env:NOTE_LLM_KEY = (Get-Content .secrets\\deepseek.key -Raw).Trim()\n"
        "\n"
        "用法：\n"
        "  llm_smoke                        发一条最短请求（验证连通与鉴权）\n"
        "  llm_smoke --draft \"一句话\"       跑 M2 的草稿提示词，打印解析与校验\n"
        "\n"
        "可选：\n"
        "  --base <url>      默认 https://api.deepseek.com\n"
        "  --path <path>     默认 /chat/completions\n"
        "  --model <name>    默认 deepseek-chat\n"
        "  --no-auth         不发 Authorization（本地服务用）\n"
        "  --direct          不走系统代理（连 ollama 建议加）\n";
}

// ---------------------------------------------------------------------------
// --draft：验证 M2 的提示词能不能稳定产出结构化草稿
//
// 接线进 app 之前先跑这个：确认「同一句话，模型能不能稳定给出
// {summary, tags, kind} 的合法 JSON」。不稳定就改提示词，别急着写业务代码。
// ---------------------------------------------------------------------------

std::string draftPrompt(std::string const& noteText)
{
    return "你是笔记整理助手。把用户的一句话整理成 JSON 对象，只输出 JSON，不要解释、不要代码块。\n"
           "\n"
           "字段要求：\n"
           "- summary：不超过 30 字的中文摘要，保留原文里的关键信息（人名、项目、时间）\n"
           "- tags：2 到 4 个中文标签，字符串数组\n"
           "- kind：只能是 event / note / todo / thought 之一\n"
           "  （event=发生或将要发生的事，note=信息/资料，todo=待办，thought=感受想法）\n"
           "\n"
           "用户记录：" + noteText;
}

/** 模型偶尔会包一层 ```json 代码块，去掉再解析。 */
std::string stripFence(std::string text)
{
    const auto trim = [](std::string s) {
        const auto isSpace = [](char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        };
        std::size_t b = 0;
        while (b < s.size() && isSpace(s[b])) { ++b; }
        std::size_t e = s.size();
        while (e > b && isSpace(s[e - 1])) { --e; }
        return s.substr(b, e - b);
    };

    text = trim(std::move(text));

    if (text.rfind("```", 0) == 0)
    {
        const auto firstNewline = text.find('\n');
        if (firstNewline != std::string::npos) { text = text.substr(firstNewline + 1); }
        if (const auto fenceEnd = text.rfind("```"); fenceEnd != std::string::npos)
        {
            text = text.substr(0, fenceEnd);
        }
        text = trim(std::move(text));
    }
    return text;
}

int runDraft(llm::IChatClient& client, std::string const& noteText)
{
    std::cout << "[draft] 原文: " << noteText << '\n';

    llm::ChatRequest request;
    request.messages.push_back({ llm::Message::Role::User, draftPrompt(noteText) });
    request.maxTokens   = 256;
    request.temperature = 0.2;   // 结构化抽取要稳，不要发散

    const auto started = std::chrono::steady_clock::now();
    auto       reply   = client.complete(request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started);

    if (!reply)
    {
        std::cerr << "[draft] 请求失败 " << elapsed.count() << "ms  ["
                  << llm::errorName(reply.error().kind) << "] http=" << reply.error().httpStatus
                  << "\n        " << reply.error().message << '\n';
        return 2;
    }

    std::cout << "[draft] 耗时 " << elapsed.count() << "ms，回复 " << reply->text.size() << " 字符\n";
    std::cout << "[draft] 原始回复: " << reply->text << '\n';

    // ---- 解析与校验：这一步就是 app 里要写的那段 ----
    const std::string jsonText = stripFence(reply->text);

    nlohmann::json parsed;
    try
    {
        parsed = nlohmann::json::parse(jsonText);
    }
    catch (std::exception const& ex)
    {
        std::cerr << "[draft] ✗ 不是合法 JSON：" << ex.what() << '\n';
        return 2;
    }

    if (!parsed.is_object())
    {
        std::cerr << "[draft] ✗ 顶层不是对象\n";
        return 2;
    }

    bool ok = true;

    const std::string summary = parsed.value("summary", std::string{});
    if (summary.empty())
    {
        std::cerr << "[draft] ✗ summary 缺失或为空\n";
        ok = false;
    }

    std::vector<std::string> tags;
    if (!parsed.contains("tags") || !parsed["tags"].is_array())
    {
        std::cerr << "[draft] ✗ tags 不是数组\n";
        ok = false;
    }
    else
    {
        for (auto const& tag : parsed["tags"])
        {
            if (tag.is_string()) { tags.push_back(tag.get<std::string>()); }
        }
        if (tags.empty())
        {
            std::cerr << "[draft] ✗ tags 为空\n";
            ok = false;
        }
    }

    const std::string kind = parsed.value("kind", std::string{});
    if (kind != "event" && kind != "note" && kind != "todo" && kind != "thought")
    {
        std::cerr << "[draft] ✗ kind 不在白名单：" << kind << '\n';
        ok = false;
    }

    std::cout << "[draft] 解析: summary=\"" << summary << "\"  kind=" << kind << "  tags=";
    for (std::size_t i = 0; i < tags.size(); ++i) { std::cout << (i ? ", " : "") << tags[i]; }
    std::cout << "\n[draft] 校验: " << (ok ? "✓ 通过" : "✗ 不通过") << '\n';

    return ok ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    // 控制台默认不是 UTF-8，不设这行中文输出会变成「璇锋眰」这种乱码。
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    // 双击 / 无参数：打印用法，并停住等回车 —— 别让窗口一闪就关。
    if (argc <= 1)
    {
        printUsage();
        if (launchedByDoubleClick())
        {
            std::cout << "\n按回车退出…" << std::flush;
            std::string ignored;
            std::getline(std::cin, ignored);
            return 0;
        }
        return 1;
    }

    Args args;
    args.base   = valueOf(argc, argv, "--base", args.base);
    args.path   = valueOf(argc, argv, "--path", args.path);
    args.model  = valueOf(argc, argv, "--model", args.model);
    args.prompt = valueOf(argc, argv, "--prompt", args.prompt);
    args.noAuth = hasFlag(argc, argv, "--no-auth");

    const std::string apiKey = args.noAuth ? std::string{} : cleanKey(envOr("NOTE_LLM_KEY", ""));

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
                                          : (apiKey.empty() ? "缺 key"
                                                            : "bearer(" + std::to_string(apiKey.size()) + " 字符)"))
              << "\n";

    if (!args.noAuth && apiKey.empty())
    {
        std::cerr << "[warn] 没读到环境变量 NOTE_LLM_KEY —— 这次请求不会带 Authorization，401 是预期的。\n"
                     "       请在**同一个** PowerShell 窗口里先执行：\n"
                     "         $env:NOTE_LLM_KEY = (Get-Content .secrets\\deepseek.key -Raw).Trim()\n"
                     "       确认读到了：  if (-not $env:NOTE_LLM_KEY) { \"没读到\" } else { \"读到 \" + $env:NOTE_LLM_KEY.Length + \" 字符\" }\n";
    }
    std::cout.flush();

    // --draft "<一句话>"：走 M2 的提示词，打印解析与校验结果。
    if (const std::string noteText = valueOf(argc, argv, "--draft", ""); !noteText.empty())
    {
        return runDraft(**client, noteText);
    }

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
