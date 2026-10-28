#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

#include "app/calendar.hpp"

// 周视图的数据准备。全是纯函数：给定一周周一和一组记录，应该摊成七列。
//
// 时区固定用 Asia/Shanghai（UTC+8，没有夏令时）—— 这样断言是确定的，
// 不会因为跑测试的机器在哪个时区而变。这也正好覆盖了最要命的一类 bug：
// 「UTC 20:00 的记录，算哪一天？」答案必须是本地次日 04:00。

using namespace std::chrono;

namespace
{

const time_zone& shanghai()
{
    static const time_zone* tz = locate_zone("Asia/Shanghai");
    return *tz;
}

// 注意别把这个函数叫 day：using namespace std::chrono 之后，
// day 会和 std::chrono::day 撞名（C2872 不明确的符号）。
year_month_day ymd(int y, unsigned m, unsigned d)
{
    return year{ y } / month{ m } / std::chrono::day{ d };
}

core::Entry makeEntry(std::string text, std::optional<sys_seconds> occurred,
                      std::optional<sys_seconds> createdAt = std::nullopt)
{
    core::Entry e;
    e.id         = text;
    e.raw_text   = text;
    e.created_at = createdAt.value_or(sys_days{ ymd(2026, 10, 26) });
    if (occurred)
    {
        e.occurred = core::TimeRange{ *occurred, std::nullopt };
    }
    return e;
}

}  // namespace

TEST_CASE("mondayOf：任何一天都能回到本周一")
{
    CHECK(app::mondayOf(ymd(2026, 10, 26)) == ymd(2026, 10, 26));   // 周一自己
    CHECK(app::mondayOf(ymd(2026, 10, 29)) == ymd(2026, 10, 26));   // 周四
    CHECK(app::mondayOf(ymd(2026, 11, 1))  == ymd(2026, 10, 26));   // 周日归上一周
    CHECK(app::mondayOf(ymd(2026, 11, 2))  == ymd(2026, 11, 2));    // 下周一
}

TEST_CASE("weekDays：七天，周一到周日")
{
    const auto days = app::weekDays(ymd(2026, 10, 29));

    REQUIRE(days.size() == 7);
    CHECK(days.front() == ymd(2026, 10, 26));
    CHECK(days.back() == ymd(2026, 11, 1));

    for (std::size_t i = 0; i < days.size(); ++i)
    {
        const weekday wd{ sys_days{ days[i] } };
        CHECK(wd.iso_encoding() == i + 1);   // 1=周一 … 7=周日
    }
}

TEST_CASE("buildWeek：UTC 深夜的记录算到本地次日")
{
    // UTC 2026-10-26 20:00 = 上海 2026-10-27 04:00 → 应落在周二那一列
    std::vector<core::Entry> entries;
    entries.push_back(makeEntry("深夜", sys_days{ ymd(2026, 10, 26) } + hours{ 20 }));

    const auto columns = app::buildWeek(entries, app::mondayOf(ymd(2026, 10, 26)), shanghai());

    REQUIRE(columns.size() == 7);
    CHECK(columns[0].entries.empty());          // 周一空
    REQUIRE(columns[1].entries.size() == 1);    // 周二有
    CHECK(columns[1].day == ymd(2026, 10, 27));
}

TEST_CASE("buildWeek：没有 occurred 的记录用 created_at 归位")
{
    std::vector<core::Entry> entries;
    entries.push_back(makeEntry("只记了时间", std::nullopt,
                                sys_days{ ymd(2026, 10, 28) } + hours{ 1 }));

    const auto columns = app::buildWeek(entries, app::mondayOf(ymd(2026, 10, 26)), shanghai());
    REQUIRE(columns[2].entries.size() == 1);    // 上海 10-28 09:00
}

TEST_CASE("buildWeek：不在本周的、已删除的都不进视图")
{
    std::vector<core::Entry> entries;
    entries.push_back(makeEntry("上一周", sys_days{ ymd(2026, 10, 20) }));
    entries.push_back(makeEntry("这一周", sys_days{ ymd(2026, 10, 27) }));

    core::Entry deleted = makeEntry("删掉的", sys_days{ ymd(2026, 10, 28) });
    deleted.deleted_at  = sys_days{ ymd(2026, 10, 29) };
    entries.push_back(deleted);

    const auto columns = app::buildWeek(entries, app::mondayOf(ymd(2026, 10, 26)), shanghai());

    std::size_t total = 0;
    for (const auto& column : columns)
    {
        total += column.entries.size();
    }
    CHECK(total == 1);
    CHECK(columns[1].entries.size() == 1);
}

TEST_CASE("buildWeek：同一天内按时间升序")
{
    std::vector<core::Entry> entries;
    entries.push_back(makeEntry("下午", sys_days{ ymd(2026, 10, 27) } + hours{ 15 }));
    entries.push_back(makeEntry("早上", sys_days{ ymd(2026, 10, 27) } + hours{ 9 }));

    const auto columns = app::buildWeek(entries, app::mondayOf(ymd(2026, 10, 26)), shanghai());
    REQUIRE(columns[1].entries.size() == 2);
    CHECK(columns[1].entries[0]->raw_text == "早上");
    CHECK(columns[1].entries[1]->raw_text == "下午");
}

TEST_CASE("timeLabel：时间点 / 时段 / 全天 / 未识别")
{
    // 上海 15:00 = UTC 07:00
    auto atTime = makeEntry("a", sys_days{ ymd(2026, 10, 27) } + hours{ 7 });
    CHECK(app::timeLabel(atTime, shanghai()) == "15:00");

    auto range = makeEntry("b", sys_days{ ymd(2026, 10, 27) } + hours{ 7 });
    range.occurred->end = sys_days{ ymd(2026, 10, 27) } + hours{ 9 };
    CHECK(app::timeLabel(range, shanghai()) == "15:00–17:00");

    // 只有日期（本地 00:00 起）= 全天。
    // 注意：生产路径里 parse 产出的是"本地墙上时间"，再转成 UTC，
    // 所以上海 10-27 00:00 存下来是 UTC 10-26 16:00，不是 UTC 00:00。
    auto allDay = makeEntry("c", sys_days{ ymd(2026, 10, 26) } + hours{ 16 });
    CHECK(app::timeLabel(allDay, shanghai()) == "全天");

    auto unknown = makeEntry("d", std::nullopt);
    CHECK(app::timeLabel(unknown, shanghai()) == "时间未识别");
}

TEST_CASE("displayTitle / weekdayLabel / weekRangeLabel"){
    auto entry    = makeEntry("原文很长很长", sys_days{ ymd(2026, 10, 27) });
    entry.summary = "摘要";
    CHECK(app::displayTitle(entry) == "摘要");

    entry.summary.clear();
    CHECK(app::displayTitle(entry) == "原文很长很长");

    CHECK(app::weekdayLabel(weekday{ 1 }) == "周一");
    CHECK(app::weekdayLabel(weekday{ 7 }) == "周日");

    CHECK(app::weekRangeLabel(ymd(2026, 10, 26)) == "2026年10月26日 – 11月1日");
    CHECK(app::weekRangeLabel(ymd(2026, 12, 28)) == "2026年12月28日 – 1月3日");
}

// ---------------------------------------------------------------------------
// 24 小时时间轴：一条记录在当天的分钟位置
//
// 用户的核心要求：**没有结束时间就不许假设 1 小时**。
// 所以 placeEntry 对"只有时刻"的记录 endMinutes 必须是空。
// ---------------------------------------------------------------------------

TEST_CASE("placeEntry：只有开始时间 → 不假设时长")
{
    // 上海 15:00 = UTC 07:00
    auto entry = makeEntry("点", sys_days{ ymd(2026, 10, 27) } + hours{ 7 });

    const auto p = app::placeEntry(entry, shanghai());
    CHECK(p.hasTime);
    CHECK(p.startMinutes == 15 * 60);
    CHECK_FALSE(p.endMinutes.has_value());   // ★ 不假设 1 小时
    CHECK_FALSE(p.allDay);
}

TEST_CASE("placeEntry：写了结束时间 → 按时长给上下边")
{
    auto entry = makeEntry("段", sys_days{ ymd(2026, 10, 27) } + hours{ 7 });   // 15:00
    entry.occurred->end = sys_days{ ymd(2026, 10, 27) } + hours{ 9 };          // 17:00

    const auto p = app::placeEntry(entry, shanghai());
    CHECK(p.startMinutes == 15 * 60);
    REQUIRE(p.endMinutes.has_value());
    CHECK(*p.endMinutes == 17 * 60);
    CHECK_FALSE(p.allDay);
}

TEST_CASE("placeEntry：跨天（23:00–01:00）收在 24:00")
{
    auto entry = makeEntry("跨", sys_days{ ymd(2026, 10, 27) } + hours{ 15 });   // 上海 23:00
    entry.occurred->end = sys_days{ ymd(2026, 10, 27) } + hours{ 17 };          // 上海次日 01:00

    const auto p = app::placeEntry(entry, shanghai());
    CHECK(p.startMinutes == 23 * 60);
    REQUIRE(p.endMinutes.has_value());
    CHECK(*p.endMinutes == 1440);
}

TEST_CASE("placeEntry：只有日期 = 全天；没有 occurred = 未排时间")
{
    // 上海 10-27 00:00 = UTC 10-26 16:00
    auto allDay = makeEntry("全天", sys_days{ ymd(2026, 10, 26) } + hours{ 16 });
    const auto dayPlacement = app::placeEntry(allDay, shanghai());
    CHECK(dayPlacement.hasTime);
    CHECK(dayPlacement.allDay);
    CHECK(dayPlacement.startMinutes == 0);
    CHECK_FALSE(dayPlacement.endMinutes.has_value());

    auto unknown = makeEntry("没时间", std::nullopt);
    const auto unplaced = app::placeEntry(unknown, shanghai());
    CHECK_FALSE(unplaced.hasTime);
}

TEST_CASE("cardTitle：有摘要用摘要，没摘要就按码点截断原文")
{
    auto entry = makeEntry("这是一段很长很长的原始内容，长到卡片上肯定放不下", sys_days{ ymd(2026, 10, 27) });

    // 没摘要 → 截到 14 个码点 + 省略号（不能截在多字节字符中间，否则乱码）
    const std::string truncated = app::cardTitle(entry, 14);
    CHECK(truncated.size() > 14);
    CHECK(truncated.substr(truncated.size() - 3) == "…");
    CHECK(truncated.find("这是") == 0);

    // 有摘要 → 直接用摘要，不截
    entry.summary = "与老张开会";
    CHECK(app::cardTitle(entry) == "与老张开会");

    // title 优先于 summary（实体里有了 title 之后，列表只用它）
    entry.title = "老张";
    CHECK(app::cardTitle(entry) == "老张");
    CHECK(app::displayTitle(entry) == "老张");

    // 短文本不截
    auto shortOne = makeEntry("跑步", sys_days{ ymd(2026, 10, 27) });
    CHECK(app::cardTitle(shortOne) == "跑步");

    // 空文本兜底
    auto empty = makeEntry("", sys_days{ ymd(2026, 10, 27) });
    CHECK(app::cardTitle(empty) == "（无标题）");
}
