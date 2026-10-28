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

// ---------------------------------------------------------------------------
// 24 小时时间轴：一条记录在当天的"分钟位置"
//
// 周视图的列不再是等高的卡片堆，而是一条 00:00→24:00 的刻度尺：
// 卡片的**上边**是开始时间、**下边**是结束时间。
//
// 关键约定（用户明确要求）：**没有结束时间就绝不假设 1 小时**。
// 那种记录只有"时刻"，没有"跨度"——UI 侧用最小可见高度把它画成一个点，
// 但高度不代表时长；跨度是 0。
// ---------------------------------------------------------------------------
struct DayPlacement
{
    bool               hasTime = false;      // 有 occurred 且带具体时刻
    bool               allDay = false;       // 只有日期（本地 00:00 起、没有 end）
    int                startMinutes = 0;     // 0..1439（本地时间）
    std::optional<int> endMinutes;           // 0..1440；跨到次日就是 1440
};

DayPlacement placeEntry(const core::Entry& entry, const std::chrono::time_zone& tz);

/** 卡片上显示的文字：有摘要用摘要，没有就退回原文。 */
std::string displayTitle(const core::Entry& entry);

/**
 * 时间轴卡片上只放"标题"：有摘要用摘要；没有摘要就把原文截到 maxCodePoints 个字。
 *
 * 为什么必须截：卡片窄（一列约 150px），原文可能是一整段话。
 * 完整内容放在详情面板里（点卡片才出现）——列表要"扫一眼"，详情才要"读全文"。
 */
std::string cardTitle(const core::Entry& entry, std::size_t maxCodePoints = 10);

/**
 * 按**像素宽度**把文字裁到能放下为止，超出补省略号。
 *
 * 为什么需要它：卡片宽度随窗口变（最大化时一列 200px，默认窗口只有 150px），
 * 所以"几个字"这种固定字数根本不靠谱 —— 必须在绘制时按可用宽度现算，
 * 否则换个窗口大小标题就溢出或被裁。
 *
 * 宽度是估算的（中日韩 1 个字宽 = fontSize，拉丁/数字约 0.56 个字宽）：
 * 宁可估大一点，也不让文字跑出卡片。
 *
 * @param maxLines 卡片里最多能放几行（按 height / lineHeight 算出来）
 */
std::string elideToFit(std::string const& text, float maxWidthPx, float fontSizePx, int maxLines);

/** 日期区间标签："2026年10月26日 – 11月1日"。 */
std::string weekRangeLabel(std::chrono::year_month_day weekStart);

}  // namespace app
