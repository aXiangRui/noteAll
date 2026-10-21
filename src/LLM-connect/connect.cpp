#include "LLM-connect/connect.hpp"

// windows.h / winhttp.h 只在这个 .cpp 里出现，绝不进公共头文件：
// 它们会往全局命名空间塞几千个宏和名字（ROADMAP §2.0(b)）。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstddef>
#include <utility>

// 链接由 CMakeLists.txt 负责（target_link_libraries(connect PRIVATE winhttp)）。
// 这里不再写 #pragma comment，避免两处各说各话。

namespace net
{

namespace
{

// ---------------------------------------------------------------------------
// RAII：HINTERNET 的唯一 owner
//
// 手写 close 是这个文件里最容易漏的地方：request() 里任何一个提前 return 都要关句柄，
// 漏一次就是句柄泄漏，而且只在高频调用时才看得出来。用这个类，顺序自动对。
// ---------------------------------------------------------------------------
struct UniqueHandle
{
    HINTERNET value = nullptr;

    UniqueHandle() = default;
    explicit UniqueHandle(HINTERNET handle) noexcept : value(handle) {}

    ~UniqueHandle()
    {
        if (value != nullptr) { ::WinHttpCloseHandle(value); }
    }

    UniqueHandle(UniqueHandle const&)            = delete;
    UniqueHandle& operator=(UniqueHandle const&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other)
        {
            if (value != nullptr) { ::WinHttpCloseHandle(value); }
            value = std::exchange(other.value, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HINTERNET get() const noexcept { return value; }
    explicit operator bool() const noexcept { return value != nullptr; }
};

// ---------------------------------------------------------------------------
// 字符串：UTF-8 <-> UTF-16
//
// 头文件承诺"全 UTF-8 的 std::string"；WinHTTP 只吃 LPCWSTR。边界就在这两个函数，
// 别在别处散落 MultiByteToWideChar。
// ---------------------------------------------------------------------------

std::wstring widen(std::string_view utf8)
{
    if (utf8.empty()) { return {}; }

    // 两步调用是 Win32 的老规矩：第一次只为拿长度，第二次才真的转。
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) { return {}; }

    std::wstring out(static_cast<std::size_t>(length), L'\0');
    const int wrote = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    if (wrote <= 0) { return {}; }
    return out;
}

std::string narrow(std::wstring_view utf16)
{
    if (utf16.empty()) { return {}; }

    const int length = ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (length <= 0) { return {}; }

    std::string out(static_cast<std::size_t>(length), '\0');
    const int wrote = ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                                            out.data(), length, nullptr, nullptr);
    if (wrote <= 0) { return {}; }
    return out;
}

// ---------------------------------------------------------------------------
// 错误
//
// GetLastError() 只给一个 DWORD，直接塞进 message 的话，排错时你只知道"失败了"。
// ---------------------------------------------------------------------------

std::string win32Message(DWORD code)
{
    if (code == 0) { return "无错误码"; }

    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);

    std::string message;
    if (length != 0 && buffer != nullptr)
    {
        message = narrow(std::wstring_view(buffer, length));
        ::LocalFree(buffer);

        // 系统文案尾巴通常带 "\r\n"，留着会让日志一行变两行。
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
        {
            message.pop_back();
        }
    }

    if (message.empty()) { message = "Win32 错误 " + std::to_string(code); }
    return message;
}

/** WinHTTP 的错误码 → 我们的分类。未识别的交给调用方给的 fallback。 */
NetErrorCode classify(DWORD code)
{
    switch (code)
    {
    case ERROR_WINHTTP_TIMEOUT:            return NetErrorCode::Timeout;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:  return NetErrorCode::Resolve;
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:   return NetErrorCode::Connect;
    case ERROR_WINHTTP_SECURE_FAILURE:     return NetErrorCode::Tls;
    default:                               return NetErrorCode::Protocol;
    }
}

NetError errorOf(NetErrorCode fallback, DWORD lastError = ::GetLastError())
{
    const NetErrorCode classified = classify(lastError);
    NetError error;
    error.code    = classified == NetErrorCode::Protocol ? fallback : classified;
    error.message = win32Message(lastError);
    return error;
}

// ---------------------------------------------------------------------------
// WinHTTP 的三段建连
// ---------------------------------------------------------------------------

UniqueHandle openSession(ConnectOptions const& options, NetError& error)
{
    // 代理三态直接映射到 WinHTTP 的 dwAccessType：
    //   System -> 跟随注册表（DEFAULT_PROXY）
    //   Direct -> 不走代理（NO_PROXY），连本机 ollama 建议用它
    //   Manual -> NAMED_PROXY + proxyAddress
    DWORD accessType = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
    if (options.proxyMode == ConnectOptions::ProxyMode::Direct) { accessType = WINHTTP_ACCESS_TYPE_NO_PROXY; }
    if (options.proxyMode == ConnectOptions::ProxyMode::Manual) { accessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY; }

    const std::wstring agent = widen(options.userAgent);
    const std::wstring proxy = widen(options.proxyAddress);

    HINTERNET session = ::WinHttpOpen(
        agent.empty() ? L"noteAll/0.1" : agent.c_str(),
        accessType,
        accessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY ? proxy.c_str() : WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (session == nullptr) { error = errorOf(NetErrorCode::Connect); }
    return UniqueHandle(session);
}

UniqueHandle openConnection(HINTERNET              session,
                            std::wstring const&    host,
                            std::uint16_t          port,
                            ConnectOptions const&  options,
                            NetError&              error)
{
    HINTERNET connection = ::WinHttpConnect(session, host.c_str(), port, 0);
    if (connection == nullptr) { error = errorOf(NetErrorCode::Connect); return {}; }

    UniqueHandle handle(connection);

    // 超时只在这里设一次（request 里不再设，重复设置会互相覆盖）。
    // 四个参数依次是 解析 / 连接 / 发送 / 接收，单位毫秒；传 0 才是"无限等"。
    ::WinHttpSetTimeouts(handle.get(),
                         static_cast<int>(options.resolveTimeout.count()),
                         static_cast<int>(options.connectTimeout.count()),
                         static_cast<int>(options.sendTimeout.count()),
                         static_cast<int>(options.receiveTimeout.count()));

    return handle;
}

// ---------------------------------------------------------------------------
// 单次请求：建 request -> 发头 + 发体 -> 收响应
// ---------------------------------------------------------------------------

UniqueHandle sendRequest(HINTERNET              connection,
                         std::wstring const&    method,
                         std::wstring const&    target,
                         std::string_view       body,
                         HeaderList const&      headers,
                         ConnectOptions const&  options,
                         bool                   secure,
                         NetError&              error)
{
    HINTERNET raw = ::WinHttpOpenRequest(connection,
                                         method.c_str(),
                                         target.c_str(),
                                         nullptr,  // 默认 HTTP/1.1
                                         WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure ? WINHTTP_FLAG_SECURE : 0);
    if (raw == nullptr) { error = errorOf(NetErrorCode::Connect); return {}; }

    UniqueHandle request(raw);

    // 解压：WinHTTP 不会自己加 Accept-Encoding，也不解压 —— 两边都要我们显式做。
    if (options.decompress)
    {
        DWORD flag = WINHTTP_DECOMPRESSION_FLAG_GZIP;
        ::WinHttpSetOption(request.get(), WINHTTP_OPTION_DECOMPRESSION, &flag, sizeof(flag));
    }

    // 请求头逐条拼。Content-Length 由下面 WinHttpSendRequest 的长度参数负责，
    // 这里不要再塞一个，否则两处打架。
    std::wstring headerText;
    for (auto const& header : headers)
    {
        headerText += widen(header.first);
        headerText += L": ";
        headerText += widen(header.second);
        headerText += L"\r\n";
    }
    if (options.decompress) { headerText += L"Accept-Encoding: gzip\r\n"; }

    const BOOL sent = ::WinHttpSendRequest(
        request.get(),
        headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
        headerText.empty() ? 0 : -1L,
        body.empty() ? nullptr : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);

    if (!sent)
    {
        error = errorOf(NetErrorCode::Connect);
        return {};
    }
    return request;
}

bool receiveResponse(HINTERNET request, NetError& error)
{
    // 重定向策略没动：WinHTTP 默认不跟随，网关回 3xx 时这里拿到的就是那个 3xx。
    if (!::WinHttpReceiveResponse(request, nullptr))
    {
        error = errorOf(NetErrorCode::Protocol);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 读响应：状态码 + 头 + 正文
// ---------------------------------------------------------------------------

std::string trimSpace(std::string text)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t'; };
    std::size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) { ++begin; }
    std::size_t end = text.size();
    while (end > begin && isSpace(text[end - 1])) { --end; }
    return text.substr(begin, end - begin);
}

bool readHeaders(HINTERNET request, HttpResponse& response, NetError& error)
{
    // 1) 状态码：WINHTTP_QUERY_FLAG_NUMBER 让它直接填 DWORD，省得自己 parseInt。
    DWORD status     = 0;
    DWORD statusSize = sizeof(status);
    if (!::WinHttpQueryHeaders(request,
                               WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX,
                               &status,
                               &statusSize,
                               WINHTTP_NO_HEADER_INDEX))
    {
        error = errorOf(NetErrorCode::Protocol);
        return false;
    }
    response.statusCode = static_cast<int>(status);

    // 2) 全部头：第一次只拿所需字节数（会以 ERROR_INSUFFICIENT_BUFFER 失败）。
    DWORD bytes = 0;
    if (!::WinHttpQueryHeaders(request,
                               WINHTTP_QUERY_RAW_HEADERS_CRLF,
                               WINHTTP_HEADER_NAME_BY_INDEX,
                               WINHTTP_NO_OUTPUT_BUFFER,
                               &bytes,
                               WINHTTP_NO_HEADER_INDEX))
    {
        const DWORD code = ::GetLastError();
        if (code != ERROR_INSUFFICIENT_BUFFER) { error = errorOf(NetErrorCode::Protocol, code); return false; }
    }
    if (bytes == 0) { return true; }  // 没有头也是合法响应

    // 第二次真取。缓冲区按 wchar_t 个数算（bytes 是字节数，通常还含结尾 '\0'）。
    std::wstring raw(bytes / sizeof(wchar_t), L'\0');
    if (!::WinHttpQueryHeaders(request,
                               WINHTTP_QUERY_RAW_HEADERS_CRLF,
                               WINHTTP_HEADER_NAME_BY_INDEX,
                               raw.data(),
                               &bytes,
                               WINHTTP_NO_HEADER_INDEX))
    {
        error = errorOf(NetErrorCode::Protocol);
        return false;
    }

    const std::string text = narrow(raw.c_str());

    // 3) 逐行解析：第一行是状态行，跳过；其余按第一个 ':' 切。
    std::size_t start      = 0;
    bool        firstLine  = true;
    while (start < text.size())
    {
        std::size_t end = text.find("\r\n", start);
        if (end == std::string::npos) { end = text.size(); }

        const std::string line = text.substr(start, end - start);
        start = end + 2;

        if (firstLine)
        {
            firstLine = false;  // "HTTP/1.1 200 OK"
            continue;
        }
        if (line.empty()) { continue; }

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) { continue; }  // 畸形行直接跳，不让它毁掉整个响应

        const std::string name  = line.substr(0, colon);
        const std::string value = trimSpace(line.substr(colon + 1));
        if (!name.empty()) { response.headers.emplace_back(name, value); }
    }

    return true;
}

bool readBody(HINTERNET request, std::string& out, NetError& error)
{
    // 上限只是防"服务端把内存吃爆"，不是业务限制。
    constexpr std::size_t kMaxBody = 32u * 1024u * 1024u;

    std::vector<char> buffer(64u * 1024u);

    for (;;)
    {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request, &available))
        {
            error = errorOf(NetErrorCode::Timeout);
            return false;
        }
        if (available == 0) { break; }  // 读干净了；不要用 Content-Length 判断，它不可信

        const DWORD toRead = std::min(available, static_cast<DWORD>(buffer.size()));
        DWORD       read   = 0;
        if (!::WinHttpReadData(request, buffer.data(), toRead, &read))
        {
            error = errorOf(NetErrorCode::Protocol);
            return false;
        }
        if (read == 0) { break; }  // 防御：available 非 0 却一个字节都没读到

        out.append(buffer.data(), read);

        if (out.size() > kMaxBody)
        {
            error = NetError{NetErrorCode::Protocol, "响应体超过 32MB 上限"};
            return false;
        }
    }
    return true;
}

}  // namespace

// ===========================================================================
// Connect 本体
// ===========================================================================

struct Connect::Impl
{
    // 三层句柄里，request 是每次请求临时建的（在 request() 里），这里只常驻前两层。
    UniqueHandle session;
    UniqueHandle connection;

    // 建连参数留在这里：Host 头、重定向后重建 connection、日志都要用。
    std::wstring   host;
    std::uint16_t  port   = 0;
    bool           secure = true;
    ConnectOptions options;
};

Connect::Connect() = default;

// 句柄的关闭顺序由 UniqueHandle 的析构顺序保证：connection 先于 session。
// Impl 是最后一个 unique_ptr，析构时成员按声明逆序销毁 —— 正好是 connection -> session。
Connect::~Connect() = default;

Connect::Connect(Connect&&) noexcept = default;
Connect& Connect::operator=(Connect&&) noexcept = default;

// ---------------------------------------------------------------------------
// create
// ---------------------------------------------------------------------------

std::expected<Connect, NetError> Connect::create(std::string    host,
                                                 std::uint16_t  port,
                                                 bool           secure,
                                                 ConnectOptions options)
{
    Connect self;
    self.impl_ = std::make_unique<Impl>();

    Impl& impl   = *self.impl_;
    impl.host    = widen(host);
    impl.secure  = secure;
    impl.options = std::move(options);

    // port == 0 表示按 scheme 取默认端口（头文件里的约定）。
    impl.port = port != 0
                    ? port
                    : static_cast<std::uint16_t>(secure ? INTERNET_DEFAULT_HTTPS_PORT
                                                        : INTERNET_DEFAULT_HTTP_PORT);

    if (impl.host.empty())
    {
        return std::unexpected(NetError{NetErrorCode::Resolve, "host 为空或无法转换为 UTF-16"});
    }

    NetError     error{};
    UniqueHandle session = openSession(impl.options, error);
    if (!session) { return std::unexpected(std::move(error)); }
    impl.session = std::move(session);

    UniqueHandle connection = openConnection(impl.session.get(), impl.host, impl.port, impl.options, error);
    if (!connection) { return std::unexpected(std::move(error)); }
    impl.connection = std::move(connection);

    return self;
}

// ---------------------------------------------------------------------------
// request / post / get
// ---------------------------------------------------------------------------

std::expected<HttpResponse, NetError> Connect::request(std::string_view  method,
                                                       std::string_view  path,
                                                       std::string_view  body,
                                                       HeaderList const& headers)
{
    // moved-from 必须先判：移动之后 impl_ 是空的，直接解引用就是崩溃，
    // 而且崩在"发了半个请求"的位置最难查。
    if (!impl_) { return std::unexpected(NetError{NetErrorCode::Moved, "Connect 已被移动，不能再发请求"}); }

    // 头文件里的约定：一个实例单线程串行使用。这里不做锁 ——
    // 需要并发就开多个 Connect 实例（各自 create），别往这个类里塞 mutex。
    Impl& impl = *impl_;

    NetError error{};

    UniqueHandle request = sendRequest(impl.connection.get(),
                                       widen(method),
                                       widen(path),
                                       body,
                                       headers,
                                       impl.options,
                                       impl.secure,
                                       error);
    if (!request) { return std::unexpected(std::move(error)); }

    if (!receiveResponse(request.get(), error)) { return std::unexpected(std::move(error)); }

    HttpResponse response;

    if (!readHeaders(request.get(), response, error)) { return std::unexpected(std::move(error)); }

    if (!readBody(request.get(), response.body, error)) { return std::unexpected(std::move(error)); }

    return response;
}

std::expected<HttpResponse, NetError> Connect::post(std::string_view  path,
                                                    std::string_view  body,
                                                    HeaderList const& headers)
{
    // 注意：Content-Type 由调用方传（本层不认识 JSON）；
    // Content-Length 由 sendRequest 按 body.size() 自动设置。
    return request("POST", path, body, headers);
}

std::expected<HttpResponse, NetError> Connect::get(std::string_view path, HeaderList const& headers)
{
    return request("GET", path, {}, headers);
}

}  // namespace net
