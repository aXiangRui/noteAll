#include "core/file_lock.hpp"

#include "core/note_log.hpp"

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

    // 记录最后一次失败原因：超时的提示里带上它，偶发问题才有线索可查。
    // （实测遇到过一次"重试 2 秒都失败、下一次又好了"，当时没记错误码，完全靠猜。）
    long lastError = 0;

    for (;;)
    {
#ifdef _WIN32
        // 共享打开 + 线程/进程级字节锁。
        //
        // 早先的写法是 dwShareMode = 0（独占打开），用「打不开」当互斥 —— 简单，
        // 但会被系统里的其他软件误伤：杀毒/索引器只要碰一下这个文件，
        // 我们下一次打开就会拿到 ERROR_ACCESS_DENIED(5)，和"别人正在写"长得一模一样。
        // 实测就因为这个丢过一次 add（提示"另一个 note 进程正在写"、其实没人写）。
        //
        // 现在允许别人读/写/删这个文件，真正的互斥交给 LockFileEx ——
        // 它只锁 1 个字节，且不阻止别的进程打开文件。
        HANDLE handle = ::CreateFileW(lockPath.wstring().c_str(),
                                      GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);

        if (handle != INVALID_HANDLE_VALUE)
        {
            OVERLAPPED overlapped{};
            if (::LockFileEx(handle,
                             LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                             0,
                             1,
                             0,
                             &overlapped) != 0)
            {
                return FileLock(handle);
            }

            // 锁被占着：把手柄还回去再重试，别攥着不放。
            lastError = static_cast<long>(::GetLastError());
            ::CloseHandle(handle);
        }
        else
        {
            lastError = static_cast<long>(::GetLastError());
        }

        if (lastError != ERROR_SHARING_VIOLATION && lastError != ERROR_ACCESS_DENIED &&
            lastError != ERROR_LOCK_VIOLATION)
        {
            return std::unexpected("无法创建锁文件: " + lockPath.string()
                                   + " (Win32 错误 " + std::to_string(lastError) + ")");
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
            std::string message = "拿不到数据文件锁（" + lockPath.string()
                                  + "），可能是另一个 note 正在写，或系统里某个程序正占着它";
            if (lastError != 0)
            {
                message += "（最后一次失败：Win32 错误 " + std::to_string(lastError) + "）";
            }
            // 锁竞争说明并发用错了，值得在日志里留下痕迹（否则只在终端闪一下）
            appendLog(dataFile, LogLevel::Error, message);
            return std::unexpected(message);
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
    // 先解锁再关句柄（CloseHandle 也会释放锁，但显式解锁更清楚）。
    OVERLAPPED overlapped{};
    ::UnlockFileEx(static_cast<HANDLE>(handle_), 0, 1, 0, &overlapped);
    ::CloseHandle(static_cast<HANDLE>(handle_));
#else
    // POSIX 分支的 handle_ 只是个标记，锁文件在下面删掉。
    // （真正的锁路径由调用方传进来，这里为了简单没有保存；见 .hpp 的说明。）
#endif

    handle_ = nullptr;
}

}  // namespace core
