#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <array>
#include <optional>
#include "core/entry.hpp"

namespace parse
{
    struct ParseResult
    {
        std::optional<core::TimeRange> occurred;
        std::string remainder;
    };

    ParseResult parseTime(std::string_view raw_text, std::chrono::sys_seconds base);

    // ═══════════════════════════════════════════════════════════════════════
    // 词表
    //
    // 表管封闭集，形状管语法。解析分两段：
    //     主体（日期）—— 落在那一天
    //     后缀（时刻）—— 落在那一天的几点
    // 两段各算各的，最后合并。所以下面按这两段分开摆。
    // ═══════════════════════════════════════════════════════════════════════

    // ── 主体用的 ───────────────────────────────────────────────────────────

    inline constexpr std::array<std::string_view, 10> frontWords =
    {
        "上", "下", "这", "本", "前", "后", "昨", "今", "明", "去"
    };
    // 「周X」这个形状只认这三个单位词。
    // 月 / 年 / 天 / 日 绝不能进来：「下月一号」是「下个月一号」，
    // 不是「下周的周一」，放进来就会把它整段吃错。
    inline constexpr std::array<std::string_view, 3> weekUnits =
    {
        "周", "星期", "礼拜"
    };

    struct SimpleTime
    {
        std::string_view word_;          // 字面量活得比程序还久，不用 string
        int              timeOperation_;
        // 「昨晚」这种词自带一个时段，光靠日期拆不出来 ——
        // 「昨」+「晚」拆开的话「晚」不成立（晚饭的晚不是时间）。所以整词收在这儿。
        // 它填的是后面的时段名，用来给跟着的钟点做修正：昨晚8点 = 昨天 20:00，不是 8:00。
        std::string_view impliedPeriod_{};
    };
    // 整词。别名之间只差一个「大」字，所以每一条的长度必须自己带上 ——
    // 匹配靠 starts_with，「后天」永远匹配不上「大后天」，谁先谁后都对。
    inline constexpr std::array<SimpleTime, 8> simpleWords =
    {{
        { "今天",  0 }, { "昨天", -1 }, { "明天",  1 },
        { "后天",  2 }, { "前天", -2 },
        { "大后天",  3 }, { "大前天", -3 },
        { "昨晚", -1, "晚上" },
    }};

    // 「一 二 三 四 五 六 日」这七个字，上面几张表里都放不下，自成一表。
    //
    // 为什么不能就地写 if( c == '三' )：汉字在 UTF-8 里占 3 个字节，而一个
    // char 只装得下 1 个字节。写 '三' 会变成"多字符字面量"，MSVC 把这三个
    // 字节打包进 int（0x00E4B889），再赋给 char 就只剩最后一个字节 0x89
    // （也就是 -119），并报 C4305 / C4309 两个截断警告。
    //
    // 所以这里只能按 string_view 整字比。
    struct WeekdayWord
    {
        std::string_view     word_;
        std::chrono::weekday wd_;
    };
    inline constexpr std::array<WeekdayWord, 8> weekdayWords =
    {{
        { "一", std::chrono::Monday },    { "二", std::chrono::Tuesday },
        { "三", std::chrono::Wednesday }, { "四", std::chrono::Thursday },
        { "五", std::chrono::Friday },    { "六", std::chrono::Saturday },
        { "日", std::chrono::Sunday },
        // 「星期天 / 周天 / 礼拜天」是口语里最常写的周日，「日」反而偏书面。
        // 代价：这个词和「天气」撞头 —— 光看词表，「下周天气不错」会被读成
        // 「下周日」+「气不错」。所以 matchWeekdayWord 里把「天气」这一种挡掉。
        { "天", std::chrono::Sunday },
    }};

    // 「今年 / 明年 / 去年」用的。单独出现算那一年的 1 月 1 日；
    // 后面跟「X月Y日」时则给那个日期定年份（「今年3月5日」不再按"还没到的那个"猜）。
    struct YearWord
    {
        std::string_view word_;
        int              offset_;   // 相对基准年
    };
    inline constexpr std::array<YearWord, 3> yearWords =
    {{
        { "去年", -1 }, { "今年", 0 }, { "明年", 1 },
    }};

    // ── 后缀用的 ───────────────────────────────────────────────────────────

    // 时段词。fromMidnight_ 是这段时间的"代表时刻"：
    // 「下午」本身不带钟点，就取 14:00 当作它的代表值。
    // 后面要是跟了钟点（下午3点），这个值就不用了，改成给钟点做修正。
    struct PeriodWord
    {
        std::string_view   word_;
        std::chrono::hours fromMidnight_;
    };
    inline constexpr std::array<PeriodWord, 6> backWords =
    {{
        { "早上", std::chrono::hours{  8 } }, { "上午", std::chrono::hours{  9 } },
        { "中午", std::chrono::hours{ 12 } }, { "下午", std::chrono::hours{ 14 } },
        { "晚上", std::chrono::hours{ 20 } }, { "凌晨", std::chrono::hours{  5 } },
    }};

}
