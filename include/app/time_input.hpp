#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

// 手动改时间：把"人写的文本"和"UTC 时刻"互相转换。
//
// 为什么不用自然语言解析器（parse::parseTime）：
//   parseTime 是给"原句"用的（认"明天下午三点"），在编辑框里它会**忽略年月日**，
//   反而认不出 "2026-10-30 08:30"。编辑框要的是**严格、可预期**：
//   写什么就是什么，写错了明确报错，而不是猜。
namespace app
{

/** UTC → 本地可编辑文本："2026-10-30 08:30"。 */
std::string formatLocalDateTime(std::chrono::sys_seconds      utc,
                                const std::chrono::time_zone& tz);

/**
 * 本地文本 → UTC。认这几种写法：
 *   "2026-10-30 08:30"  完整日期+时刻
 *   "2026-10-30"        只有日期（当天 00:00）
 *   "10-30 08:30"       缺年份 → 取 fallback 的年份
 *   "08:30" / "8:30"    只有时刻 → 取 fallback 的本地日期
 *
 * 认不出返回空（调用方负责报错），**绝不猜**。
 *
 * @param tz 显式传入而不是内部取 current_zone()：这样测试能用固定时区，
 *           不会因为跑测试的机器在哪而随机挂。
 */
std::optional<std::chrono::sys_seconds> parseLocalDateTime(std::string_view               text,
                                                           std::chrono::sys_seconds      fallback,
                                                           const std::chrono::time_zone& tz);

/** 同上，用系统当前时区（给 UI 用）。 */
std::optional<std::chrono::sys_seconds> parseLocalDateTime(std::string_view          text,
                                                           std::chrono::sys_seconds fallback);

}  // namespace app
