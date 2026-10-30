#pragma once

#include <chrono>
#include <expected>
#include <string_view>

#include "core/core.hpp"

// 记录层面的"用例"：加一条记录这件事是怎么编排的。
//
// 为什么要单独一层：这件事不只是 core.add ——
//   解析中文时间 → 本地墙上时间 ↔ UTC 换算 → 再调 core.add
// CLI 和 GUI 都必须走同一条路径。放在 main.cpp 里就只能 GUI 重写一遍，
// 而两份实现迟早会因为一个时区细节分叉。
namespace app
{

/** 现在（UTC，秒精度）。 */
std::chrono::sys_seconds nowUtc();

/**
 * UTC → 本地"墙上时间"。
 *
 * 返回类型仍是 sys_seconds，但它的**值**是墙上时钟（本地 15:00 就是 15:00），
 * 只用来喂 parseTime，绝不能当时间点存进 Entry。
 */
std::chrono::sys_seconds utcToLocalWall(std::chrono::sys_seconds utc);

/**
 * 本地"墙上时间" → UTC。落盘前必须做这一步。
 *
 * 用 get_info 而不是 zoned_time 构造：后者碰到夏令时"跳过"的时刻会抛
 * nonexistent_local_time，一条笔记不该因为这种边界存不进去。
 */
std::chrono::sys_seconds localWallToUtc(std::chrono::sys_seconds wall);

/**
 * 加一条记录：解析时间 → 换算 → core.add。
 *
 * 原文原样保存；解析不出时间也照样存（occurred 留空），这是"记录优先"的底线。
 */
std::expected<core::Entry, std::string> addFromText(core::Core& note, std::string_view text);

/**
 * 只管"这句话里的时间"：解析 → 换回 UTC。解析不出返回空（不猜）。
 *
 * 改原文之后要用它重算 occurred —— 和 add 走同一套时区换算，
 * 否则"改一条"和"加一条"的同一句话会得到不同时间。
 */
std::optional<core::TimeRange> occurredFromText(std::string_view text,
                                                std::chrono::sys_seconds createdAt);

}  // namespace app
