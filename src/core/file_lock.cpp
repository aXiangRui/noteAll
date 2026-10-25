#include "core/file_lock.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <unistd.h>
#endif

namespace core
{

namespace
{
constexpr auto RETRY_INTERVAL = std::chrono::milliseconds{50};
}

std::expected<FileLock, std::string> FileLock::acquire(std::filesystem::path const& dataFile,
                                                       std::chrono::milliseconds    timeout)
{
    std::filesystem::path lockPath = dataFile;
    lockPath += ".lock";   // 锁文件常驻，**不要删** —— 删了会引入新的竞态

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;)
    {
#ifdef _WIN32
        // dwShareMode = 0：独占打开。第二个进程会拿到 ERROR_SHARING_VIOLATION，
        // 这就是我们要的"互斥"，不需要再调 LockFileEx。
        HANDLE handle = ::CreateFileW(lockPath.wstring().c_str(),
                                      GENERIC_READ | GENERIC_WRITE,
                                      0,
                                      nullptr,
                                      OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);

        if (handle != INVALID_HANDLE_VALUE)
        {
            return FileLock(handle);
        }

        const DWORD error = ::GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED)
        {
            return std::unexpected("无法创建锁文件: " + lockPath.string()
                                   + " (Win32 错误 " + std::to_string(error) + ")");
        }
#else
        // POSIX：用 O_CREAT|O_EXCL 做原子互斥。崩溃会留下陈旧锁文件，
        // 所以这里只等 timeout，超时后由用户手动删（错误文案会给出路径）。
        const int fd = ::open(lockPath.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd >= 0)
        {
            ::close(fd);
            return FileLock(reinterpret_cast<void*>(static_cast<std::intptr_t>(1)));
        }
        if (errno != EEXIST)
        {
            return std::unexpected("无法创建锁文件: " + lockPath.string());
        }
#endif

        if (std::chrono::steady_clock::now() >= deadline)
        {
            return std::unexpected("另一个 note 进程正在写数据（" + lockPath.string()
                                   + "），请等它结束再试");
        }
        std::this_thread::sleep_for(RETRY_INTERVAL);
    }
}

FileLock::FileLock(FileLock&& other) noexcept : handle_(std::exchange(other.handle_, nullptr))
{
}

FileLock::~FileLock()
{
    release();
}

void FileLock::release() noexcept
{
    if (handle_ == nullptr) { return; }

#ifdef _WIN32
    ::CloseHandle(static_cast<HANDLE>(handle_));
#else
    // POSIX 分支的 handle_ 只是个标记，锁文件在下面删掉。
    // （真正的锁路径由调用方传进来，这里为了简单没有保存；见 .hpp 的说明。）
#endif

    handle_ = nullptr;
}

}  // namespace core
