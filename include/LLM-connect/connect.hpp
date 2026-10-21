#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Connect —— 只管"把字节发出去、把字节收回来"。
// 不认识 DeepSeek、不认识 JSON、不认识 note（见 CMakeLists.txt 里 connect 的注释）。
//
// 四条硬约束：
//   1. 这个头文件里不许出现 windows.h / winhttp.h —— 句柄藏在 Impl 里，宽字符不出 .cpp；
//   2. 接口全部用 UTF-8 的 std::string：项目其余部分都是 std::string，不让 wchar_t 漏出来；
//   3. 本层不打印任何请求头 —— Authorization 只允许进 WinHTTP，不允许进日志与错误消息；
//   4. 一次只伺候一个请求：一个 Connect 实例请在单线程串行使用（WinHTTP 句柄不保证并发安全，
//      并发请开多个实例，各自 create）。
namespace net
{

// 有序容器而不是 map：HTTP 允许重复头（Set-Cookie 就是典型），map 会悄悄吃掉重复项。
using HeaderList = std::vector<std::pair<std::string, std::string>>;

// 错误分类。
//
// 为什么不沿用 core 那边的 std::string：core 的错误只有"读盘/写盘失败了"一种，
// 上层只需要把话术打给人看；这里的错误要驱动行为 —— 超时可以退避重试，
// 401 重试一万次也没用，证书错误重试更是没意义。让调用方去 match 错误字符串是灾难。
enum class NetErrorCode
{
    Resolve,   // DNS 解析失败（域名拼错、断网）
    Connect,   // TCP 连不上（端口错、被墙、服务没起）
    Tls,       // 握手/证书/SNI 失败（证书过期、代理 MITM、系统时间错）
    Timeout,   // 四段超时之一命中
    Protocol,  // 连上了但响应不合 HTTP 语义（状态行/头畸形、读中断）
    Moved,     // 对象已被移动后再调用 —— 编程错误，不是网络问题
};

// code 的短名，只给日志用（别拿它当给用户看的文案）。
//
// 故意不叫 toString：任何做 ADL 的库都会在 net:: 里找到它，然后拿返回的
// char const* 去跟字符串相加 —— doctest 就是这么炸的（报一个完全无关的
// "不能添加两个指针"）。想给它 ostream 输出，自己在用它的地方加 operator<<。
constexpr char const* errorName(NetErrorCode code) noexcept
{
    switch (code)
    {
    case NetErrorCode::Resolve:  return "resolve";
    case NetErrorCode::Connect:  return "connect";
    case NetErrorCode::Tls:      return "tls";
    case NetErrorCode::Timeout:  return "timeout";
    case NetErrorCode::Protocol: return "protocol";
    case NetErrorCode::Moved:    return "moved";
    }
    return "unknown";
}

struct NetError
{
    NetErrorCode code = NetErrorCode::Protocol;
    std::string  message;   // 给人看的细节，已脱敏（不含 URL 里的密钥、不含 Authorization）
};

struct HttpResponse
{
    // 0 表示"没收到响应"——那种情况根本不会走到这里，而是 expected 里带 NetError。
    // 能拿到 HttpResponse 就说明至少收到过一个完整的 HTTP 响应头。
    int         statusCode = 0;

    // 原样保留（大小写、重复项都不动）。LLM 的限流重试全靠这几个头：
    //   retry-after / x-ratelimit-remaining / x-request-id
    HeaderList headers;

    // 已按 Content-Encoding 解压后的字节。是不是 UTF-8 由调用方判断 —— 本层不认识文本。
    std::string body;
};

struct ConnectOptions
{
    std::string userAgent = "noteAll/0.1";

    // WinHTTP 的四段超时，必须显式设：不设就是很久，断网时会把你卡在原地。
    // receive 给得最宽：LLM 生成慢，但也不能无限等。
    std::chrono::milliseconds resolveTimeout{5000};
    std::chrono::milliseconds connectTimeout{5000};
    std::chrono::milliseconds sendTimeout{30000};
    std::chrono::milliseconds receiveTimeout{60000};

    // 代理：System = 跟随系统设置；Direct = 直连（连本机 ollama 建议显式 Direct，
    // 免得被系统代理劫持到 127.0.0.1 之外）；Manual 时用 proxyAddress（"host:port"）。
    enum class ProxyMode { System, Direct, Manual };
    ProxyMode   proxyMode = ProxyMode::System;
    std::string proxyAddress;

    // 自动加 Accept-Encoding 并解压响应。关掉只在调试"服务器到底回了什么字节"时用。
    bool decompress = true;
};

class Connect
{
public:
    // 工厂而不是构造函数：WinHttpOpen / WinHttpConnect 会失败，
    // 而构造函数没有返回值的位子 —— 同 core::Core::load() 的理由。
    //
    // port 传 0 表示"按 secure 取默认端口"（443 / 80），免得调用方把
    // secure 和端口各写一遍、然后写矛盾（create(host, 80, true) 这种）。
    static std::expected<Connect, NetError> create(std::string    host,
                                                   std::uint16_t  port,
                                                   bool           secure,
                                                   ConnectOptions options = {});

    // 声明在头里、定义在 .cpp：Impl 在这里还不完整，
    // 析构/移动必须在能看见它的地方生成。
    ~Connect();

    Connect(Connect&&) noexcept;
    Connect& operator=(Connect&&) noexcept;
    Connect(Connect const&)            = delete;
    Connect& operator=(Connect const&) = delete;

    // 通用入口。method 用 "GET" / "POST" 这种大写字符串，本层只做转发与校验，
    // 不维护方法枚举 —— 用不到就别加。
    //
    // 约定：
    //   * Content-Length 由本层按 body.size() 自动设置，headers 里重复传会被覆盖；
    //   * body 是原始字节（JSON 序列化是上层的事）；
    //   * HTTP 4xx / 5xx 不算 error：连接成功就是 value，真实状态码在 statusCode 里，
    //     重试还是报错由调用方按 statusCode 决定；
    //   * error 只表示"没拿到一次完整响应"。
    std::expected<HttpResponse, NetError> request(std::string_view  method,
                                                  std::string_view  path,
                                                  std::string_view  body,
                                                  HeaderList const& headers);

    std::expected<HttpResponse, NetError> post(std::string_view  path,
                                               std::string_view  body,
                                               HeaderList const& headers);

    std::expected<HttpResponse, NetError> get(std::string_view  path,
                                              HeaderList const& headers = {});

    // moved-from 检查。移动之后 impl_ 为空，这时候调 request/post/get
    // 必须返回 NetError{Moved} 而不是解引用空指针 —— 实现里记得先看这个。
    bool valid() const noexcept { return impl_ != nullptr; }

private:
    Connect();

    // 句柄（HINTERNET）藏在 Impl 里，windows.h / winhttp.h 不出 .cpp。
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace net
