#pragma once

#include <filesystem>
#include <string_view>

// note_log —— 只记"异常"的极简日志。
//
// 它不做的事（刻意）：
//   * 不做 Logger 抽象、不分级别表、不做轮转 —— 现在只有一个消费者（CLI 进程）；
//   * 不记录 raw_text —— 记录系统里，日志泄露正文比程序崩溃更严重；
//   * 不参与测试断言 —— 文案会改，日志不是契约。
//
// 三条硬规矩：
//   1. **永不抛异常**：日志失败必须静默，不能让"记日志"变成新的故障点；
//   2. 永远追加，不覆盖（出事时前几行往往比最后一行有用）；
//   3. 跟着数据文件走（<数据文件>.log），天然落在同一个备份/忽略范围里。
//
// 关掉它：环境变量 NOTE_LOG=0（排查问题时默认开）。
namespace core
{

enum class LogLevel
{
    Warn,
    Error,
};

/**
 * 追加一行到 <dataFile>.log。
 *
 * 格式：`2026-10-25T14:30:11Z  WARN   第 17 行解析失败，已跳过`
 * 时间用 UTC（和条目里的 created_at 同口径，跨时区排查不会错）。
 *
 * @param dataFile 数据文件路径（日志写在它旁边；空路径直接忽略）
 * @param level    级别
 * @param message  已脱敏的说明 —— **不要传 raw_text**
 */
void appendLog(std::filesystem::path const& dataFile, LogLevel level, std::string_view message) noexcept;

}  // namespace core
