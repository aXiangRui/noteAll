# timeParse 源码

> 快照，2026-10-14，M1 收尾状态（**已含当天的解析器加固**：12 点 / 0 点边界、冒号形式的时段词修正、
> 区间第二个钟点自带时段词、跨零点、日期区间、星期天、大前天 / 大后天、下下周三、今年 / 明年）。
>
> 这是**代码快照**，不是手册 —— 想知道"现在能解析什么"，看
> [parse-time.md §3.5](parse-time.md)。两个文件：
> - `include/parse/timeParse.hpp` —— 词表 + 接口
> - `src/parse/timeParse.cpp` —— 实现

---

## include/parse/timeParse.hpp

```cpp
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
    inline constexpr std::array<std::string_view, 7> unitWords =
    {
        "周", "星期", "礼拜", "月", "年", "天", "日"
    };
    // 「周X」这个形状只认这三个单位词。
    // unitWords 里另外四个（月 / 年 / 天 / 日）绝不能进来：「下月一号」
    // 是「下个月一号」，不是「下周的周一」，放进来就会把它整段吃错。
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

    // 「0点」..「24点」。留着备查，当前实现走的是数字扫描
    // （因为 15:30 / 下午3点 都得扫数字），这张表暂时没有被引用。
    inline constexpr std::array<std::string_view, 25> timeWords =
    {
        "0点",  "1点",  "2点",  "3点",  "4点",  "5点",  "6点",
        "7点",  "8点",  "9点",  "10点", "11点", "12点", "13点",
        "14点", "15点", "16点", "17点", "18点", "19点", "20点",
        "21点", "22点", "23点", "24点"
    };
}
```

## src/parse/timeParse.cpp

```cpp
#include "parse/timeParse.hpp"

namespace parse
{
    namespace
    {
        // ═══════════════════════════════════════════════════════════════════
        // 小工具
        // ═══════════════════════════════════════════════════════════════════

        // s 的 pos 位置是不是 lit 开头。pos 超界也算"不是"，不抛异常。
        bool at(std::string_view s, std::size_t pos, std::string_view lit)
        {
            return pos <= s.size() && s.substr(pos).starts_with(lit);
        }

        // 读十进制数字，返回 (值, 吃掉几个字节)。
        // 读到的位数少于 minDigits 就失败 —— 也顺手挡住了空串和"根本没数字"。
        std::optional<std::pair<int, std::size_t>> readNumber(std::string_view s, std::size_t minDigits, std::size_t maxDigits)
        {
            std::size_t n = 0;
            int         v = 0;
            while( n < maxDigits && n < s.size() && s[n] >= '0' && s[n] <= '9' )
            {
                v = v * 10 + (s[n] - '0');
                ++n;
            }
            if( n < minDigits )
            {
                return std::nullopt;
            }
            return std::make_pair(v, n);
        }

        // ═══════════════════════════════════════════════════════════════════
        // 后缀（时刻）
        // ═══════════════════════════════════════════════════════════════════

        struct ClockHit
        {
            std::chrono::minutes sinceMidnight_;
            int                  dayOffset_;   // 0 / 1：「晚上12点」是次日零点，不是当天中午
            std::size_t          length_;
        };

        struct ClockRange
        {
            std::chrono::minutes                start_;
            std::optional<std::chrono::minutes> end_;            // 空 = 时间点；非空 = 时段
            std::size_t                         length_;
            int                                 startDayOffset_;  // 语义同 ClockHit::dayOffset_
            int                                 endDayOffset_;
        };

        // 三个构造函数。字段排在一起之后，位置初始化很容易把 length_ 和 dayOffset_
        // 写反（两个都是整数，编译器一声不吭），所以统一从这里出。
        ClockRange pointRange(std::chrono::minutes start, std::size_t length)
        {
            return ClockRange{ start, std::nullopt, length, 0, 0 };
        }

        ClockRange pointHit(const ClockHit& h, std::size_t length)
        {
            return ClockRange{ h.sinceMidnight_, std::nullopt, length, h.dayOffset_, 0 };
        }

        ClockRange spanRange(const ClockHit& a, const ClockHit& b, std::size_t length)
        {
            return ClockRange{ a.sinceMidnight_, b.sinceMidnight_, length,
                               a.dayOffset_, b.dayOffset_ };
        }

        // 钟点相对当天零点的偏移（分钟），跨天的那一天也算进去。
        // 比两个钟点谁早谁晚必须用它：光看 sinceMidnight_，
        // 「晚上11点到12点半」的次日 00:30 会被当成比 23:00 还早。
        std::chrono::minutes offsetOf(const ClockHit& h)
        {
            return h.sinceMidnight_ + std::chrono::days{ h.dayOffset_ };
        }

        // 时段词对钟点的修正结果：改完的小时 + 要不要顺延一天。
        //
        // ⚠️ 12 点和 0 点是边界，不能拿"小于 12 就 +12"一把梭：
        //     中午12点 / 下午12点 / 上午12点 -> 12:00（就是中午，不动）
        //     凌晨12点                       -> 00:00（当天，凌晨是后半夜）
        //     晚上12点                       -> 次日 00:00（晚上 12 点是这一天的 24 点）
        //     晚上0点 / 0点                  -> 00:00（当天）
        struct HourFix
        {
            int hour_;
            int dayOffset_;
        };

        HourFix applyPeriod(int hour, std::string_view period)
        {
            if( period == "晚上" )
            {
                if( hour == 0 )  { return { 0, 0 }; }            // 晚上0点 = 当天零点
                if( hour == 12 ) { return { 0, 1 }; }            // 晚上12点 = 次日零点
                return { hour < 12 ? hour + 12 : hour, 0 };
            }
            if( period == "中午" || period == "下午" )
            {
                return { hour < 12 ? hour + 12 : hour, 0 };
            }
            if( period == "凌晨" )
            {
                return { hour == 12 ? 0 : hour, 0 };             // 凌晨12点 = 00:00
            }
            return { hour, 0 };   // 早上 / 上午 / 没写时段词：照字面
        }

        // 只试钟点本身，不试时段词。period 可以是空的 —— 空表示前面没写时段词。
        std::optional<ClockHit> matchClockNumber(std::string_view s, std::string_view period)
        {
            // 形式甲：15:30
            if( const auto h = readNumber(s, 1, 2); h && at(s, h->second, ":") )
            {
                const std::size_t minutePos = h->second + 1;
                const auto        m         = readNumber(s.substr(minutePos), 1, 2);
                if( !m || h->first > 23 || m->first > 59 )
                {
                    return std::nullopt;
                }
                // ⚠️ 这里也必须走 applyPeriod：「下午3:30」是 15:30，跟「下午3点半」一样。
                // 这个分支以前直接 return，于是同一个意思两种写法差 12 小时，还不报错。
                const HourFix fix = applyPeriod(h->first, period);
                if( fix.hour_ > 23 )
                {
                    return std::nullopt;
                }
                return ClockHit{ std::chrono::minutes{ fix.hour_ * 60 + m->first },
                                 fix.dayOffset_, minutePos + m->second };
            }

            // 形式乙：15点 / 9点半 / 15点30分
            const auto h = readNumber(s, 1, 2);
            if( !h || !at(s, h->second, "点") )     // 「点」占 3 字节
            {
                return std::nullopt;
            }

            std::size_t          pos   = h->second + 3;
            std::chrono::minutes extra{ 0 };

            if( at(s, pos, "半") )
            {
                extra = std::chrono::minutes{ 30 };
                pos  += 3;
            }
            else if( const auto m = readNumber(s.substr(pos), 1, 2); m && at(s, pos + m->second, "分") )
            {
                if( m->first > 59 )
                {
                    return std::nullopt;
                }
                extra = std::chrono::minutes{ m->first };
                pos  += m->second + 3;
            }

            const HourFix fix = applyPeriod(h->first, period);
            if( fix.hour_ > 23 )
            {
                return std::nullopt;
            }
            return ClockHit{ std::chrono::minutes{ fix.hour_ * 60 } + extra, fix.dayOffset_, pos };
        }

        // 开头的时段词（可选）。命中就返回词，长度写进 length。
        std::string_view takePeriod(std::string_view s, std::size_t& length)
        {
            for( const auto& p : backWords )
            {
                if( s.starts_with(p.word_) )
                {
                    length = p.word_.size();
                    return p.word_;
                }
            }
            length = 0;
            return {};
        }

        // [时段词]? [钟点]? [到|至 [时段词]? 钟点]?
        // 至少得命中一样，否则返回空（= 这里没有时刻信息）。
        //
        // impliedPeriod 是前面日期那一段留下的隐含时段（「昨晚」→「晚上」）。
        // 文字里明写了时段词就以文字为准，没写才用它。
        std::optional<ClockRange> matchClockRange(std::string_view s, std::string_view impliedPeriod)
        {
            // 开头的时段词，可选
            std::size_t      pos    = 0;
            std::string_view period = takePeriod(s, pos);
            if( period.empty() )
            {
                period = impliedPeriod;
            }

            const auto first = matchClockNumber(s.substr(pos), period);

            if( !first )
            {
                if( period.empty() )
                {
                    return std::nullopt;      // 既没时段词也没钟点
                }
                // 光有「下午」这种：取它的代表时刻
                for( const auto& p : backWords )
                {
                    if( p.word_ == period )
                    {
                        return pointRange(
                            std::chrono::duration_cast<std::chrono::minutes>(p.fromMidnight_), pos);
                    }
                }
                return std::nullopt;
            }

            pos += first->length_;

            // 「下午3点到5点」——第二个钟点没写时段词就沿用同一个，所以那个 5点 也算 17:00。
            // ⚠️ 它自己写了就用自己的：「晚上11点到凌晨1点」的 1点 是 01:00，不是 13:00。
            for( const std::string_view sep : { "到", "至" } )
            {
                if( !at(s, pos, sep) )
                {
                    continue;
                }

                const std::size_t secondStart = pos + sep.size();
                std::size_t       secondSkip  = 0;
                const std::string_view written = takePeriod(s.substr(secondStart), secondSkip);
                const std::string_view secondPeriod = written.empty() ? period : written;

                auto second = matchClockNumber(s.substr(secondStart + secondSkip), secondPeriod);
                if( !second )
                {
                    continue;
                }

                // 「晚上11点到1点」：第二个钟点没写时段词，沿用「晚上」会把 1点 读成 13:00。
                // 它比起点还早，只能靠跨天补救，跨度一下变成 14 小时 —— 显然不是这个意思。
                // 于是改按字面读（1点 = 01:00），跨天之后只剩 2 小时。
                //
                // 「下午3点到5点」不受影响：17:00 本来就晚于 15:00，压根不需要跨天，
                // 沿用时段词正是它对的原因。
                //
                // ⚠️ 比大小必须带上 dayOffset_：「晚上11点半到12点半」的 12点半 是
                // 次日 00:30（偏移 1470 分钟），光看 sinceMidnight_（30）会以为它更早，
                // 回落成当天 12:30，区间直接歪掉 12 小时。
                if( written.empty() && !period.empty() && offsetOf(*second) < offsetOf(*first) )
                {
                    if( const auto literal = matchClockNumber(s.substr(secondStart + secondSkip), {}) )
                    {
                        if( offsetOf(*literal) < offsetOf(*first) )
                        {
                            second = literal;
                        }
                    }
                }

                return spanRange(*first, *second, secondStart + secondSkip + second->length_);
            }

            // ⚠️ 长度用 pos，不要用 first->length_ ——
            // 前面的时段词（「下午」那 6 个字节）也是被吃掉的。
            return pointHit(*first, pos);
        }

        // ═══════════════════════════════════════════════════════════════════
        // 主体（日期）
        // ═══════════════════════════════════════════════════════════════════

        struct DateHit
        {
            std::chrono::sys_days day_;
            std::size_t           length_;
            std::string_view      impliedPeriod_{};   // 「昨晚」这种自带时段的词才填
        };

        // 前缀 → 周 / 月的偏移。只有这四个搭得上；前 / 后 / 昨 / 今 / 明 / 去 返回空。
        std::optional<int> prefixOffset(std::string_view pre)
        {
            if(      pre == "上" )                { return -1; }
            else if( pre == "下" )                { return  1; }
            else if( pre == "这" || pre == "本" ) { return  0; }
            return std::nullopt;
        }

        // s 开头那一段是「周」字后面那几个字。命中就返回表里那一项，否则 nullptr。
        // 返回表项而不是只返回 weekday，是因为调用方还得知道它吃掉了几个字节。
        //
        // 表本身在 timeParse.hpp，和另外几张表排在一起；这个查询只在这儿用，
        // 所以关在匿名 namespace 里，外面看不见。
        const WeekdayWord* matchWeekdayWord(std::string_view s)
        {
            for( const auto& w : weekdayWords )
            {
                if( !s.starts_with(w.word_) )
                {
                    continue;
                }
                // 「天」＝周日（口语写法），但「天气」不是星期词：
                // 「下周天气不错」必须一个都不认，不能读成「下周日」+「气不错」。
                if( w.word_ == "天" && at(s, w.word_.size(), "气") )
                {
                    continue;
                }
                return &w;
            }
            return nullptr;
        }

        // 本周的那个星期几（零点）。
        //
        // 注意：不能写 wd - weekday{ baseDay } —— weekday 相减返回的是 [0,6] 的
        // "模 7"结果，不是有符号差：周一(1) 减 周四(4) 得到的是 4 而不是 -3。
        // 用它算会整整偏一周，而且只有目标星期早于基准星期时才出错，
        // 一半用例照过，非常难查。
        std::chrono::sys_days dayOfThisWeek(std::chrono::sys_days baseDay, std::chrono::weekday wd)
        {
            const int offset = (int)wd.iso_encoding()
                             - (int)std::chrono::weekday{ baseDay }.iso_encoding();   // -6 .. 6
            return baseDay + std::chrono::days{ offset };
        }

        struct WeekHit
        {
            std::chrono::sys_days dayOfThisWeek_;   // 本周的那个周X（零点）
            std::size_t           length_;          // 「周X」一共吃掉几个字节
        };

        // 匹配一整条「[周|星期|礼拜] + [一..日]」。
        //
        // 单位词只走 weekUnits（三个），不走 unitWords（七个）——
        // 多了 月/年/天/日 就会把「下月一号」当成「下周的周一」。
        std::optional<WeekHit> matchWeekDay(std::string_view s, std::chrono::sys_days baseDay)
        {
            for( const std::string_view unit : weekUnits )
            {
                if( !s.starts_with(unit) )
                {
                    continue;
                }
                const WeekdayWord* wdWord = matchWeekdayWord(s.substr(unit.size()));
                if( wdWord == nullptr )
                {
                    continue;
                }
                return WeekHit{ dayOfThisWeek(baseDay, wdWord->wd_),
                                unit.size() + wdWord->word_.size() };
            }
            return std::nullopt;
        }

        std::optional<std::chrono::sys_days> makeDate(int y, unsigned m, unsigned d)
        {
            const std::chrono::year_month_day ymd{ std::chrono::year{ y }
                                                 / std::chrono::month{ m }
                                                 / std::chrono::day{ d } };
            if( !ymd.ok() )
            {
                return std::nullopt;      // 2月30日 这种
            }
            return std::chrono::sys_days{ ymd };
        }

        // 没写年份时取"还没到的那个"：基准年这个日子还没到就用基准年，过了就用下一年。
        // 今年压根没这一天（2月29日）就往后找 —— 闰年间隔不会超过 8 年。
        //
        // floorDay 是"不许早于哪一天"。单独解析一个日期时它就是基准日；
        // 解析「X 到 Y」里的 Y 时它是 X —— 否则「周一到周五」的周五会留在本周、
        // 周一却跳到下周，区间直接倒过来。
        std::optional<std::chrono::sys_days> pickYear(unsigned m, unsigned d, int baseYear,
                                                     std::chrono::sys_days floorDay)
        {
            for( int offset = 0; offset < 8; ++offset )
            {
                if( const auto t = makeDate(baseYear + offset, m, d); t && *t >= floorDay )
                {
                    return t;
                }
            }
            return std::nullopt;
        }

        // 绝对日期：2026-10-01 或 [2026年 | 今年 | 明年 | 去年]10月1日 或 10月1号
        std::optional<DateHit> matchAbsoluteDate(std::string_view s, std::chrono::sys_days baseDay,
                                                 std::chrono::sys_days floorDay)
        {
            const int baseYear = (int)std::chrono::year_month_day{ baseDay }.year();

            // 形式甲：2026-10-01
            if( const auto y = readNumber(s, 4, 4); y && at(s, 4, "-") )
            {
                const auto mo = readNumber(s.substr(5), 1, 2);
                if( !mo || !at(s, 5 + mo->second, "-") )
                {
                    return std::nullopt;
                }
                const std::size_t dayPos = 5 + mo->second + 1;
                const auto        d      = readNumber(s.substr(dayPos), 1, 2);
                if( !d )
                {
                    return std::nullopt;
                }
                if( const auto t = makeDate(y->first, (unsigned)mo->first, (unsigned)d->first) )
                {
                    return DateHit{ *t, dayPos + d->second };
                }
                return std::nullopt;      // 2026-02-30：写出来了但不合法，不猜
            }

            // 形式乙：[2026年 | 今年 | 明年 | 去年]10月1日
            std::size_t        pos = 0;
            std::optional<int> year;
            if( const auto y = readNumber(s, 4, 4); y && at(s, 4, "年") )
            {
                year = y->first;
                pos  = 4 + 3;             // 「年」占 3 字节
            }
            else
            {
                // 「今年」这种整词已经把「年」吃掉了，所以底下直接从「X月」接着读。
                for( const auto& w : yearWords )
                {
                    if( s.starts_with(w.word_) )
                    {
                        year = baseYear + w.offset_;
                        pos  = w.word_.size();
                        break;
                    }
                }
            }

            const auto mo = readNumber(s.substr(pos), 1, 2);
            if( !mo || !at(s, pos + mo->second, "月") )
            {
                return std::nullopt;
            }
            pos += mo->second + 3;

            const auto d = readNumber(s.substr(pos), 1, 2);
            if( !d )
            {
                return std::nullopt;
            }
            pos += d->second;

            if( at(s, pos, "日") || at(s, pos, "号") )
            {
                pos += 3;
            }
            else
            {
                return std::nullopt;      // 光有「3月5」不算日期
            }

            if( year )
            {
                if( const auto t = makeDate(*year, (unsigned)mo->first, (unsigned)d->first) )
                {
                    return DateHit{ *t, pos };
                }
                return std::nullopt;
            }
            if( const auto t = pickYear((unsigned)mo->first, (unsigned)d->first, baseYear, floorDay) )
            {
                return DateHit{ *t, pos };
            }
            return std::nullopt;
        }

        // 光写「今年 / 明年 / 去年」= 那一年的 1 月 1 日。
        // 整年是个区间，M1 这里先只给一个点，跟「月初」一个路数（不填 end）。
        //
        // 后面紧跟数字就不认：「今年10月」缺了「日 / 号」，
        // 宁可整段不解析（和「2026年10月」一致），也不许只吃掉「今年」留下半截。
        std::optional<DateHit> matchYearStart(std::string_view s, std::chrono::sys_days baseDay)
        {
            const int baseYear = (int)std::chrono::year_month_day{ baseDay }.year();

            for( const auto& w : yearWords )
            {
                if( !s.starts_with(w.word_) )
                {
                    continue;
                }
                const std::string_view after = s.substr(w.word_.size());
                if( !after.empty() && after[0] >= '0' && after[0] <= '9' )
                {
                    continue;
                }
                if( const auto t = makeDate(baseYear + w.offset_, 1u, 1u) )
                {
                    return DateHit{ *t, w.word_.size() };
                }
            }
            return std::nullopt;
        }

        // 「[上|下|这|本] + [个]月初」→ 那个月 1 号零点。
        // 想落地成"整月"是个时段（要填 end），M1 先只给一个点。
        std::optional<DateHit> matchMonthStart(std::string_view s, std::chrono::sys_days baseDay)
        {
            for( const std::string_view pre : frontWords )
            {
                const auto monthOffset = prefixOffset(pre);
                if( !monthOffset || !s.starts_with(pre) )
                {
                    continue;
                }

                std::size_t      length = pre.size();
                std::string_view after  = s.substr(length);

                if( after.starts_with("个") )      // 「下个月初」里的那个「个」
                {
                    length += 3;
                    after   = after.substr(3);
                }
                if( !after.starts_with("月初") )
                {
                    continue;
                }

                const std::chrono::year_month_day base{ baseDay };
                const std::chrono::year_month    ym =
                    base.year() / base.month() + std::chrono::months{ *monthOffset };

                return DateHit{ std::chrono::sys_days{ ym / std::chrono::day{ 1 } }, length + 6 };
            }
            return std::nullopt;
        }

        // 各种日期形状依次试。顺序就是优先级。
        //
        // floorDay = "不许早于哪一天"，只影响"取还没到的那个"这类相对规则
        // （裸周X / 裸周末 / 没写年份的月日）。单独解析一个日期时它等于 baseDay；
        // 解析「X 到 Y」里的 Y 时它是 X —— 不这么做，「周一到周五」的周一跳到下周、
        // 周五却留在本周，区间会倒过来。
        std::optional<DateHit> matchDate(std::string_view s, std::chrono::sys_days baseDay,
                                         std::chrono::sys_days floorDay)
        {
            // 形状 1：整词 今天 / 昨天 / 明天 / 后天 / 前天 / 大前天 / 大后天 / 昨晚
            for( const auto& d : simpleWords )
            {
                if( s.starts_with(d.word_) )
                {
                    return DateHit{ baseDay + std::chrono::days{ d.timeOperation_ },
                                    d.word_.size(), d.impliedPeriod_ };
                }
            }

            // 形状 2：[上|下|这|本] + ( 周X | 周末 )
            for( const std::string_view pre : frontWords )
            {
                const auto weekOffset = prefixOffset(pre);
                if( !weekOffset || !s.starts_with(pre) )
                {
                    continue;
                }

                // 「下下周三」「上上周三」＝ 同一个前缀写两遍，偏移累加。
                // 只有 上 / 下 允许叠；「这这周三」不成话，重复也只当一次。
                std::size_t      repeat = 0;
                std::string_view after  = s;
                while( after.starts_with(pre) && repeat < (*weekOffset == 0 ? 1u : 2u) )
                {
                    after.remove_prefix(pre.size());
                    ++repeat;
                }
                const int offset = *weekOffset * (int)repeat;

                if( const auto hit = matchWeekDay(after, baseDay) )
                {
                    return DateHit{ hit->dayOfThisWeek_ + std::chrono::days{ 7 * offset },
                                    pre.size() * repeat + hit->length_ };
                }
                if( after.starts_with("周末") )
                {
                    // 「周末」落在那周的周六。「上周三」是周三，「上周末」就是周六。
                    return DateHit{ dayOfThisWeek(baseDay, std::chrono::Saturday)
                                        + std::chrono::days{ 7 * offset },
                                    pre.size() * repeat + 6 };
                }
            }

            // 形状 3：裸写「周三」= 还没到的那个。
            // 今天正好就是周三就取今天 —— 所以是 `<` 而不是 `<=`。
            if( const auto hit = matchWeekDay(s, baseDay) )
            {
                std::chrono::sys_days target = hit->dayOfThisWeek_;
                if( target < floorDay )
                {
                    target += std::chrono::days{ 7 };
                }
                return DateHit{ target, hit->length_ };
            }

            // 形状 3b：裸写「周末」，同样取还没到的那个
            if( s.starts_with("周末") )
            {
                std::chrono::sys_days target = dayOfThisWeek(baseDay, std::chrono::Saturday);
                if( target < floorDay )
                {
                    target += std::chrono::days{ 7 };
                }
                return DateHit{ target, 6 };
            }

            // 形状 4：绝对日期
            if( const auto absolute = matchAbsoluteDate(s, baseDay, floorDay) )
            {
                return absolute;
            }

            // 形状 5：[上|下|这|本] + [个]月初
            if( const auto monthStart = matchMonthStart(s, baseDay) )
            {
                return monthStart;
            }

            // 形状 6：今年 / 明年 / 去年 -> 那一年的 1 月 1 日
            return matchYearStart(s, baseDay);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 主体 + 后缀 → 合并
    // ═══════════════════════════════════════════════════════════════════════

    ParseResult parseTime(std::string_view raw_text, std::chrono::sys_seconds base)
    {
        const std::chrono::sys_days baseDay = std::chrono::floor<std::chrono::days>(base);

        // ── 主体：先试单个日期，再试「日期 到 日期」 ──
        const auto date = matchDate(raw_text, baseDay, baseDay);

        std::size_t                          pos = date ? date->length_ : 0;
        std::optional<std::chrono::sys_days> dateEnd;

        if( date )
        {
            for( const std::string_view sep : { "到", "至" } )
            {
                if( !at(raw_text, pos, sep) )
                {
                    continue;
                }
                const std::size_t secondStart = pos + sep.size();
                // 第二个日期的"不许早于"是第一个日期，否则「周一到周五」会倒过来。
                // 万一对出来的日期仍然更早（「明天到昨天」这种），就整个不吃这段区间 ——
                // 宁可退回"单个日期 + 原文留在剩余里"，也不产出 end < start 的区间。
                const auto second = matchDate(raw_text.substr(secondStart), baseDay, date->day_);
                if( !second || second->day_ < date->day_ )
                {
                    continue;
                }
                dateEnd = second->day_;
                pos     = secondStart + second->length_;
                break;
            }
        }

        // ── 后缀：时刻（可以带自己的时段词，也可以是个区间）──
        const auto clock = matchClockRange(raw_text.substr(pos),
                                           date ? date->impliedPeriod_ : std::string_view{});
        if( clock )
        {
            pos += clock->length_;
        }

        // 两段都没命中 = 这里根本没有时间信息。一个字都不许动。
        if( !date && !clock )
        {
            return ParseResult{ std::nullopt, std::string{ raw_text } };
        }

        // 缺哪段补哪段：没写日期就用基准那天，没写时刻就用零点。
        const std::chrono::sys_days day = date ? date->day_ : baseDay;
        const std::chrono::minutes  at  = clock ? clock->start_ : std::chrono::minutes{ 0 };

        // 「晚上12点」这类落在次日的时刻靠 dayOffset_ 把日期推一天。
        const int startDayOffset = clock ? clock->startDayOffset_ : 0;

        const std::chrono::sys_seconds start{ day + at + std::chrono::days{ startDayOffset } };
        std::optional<std::chrono::sys_seconds> end;

        if( clock && clock->end_ )
        {
            std::chrono::sys_seconds e{ day + *clock->end_
                                      + std::chrono::days{ clock->endDayOffset_ } };

            // 「23点到1点」：终点早于起点就是跨了零点，往后推一天。
            // 「9点到9点」这种正好相等的原样留着，不擅自补成 24 小时。
            if( e < start )
            {
                e += std::chrono::days{ 1 };
            }
            end = e;
        }
        else if( dateEnd )
        {
            // 日期区间：半开区间，终点是第二个日期的零点。
            // 同时写了钟点区间（「周一到周五下午3点到5点」）时钟点区间优先 ——
            // 它离得更近、写得更具体。
            end = std::chrono::sys_seconds{ *dateEnd };
        }

        core::TimeRange range{ start, end };
        return ParseResult{ range, std::string{ raw_text.substr(pos) } };
    }
}
```
