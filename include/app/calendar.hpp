#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/entry.hpp"

// 周视图的数据准备：把「一条条记录」变成「一周七天的列」。
//
// 刻意不依赖任何 UI：这样它能被单元测试，也能被以后的别的视图（月视图、
// 今天视图）复用。GUI 只负责把这里的结果画出来。
namespace app
{

// 数据文件位置：环境变量 NOTE_FILE，默认当前目录的 notes.jsonl。
// 放在这里是为了让 CLI 和 GUI 共用同一个规则 —— 两边算出不同的路径，
// 就是「同一个程序看到两份数据」这种最难查的 bug。
std::filesystem::path dataPath();

struct DayColumn
{
    std::chrono::year_month_day day;
    std::vector<core::Entry const*> entries;   // 按时间升序
};

/** 一周七天（周一 → 周日）。anyDay 落在哪一周，就返回哪一周。 */
std::vector<std::chrono::year_month_day> weekDays(std::chrono::year_month_day anyDay);

/** 这一天这一周的周一。 */
std::chrono::year_month_day mondayOf(std::chrono::year_month_day anyDay);

/** 一条记录属于哪个本地日期：优先 occurred，没有就用 created_at。 */
std::chrono::year_month_day localDayOf(const core::Entry& entry,
                                       const std::chrono::time_zone& tz);

/**
 * 把 entries 摊到一周七列里。
 *
 * @param weekStart 该周的周一（本函数只取它当基准，不会自己去算周一）
 * @param tz        用哪个时区判"哪天" —— 显示给人看的时间，永远是本地时区
 */
std::vector<DayColumn> buildWeek(const std::vector<core::Entry>&   entries,
                                 std::chrono::year_month_day      weekStart,
                                 const std::chrono::time_zone&    tz);

// ---- 显示用的格式化 ----

std::string weekdayLabel(std::chrono::weekday wd);        // 周一 …
std::string monthDayLabel(std::chrono::year_month_day d); // 10/27

/** 时间标签："15:00" / "15:00–17:00" / "全天"（只有日期没有时刻）。 */
std::string timeLabel(const core::Entry& entry, const std::chrono::time_zone& tz);

/** 卡片上显示的文字：有摘要用摘要，没有就退回原文。 */
std::string displayTitle(const core::Entry& entry);

/** 日期区间标签："2026年10月26日 – 11月1日"。 */
std::string weekRangeLabel(std::chrono::year_month_day weekStart);

}  // namespace app
