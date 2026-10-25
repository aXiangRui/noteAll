#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <string>

// 文件锁 —— 让"两个 note 进程同时写"变成排队，而不是互相覆盖。
//
// 为什么必须有：CLI 最容易被并发调用（脚本、批量导入、以后的后台补草稿）。
// 现在每次修改都是"全量读 → 改内存 → 全量重写"，两个进程交错执行就是
// **后写覆盖前写，静默丢数据**，而且丢完你根本看不出来。
//
// 用法（见 core.cpp 的 mutation 流程）：
//   1. acquire(path) 拿到独占锁；
//   2. 在锁内重新 load() 一次，用磁盘上的最新状态覆盖内存；
//   3. 改内存、save()；
//   4. 锁离开作用域自动释放。
//
// 第 2 步是关键：只加锁不重读，第二个进程仍然会拿它启动时的旧快照覆盖别人。
namespace core
{

class FileLock
{
public:
    /**
     * 获取数据文件的独占锁（锁文件是 <path>.lock）。
     *
     * 拿不到会重试到 timeout；超时返回错误，错误文案要能直接展示给用户
     * （"另一个进程正在写"这种），不要吐 Win32 错误码。
     */
    static std::expected<FileLock, std::string> acquire(
        std::filesystem::path const& dataFile,
        std::chrono::milliseconds    timeout = std::chrono::milliseconds{2000});

    ~FileLock();

    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&&)            = delete;
    FileLock(FileLock const&)                  = delete;
    FileLock& operator=(FileLock const&)       = delete;

private:
    explicit FileLock(void* handle) noexcept : handle_(handle) {}

    void release() noexcept;

    // Windows 上是 HANDLE；POSIX 上记录锁文件路径（见 .cpp）。
    void* handle_ = nullptr;
};

}  // namespace core
