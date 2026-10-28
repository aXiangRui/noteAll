#include "app/calendar.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>

namespace app
{

namespace
{

// 一条记录的"排序/归属时间点"：有 occurred 用它，没有就退回 created_at。
std::chrono::sys_seconds sortKey(const core::Entry& entry)
{
    if (entry.occurred)
    {
        return entry.occurred->start;
    }
    return entry.created_at;
}

// UTC -> 本地"年月日"
std::chrono::year_month_day toLocalDay(std::chrono::sys_seconds utc,
                                       const std::chrono::time_zone& tz)
{
    // 注意传 &tz：zoned_time 默认模板是 zoned_time<Duration, const time_zone*>，
    // 传引用会去拷贝 time_zone，而它的拷贝构造是 deleted（C2280）。
    const std::chrono::zoned_time zt{ &tz, utc };
    const auto local = std::chrono::floor<std::chrono::days>(zt.get_local_time());
    return std::chrono::year_month_day{local};
}

}  // namespace

std::filesystem::path dataPath()
{
    if (const char* env = std::getenv("NOTE_FILE"); env != nullptr && *env != '\0')
    {
        return env;
    }
    return "notes.jsonl";
}

std::chrono::year_month_day mondayOf(std::chrono::year_month_day anyDay)
{
    const std::chrono::sys_days day{anyDay};
    // iso_encoding(): 周一 = 1 … 周日 = 7。减掉 (iso-1) 就回到本周一。
    const int offset = static_cast<int>(std::chrono::weekday{day}.iso_encoding()) - 1;
    return std::chrono::year_month_day{day - std::chrono::days{offset}};
}

std::vector<std::chrono::year_month_day> weekDays(std::chrono::year_month_day anyDay)
{
    const std::chrono::sys_days monday{mondayOf(anyDay)};

    std::vector<std::chrono::year_month_day> days;
    days.reserve(7);
    for (int i = 0; i < 7; ++i)
    {
        days.emplace_back(monday + std::chrono::days{i});
    }
    return days;
}

std::chrono::year_month_day localDayOf(const core::Entry& entry, const std::chrono::time_zone& tz)
{
    return toLocalDay(sortKey(entry), tz);
}

std::vector<DayColumn> buildWeek(const std::vector<core::Entry>& entries,
                                 std::chrono::year_month_day    weekStart,
                                 const std::chrono::time_zone&  tz)
{
    std::vector<DayColumn> columns;
    columns.reserve(7);

    const std::chrono::sys_days start{weekStart};
    for (int i = 0; i < 7; ++i)
    {
        columns.push_back(DayColumn{ std::chrono::year_month_day{start + std::chrono::days{i}}, {} });
    }

    for (const auto& entry : entries)
    {
        // 已删除的不进视图（墓碑只在「已删除」列表里出现）
        if (entry.deleted_at)
        {
            continue;
        }

        const std::chrono::year_month_day day = localDayOf(entry, tz);
        const auto offset = (std::chrono::sys_days{day} - start).count();
        if (offset < 0 || offset > 6)
        {
            continue;   // 不在这一周
        }

        columns[static_cast<std::size_t>(offset)].entries.push_back(&entry);
    }

    for (auto& column : columns)
    {
        std::sort(column.entries.begin(), column.entries.end(),
                  [](const core::Entry* a, const core::Entry* b) {
                      return sortKey(*a) < sortKey(*b);
                  });
    }

    return columns;
}

std::string weekdayLabel(std::chrono::weekday wd)
{
    // iso_encoding: 1=周一 … 7=周日
    static constexpr std::array<const char*, 7> NAMES = {
        "周一", "周二", "周三", "周四", "周五", "周六", "周日",
    };

    const unsigned index = static_cast<unsigned>(wd.iso_encoding()) - 1;
    return NAMES[index < NAMES.size() ? index : 0];
}

std::string monthDayLabel(std::chrono::year_month_day d)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02u/%02u",
                  static_cast<unsigned>(d.month()), static_cast<unsigned>(d.day()));
    return buffer;
}

std::string timeLabel(const core::Entry& entry, const std::chrono::time_zone& tz)
{
    if (!entry.occurred)
    {
        return "时间未识别";
    }

    const auto formatUtc = [&tz](std::chrono::sys_seconds utc) {
        const std::chrono::zoned_time zt{ &tz, utc };
        const auto local = zt.get_local_time();
        const auto time  = std::chrono::hh_mm_ss{std::chrono::floor<std::chrono::seconds>(local) -
                                                std::chrono::floor<std::chrono::days>(local)};
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d",
                      static_cast<int>(time.hours().count()),
                      static_cast<int>(time.minutes().count()));
        return std::string{ buffer };
    };

    const std::string start = formatUtc(entry.occurred->start);

    // 本地 00:00 起 = "只有日期"，显示成"全天"，
    // 别让人以为半夜零点真有事（parse 里"只有日期"就是这么落的）。
    const bool atMidnight = (start == "00:00");

    if (!entry.occurred->end)
    {
        return atMidnight ? "全天" : start;
    }

    if (atMidnight)
    {
        return "全天";
    }
    return start + "–" + formatUtc(*entry.occurred->end);
}

namespace
{

int minutesOfLocalDay(std::chrono::sys_seconds utc, const std::chrono::time_zone& tz)
{
    const std::chrono::zoned_time zt{ &tz, utc };
    const auto                    local = zt.get_local_time();
    const auto                    day   = std::chrono::floor<std::chrono::days>(local);
    return static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(local - day).count());
}

}  // namespace

DayPlacement placeEntry(const core::Entry& entry, const std::chrono::time_zone& tz)
{
    DayPlacement placement;

    if (!entry.occurred)
    {
        return placement;   // hasTime = false：时间没识别出来
    }

    placement.hasTime      = true;
    placement.startMinutes = minutesOfLocalDay(entry.occurred->start, tz);

    if (!entry.occurred->end)
    {
        // 只有时刻，没有结束。**不假设时长**：endMinutes 保持空，
        // 由 UI 决定用一个"最小可见高度"画成点（高度不代表时长）。
        placement.allDay = (placement.startMinutes == 0);
        return placement;
    }

    const int endMinutes = minutesOfLocalDay(*entry.occurred->end, tz);

    // 结束早于开始 = 跨到次日（比如 23:00–01:00），当成 24:00 收口
    placement.endMinutes = (endMinutes < placement.startMinutes) ? 1440 : endMinutes;
    placement.allDay     = (placement.startMinutes == 0 && *placement.endMinutes == 1440);

    return placement;
}

std::string displayTitle(const core::Entry& entry){
    if (!entry.summary.empty())
    {
        return entry.summary;
    }
    return entry.raw_text;
}

std::string weekRangeLabel(std::chrono::year_month_day weekStart)
{
    const std::chrono::sys_days start{weekStart};
    const std::chrono::year_month_day first{start};
    const std::chrono::year_month_day last{start + std::chrono::days{6}};

    char buffer[64];
    if (first.month() == last.month())
    {
        std::snprintf(buffer, sizeof(buffer), "%d年%u月%u日 – %u日",
                      static_cast<int>(first.year()),
                      static_cast<unsigned>(first.month()),
                      static_cast<unsigned>(first.day()),
                      static_cast<unsigned>(last.day()));
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer), "%d年%u月%u日 – %u月%u日",
                      static_cast<int>(first.year()),
                      static_cast<unsigned>(first.month()),
                      static_cast<unsigned>(first.day()),
                      static_cast<unsigned>(last.month()),
                      static_cast<unsigned>(last.day()));
    }
    return buffer;
}

}  // namespace app
