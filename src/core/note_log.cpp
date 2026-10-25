#include "core/note_log.hpp"

#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

namespace core
{

namespace
{

char const* levelName(LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    }
    return "INFO ";
}

bool loggingDisabled() noexcept
{
    // 只认显式的 "0"：没设、设成别的值都按"开"处理 —— 默认行为必须是能查到东西。
    if (const char* flag = std::getenv("NOTE_LOG"))
    {
        const std::string_view value{flag};
        return value == "0";
    }
    return false;
}

}  // namespace

void appendLog(std::filesystem::path const& dataFile, LogLevel level, std::string_view message) noexcept
{
    // noexcept 函数里所有可能抛的（分配、格式化、IO）都自己吞掉：
    // 日志失败的后果只是"少一条记录"，而让主流程崩掉的后果是"用户丢一次操作"。
    try
    {
        if (loggingDisabled() || dataFile.empty()) { return; }

        std::filesystem::path logPath = dataFile;
        logPath += ".log";

        std::ofstream out(logPath, std::ios::binary | std::ios::app);
        if (!out) { return; }

        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

        // std::format 对 sys_time 直接按 UTC 输出，正好和 created_at 同口径。
        out << std::format("{:%Y-%m-%dT%H:%M:%SZ}", now) << "  " << levelName(level) << "  " << message
            << '\n';
    }
    catch (...)
    {
        // 故意空：日志自己没有资格失败。
    }
}

}  // namespace core
