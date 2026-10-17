#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <string_view>

#include "parse/timeParse.hpp"

// parse（中文时间解析）的单元测试。
// 对照手册：docs/parse-time.md；验收标准：docs/ROADMAP.md §3。
//
// 覆盖 M1 的全部六种形状：
//     形状 1  整词         今天 / 昨天 / 明天 / 后天 / 前天
//     形状 2  前缀 + 周X    上周三 / 下周一 / 这周五
//     形状 3  裸周X         周三
//     形状 4  绝对日期      3月5日 / 2026年10月1日 / 2026-10-01
//     形状 5  时段词        早上 / 下午
//     形状 6  钟点          15点 / 15:30 / 9点半 / 下午3点
//
// 解析分「主体（日期）+ 后缀（时刻）」两段，各算各的，最后合并 ——
// 所以「上周三下午」这种组合是**两段拼起来的**，不是一条规则吃下来的。

using namespace std::chrono;

namespace
{
    // 所有用例共用的基准时刻：2026-10-08（周四）14:00。
    //
    // 这个值必须写死。绝不能用 system_clock::now() —— 否则半夜跑测试
    // 跨了一天，昨天就变了，测试会随机挂。见 DESIGN.md §8.4。
    constexpr sys_seconds BASE = sys_days{ 2026y / 10 / 8d } + 14h;

    // 拼一个当天零点的时刻：date(2026, 10, 8) → 2026-10-08 00:00:00
    constexpr sys_seconds date(int y, unsigned m, unsigned d)
    {
        return sys_days{ year{ y } / month{ m } / day{ d } };
    }

    // 只关心「解析出来的时刻」时用它。解析不出来会直接判该用例失败，
    // 不会让后面的 -> 解空 optional（那是未定义行为，会崩）。
    sys_seconds startedAt(std::string_view text, sys_seconds base = BASE)
    {
        const auto r = parse::parseTime(text, base);
        REQUIRE(r.occurred.has_value());
        return r.occurred->start;
    }
}

// ---------------------------------------------------------------------------
// 形状 1：整词查表
// ---------------------------------------------------------------------------

TEST_CASE("五个整词各归各位")
{
    CHECK(startedAt("今天") == date(2026, 10, 8));
    CHECK(startedAt("昨天") == date(2026, 10, 7));
    CHECK(startedAt("明天") == date(2026, 10, 9));
    CHECK(startedAt("后天") == date(2026, 10, 10));
    CHECK(startedAt("前天") == date(2026, 10, 6));
}

TEST_CASE("base 的时分秒被丢掉，结果落在当天零点")
{
    // BASE 是 14:00，但「今天」指的是整天，不是某个时刻
    CHECK(startedAt("今天") == date(2026, 10, 8));
    CHECK(startedAt("今天") != BASE);
}

// ---------------------------------------------------------------------------
// remainder：抠掉时间词之后剩下的文本
// ---------------------------------------------------------------------------

TEST_CASE("remainder 正好抠掉时间词那几个字节")
{
    CHECK(parse::parseTime("今天和小李吃饭", BASE).remainder == "和小李吃饭");
    CHECK(parse::parseTime("昨天没睡好", BASE).remainder == "没睡好");
    CHECK(parse::parseTime("明天开会", BASE).remainder == "开会");
}

TEST_CASE("整句就是时间词时，remainder 是空串不是错")
{
    CHECK(parse::parseTime("今天", BASE).remainder.empty());
    CHECK(parse::parseTime("前天", BASE).remainder.empty());
}

// ---------------------------------------------------------------------------
// TimeRange：时间点 vs 时段
//
// end 空 = 时间点（M1 大多数情况）；end 非空 = 时段（「3点到5点」那种）。
// ---------------------------------------------------------------------------

TEST_CASE("没写范围的，end 是空的")
{
    const std::string texts[] = { "今天开会", "周三", "下午跑步", "3月5日" };

    for( const auto& t : texts )
    {
        CAPTURE(t);
        const auto r = parse::parseTime(t, BASE);
        REQUIRE(r.occurred.has_value());
        CHECK_FALSE(r.occurred->end.has_value());
    }
}

// ---------------------------------------------------------------------------
// 跨边界：这几条是 floor<days> 和小学生日历的混合考题
// ---------------------------------------------------------------------------

TEST_CASE("跨月")
{
    CHECK(startedAt("明天", date(2026, 10, 31)) == date(2026, 11, 1));
    CHECK(startedAt("昨天", date(2026, 11, 1)) == date(2026, 10, 31));
}

TEST_CASE("跨年")
{
    CHECK(startedAt("明天", date(2026, 12, 31)) == date(2027, 1, 1));
    CHECK(startedAt("昨天", date(2027, 1, 1)) == date(2026, 12, 31));
}

TEST_CASE("闰年")
{
    CHECK(startedAt("明天", date(2028, 2, 28)) == date(2028, 2, 29));  // 2028 是闰年
    CHECK(startedAt("明天", date(2026, 2, 28)) == date(2026, 3, 1));   // 2026 不是
}

// ---------------------------------------------------------------------------
// 该解析失败的样子 —— 这类用例在整个 M1 里至少要占 5 个
// ---------------------------------------------------------------------------

TEST_CASE("没有时间词：occurred 留空，原文一个字都不许动")
{
    const std::string texts[] =
    {
        "最近有点迷茫",
        "呵呵呵呵",
        "和小李讨论了方案",
        "abcdefg",
        "",
    };

    for (const auto& t : texts)
    {
        CAPTURE(t);
        const auto r = parse::parseTime(t, BASE);
        CHECK_FALSE(r.occurred.has_value());
        CHECK(r.remainder == t);
    }
}

TEST_CASE("时间写在中间 —— M1 只从开头解析，中间的先不管")
{
    const auto r = parse::parseTime("和小李今天讨论了方案", BASE);
    CHECK_FALSE(r.occurred.has_value());
    CHECK(r.remainder == "和小李今天讨论了方案");
}

TEST_CASE("半截词不算：「今」单独出现不该命中")
{
    // 「今」只是别的词的第一个字，自己不成时间词 —— 原文一个字都不许动。
    // （「今年」是支持的，见下面的形状 6 用例，所以这条不能再拿它当例子。）
    const std::string texts[] = { "今", "明", "昨", "本" };

    for (const auto& t : texts)
    {
        CAPTURE(t);
        const auto r = parse::parseTime(t, BASE);
        CHECK_FALSE(r.occurred.has_value());
        CHECK(r.remainder == t);
    }
}

// ---------------------------------------------------------------------------
// 形状 2：[上|下|这|本] + [周|星期|礼拜] + [一..日]
//
// BASE 是 2026-10-08 周四，所以本周的 周一..周日 = 10-05 .. 10-11。
// 下面每一行都按「本周 ± 7 天」手推过，别改期望值来迁就实现。
// ---------------------------------------------------------------------------

TEST_CASE("形状 2：上周 / 本周 / 下周")
{
    CHECK(startedAt("上周一") == date(2026, 9, 28));
    CHECK(startedAt("上周三") == date(2026, 9, 30));
    CHECK(startedAt("上周日") == date(2026, 10, 4));

    CHECK(startedAt("本周一") == date(2026, 10, 5));
    CHECK(startedAt("本周三") == date(2026, 10, 7));
    CHECK(startedAt("这周五") == date(2026, 10, 9));
    CHECK(startedAt("本周日") == date(2026, 10, 11));

    CHECK(startedAt("下周一") == date(2026, 10, 12));
    CHECK(startedAt("下周三") == date(2026, 10, 14));
    CHECK(startedAt("下周日") == date(2026, 10, 18));
}

TEST_CASE("形状 2：目标星期早于基准星期时，要往前数而不是往后数")
{
    // BASE 是周四。目标是一/二/三 时必须往本周开头走。
    // 如果实现里误用 weekday - weekday（返回 [0,6] 的模 7 值），
    // 这三个会算到下一周去，而 四/五/六/日 却照过 —— 所以单独盯住这个方向。
    CHECK(startedAt("本周一") == date(2026, 10, 5));    // 不是 10-12
    CHECK(startedAt("本周二") == date(2026, 10, 6));    // 不是 10-13
    CHECK(startedAt("本周三") == date(2026, 10, 7));    // 不是 10-14
}

TEST_CASE("形状 2：周 / 星期 / 礼拜 三个单位词等价")
{
    CHECK(startedAt("上星期三") == date(2026, 9, 30));
    CHECK(startedAt("下星期一") == date(2026, 10, 12));
    CHECK(startedAt("这礼拜五") == date(2026, 10, 9));
}

TEST_CASE("形状 2：这 和 本 是同一个意思")
{
    CHECK(startedAt("这周三") == startedAt("本周三"));
    CHECK(startedAt("这周日") == startedAt("本周日"));
}

TEST_CASE("形状 2：跨月 / 跨年")
{
    // 2026-01-01 是周四，它所在的 ISO 周从 2025-12-29 周一算起，
    // 所以「上周三」落在 2025-12-24。
    CHECK(startedAt("上周三", date(2026, 1, 1)) == date(2025, 12, 24));

    // 2026-12-31 也是周四，它那一周的「下周一」跨到 2027。
    CHECK(startedAt("下周一", date(2026, 12, 31)) == date(2027, 1, 4));
}

TEST_CASE("形状 2：remainder 抠掉「前缀 + 单位字 + 星期字」")
{
    // 「下午」现在会被后缀那段吃掉，所以 remainder 只剩后面半句。
    // 加前缀那会儿这里期望的还是「下午和小李讨论了方案」—— 组合能力上来之后，
    // 时间词就该被完整抠掉，这正是 M1 想要的行为。
    CHECK(parse::parseTime("上周三下午和小李讨论了方案", BASE).remainder == "和小李讨论了方案");
    CHECK(parse::parseTime("下周一", BASE).remainder.empty());
    CHECK(parse::parseTime("这周五交报告", BASE).remainder == "交报告");
}

TEST_CASE("形状 2：认不出的不许硬吃")
{
    const std::string texts[] =
    {
        "下周八",      // 八 不在 一二三四五六日 里
        "上周",        // 有前缀有单位，就是缺星期字
        "本来就这样",   // 「本」后面是「来」
        "后来我才知道", // 「后」压根不接「周X」
    };

    for (const auto& t : texts)
    {
        CAPTURE(t);
        const auto r = parse::parseTime(t, BASE);
        CHECK_FALSE(r.occurred.has_value());
        CHECK(r.remainder == t);
    }
}

// ---------------------------------------------------------------------------
// 形状 3：裸写「周X」= 还没到的那个
//
// BASE 是 2026-10-08 周四，所以本周 = 10-05 .. 10-11：
//     周一/二/三 已经过去 → 取下一周
//     周四（就是今天）/五/六/日 还没到 → 取本周
// ---------------------------------------------------------------------------

TEST_CASE("形状 3：过去的往后跳一周，没到的就在本周")
{
    CHECK(startedAt("周四") == date(2026, 10, 8));    // 今天，不跳
    CHECK(startedAt("周五") == date(2026, 10, 9));
    CHECK(startedAt("周六") == date(2026, 10, 10));
    CHECK(startedAt("周日") == date(2026, 10, 11));

    CHECK(startedAt("周一") == date(2026, 10, 12));   // 不是 10-05，那个已经过了
    CHECK(startedAt("周二") == date(2026, 10, 13));
    CHECK(startedAt("周三") == date(2026, 10, 14));
}

TEST_CASE("形状 3：星期 / 礼拜 也认")
{
    CHECK(startedAt("星期三") == date(2026, 10, 14));
    CHECK(startedAt("礼拜一") == date(2026, 10, 12));
}

TEST_CASE("形状 3：加了前缀就按前缀走，不许被「还没到的那个」抢走")
{
    // 形状 2 排在形状 3 前面。如果顺序反了或者前缀没拦住，
    // 「上周三」会被算成本周的 10-07 甚至下周的 10-14。
    CHECK(startedAt("上周三") == date(2026, 9, 30));
    CHECK(startedAt("本周一") == date(2026, 10, 5));   // 不是 10-12
    CHECK(startedAt("这周五") == date(2026, 10, 9));
}

TEST_CASE("形状 3：remainder")
{
    CHECK(parse::parseTime("周三下午开会", BASE).remainder == "开会");   // 「下午」被后缀吃掉了
    CHECK(parse::parseTime("周日", BASE).remainder.empty());
}

TEST_CASE("形状 3：单位词限定在 周 / 星期 / 礼拜")
{
    // 「月」「年」「天」「日」不是「周X」的单位词。
    // 「下月一号」是「下个月一号」（形状 4 的活儿），绝不能被这里截成「下周的周一」。
    const std::string texts[] =
    {
        "下月一号交房租",
        "本月三日",
        "下周八",          // 有周X的样子，但「八」不在一二三四五六日里
        "周",
        // 「明年的计划」原来在这张表里。现在「明年」是支持的（见形状 6 的用例），
        // 所以它挪走了 —— 这条不是放宽，是需求变了。
    };

    for( const auto& t : texts )
    {
        CAPTURE(t);
        const auto r = parse::parseTime(t, BASE);
        CHECK_FALSE(r.occurred.has_value());
        CHECK(r.remainder == t);
    }
}

// ---------------------------------------------------------------------------
// 形状 4：绝对日期
//
// 没写年份时，和「周三」同一个规矩 —— 取还没到的那个。
// 所以 base 是 2026-10-08 时，「3月5日」是 2027-03-05，不是 2026-03-05。
// ---------------------------------------------------------------------------

TEST_CASE("形状 4：没写年份 → 今年还没到就今年，过了就明年")
{
    CHECK(startedAt("10月20日") == date(2026, 10, 20));   // 还没到
    CHECK(startedAt("10月8日") == date(2026, 10, 8));     // 就是今天
    CHECK(startedAt("10月1日") == date(2027, 10, 1));     // 已经过了 → 明年
    CHECK(startedAt("3月5日")  == date(2027, 3, 5));
}

TEST_CASE("形状 4：日 / 号 都认")
{
    CHECK(startedAt("3月5号") == startedAt("3月5日"));
    CHECK(startedAt("12月31号") == date(2026, 12, 31));
}

TEST_CASE("形状 4：写了年份就照写的算，哪怕已经过去")
{
    CHECK(startedAt("2026年2月14日") == date(2026, 2, 14));
    CHECK(startedAt("2026年10月1日") == date(2026, 10, 1));
    CHECK(startedAt("2027年1月1日")  == date(2027, 1, 1));
}

TEST_CASE("形状 4：2026-10-01 这种写法")
{
    CHECK(startedAt("2026-10-01") == date(2026, 10, 1));
    CHECK(startedAt("2026-10-01的会议") == date(2026, 10, 1));
    CHECK(parse::parseTime("2026-10-01的会议", BASE).remainder == "的会议");
}

TEST_CASE("形状 4：2月29日 要跳到下一个闰年")
{
    CHECK(startedAt("2月29日") == date(2028, 2, 29));   // base 是 2026，2028 才是闰年
}

// ---------------------------------------------------------------------------
// 形状 5：时段词
//
// 光写「下午」不带钟点时，取一个代表时刻（下午 = 14:00）。
// 没写日期部分，就落在基准那天。
// ---------------------------------------------------------------------------

TEST_CASE("形状 5：六个时段词各自的代表时刻")
{
    CHECK(startedAt("早上跑步") == date(2026, 10, 8) + 8h);
    CHECK(startedAt("上午开会") == date(2026, 10, 8) + 9h);
    CHECK(startedAt("中午吃饭") == date(2026, 10, 8) + 12h);
    CHECK(startedAt("下午和小李开会") == date(2026, 10, 8) + 14h);
    CHECK(startedAt("晚上加班") == date(2026, 10, 8) + 20h);
    CHECK(startedAt("凌晨写代码") == date(2026, 10, 8) + 5h);
}

TEST_CASE("形状 5：remainder 抠掉时段词")
{
    CHECK(parse::parseTime("下午和小李开会", BASE).remainder == "和小李开会");
    CHECK(parse::parseTime("早上", BASE).remainder.empty());
}

// ---------------------------------------------------------------------------
// 形状 6：钟点
// ---------------------------------------------------------------------------

TEST_CASE("形状 6：钟点三种写法")
{
    CHECK(startedAt("15点开会") == date(2026, 10, 8) + 15h);
    CHECK(startedAt("9:30 开会") == date(2026, 10, 8) + 9h + 30min);
    CHECK(startedAt("9点半") == date(2026, 10, 8) + 9h + 30min);
    CHECK(startedAt("15点30分") == date(2026, 10, 8) + 15h + 30min);
}

TEST_CASE("形状 6：时段词 + 钟点，时段词负责修正钟点")
{
    CHECK(startedAt("下午3点") == date(2026, 10, 8) + 15h);   // 不是 3:00
    CHECK(startedAt("上午9点") == date(2026, 10, 8) + 9h);    // 不加 12
    CHECK(startedAt("晚上8点") == date(2026, 10, 8) + 20h);
    CHECK(startedAt("中午12点") == date(2026, 10, 8) + 12h);
    CHECK(startedAt("凌晨3点") == date(2026, 10, 8) + 3h);

    // 下午1点 和 下午13点 应该落到同一个时刻
    CHECK(startedAt("下午1点") == startedAt("下午13点"));
}

// ---------------------------------------------------------------------------
// 组合：主体 + 后缀
//
// 这是整套设计的目的 —— 日期那一段和时刻那一段各算各的，最后相加。
// ---------------------------------------------------------------------------

TEST_CASE("组合：日期 + 时段词")
{
    CHECK(startedAt("上周三下午") == date(2026, 9, 30) + 14h);
    CHECK(startedAt("明天早上") == date(2026, 10, 9) + 8h);
    CHECK(startedAt("周三晚上") == date(2026, 10, 14) + 20h);
}

TEST_CASE("组合：日期 + 钟点")
{
    CHECK(startedAt("昨天下午3点") == date(2026, 10, 7) + 15h);
    CHECK(startedAt("明天早上8点") == date(2026, 10, 9) + 8h);
    CHECK(startedAt("3月5日15:30") == date(2027, 3, 5) + 15h + 30min);
}

TEST_CASE("组合：整句的 remainder 把日期和时刻都抠干净")
{
    CHECK(parse::parseTime("上周三下午和小李讨论了方案", BASE).remainder == "和小李讨论了方案");
    CHECK(parse::parseTime("明天早上8点开会", BASE).remainder == "开会");
    CHECK(parse::parseTime("下午3点", BASE).remainder.empty());
}

// ---------------------------------------------------------------------------
// 时段：X点到Y点 —— 这条会填 end
// ---------------------------------------------------------------------------

TEST_CASE("时段：「3点到5点」的 end 有值")
{
    const auto r = parse::parseTime("下午3点到5点", BASE);
    REQUIRE(r.occurred.has_value());
    REQUIRE(r.occurred->end.has_value());
    CHECK(r.occurred->start == date(2026, 10, 8) + 15h);
    CHECK(*r.occurred->end   == date(2026, 10, 8) + 17h);   // 沿用「下午」，所以是 17:00 不是 5:00
    CHECK(r.remainder.empty());
}

TEST_CASE("时段：不带时段词时就是字面时刻")
{
    const auto r = parse::parseTime("9点到11点", BASE);
    REQUIRE(r.occurred.has_value());
    REQUIRE(r.occurred->end.has_value());
    CHECK(r.occurred->start == date(2026, 10, 8) + 9h);
    CHECK(*r.occurred->end   == date(2026, 10, 8) + 11h);
}

TEST_CASE("时段：可以和日期组合")
{
    const auto r = parse::parseTime("3月5日下午3点到5点", BASE);
    REQUIRE(r.occurred.has_value());
    REQUIRE(r.occurred->end.has_value());
    CHECK(r.occurred->start == date(2027, 3, 5) + 15h);
    CHECK(*r.occurred->end   == date(2027, 3, 5) + 17h);
}

// ---------------------------------------------------------------------------
// 收尾：昨晚 / 周末 / 月初
// ---------------------------------------------------------------------------

TEST_CASE("昨晚：整词，自带「晚上」")
{
    CHECK(startedAt("昨晚") == date(2026, 10, 7) + 20h);
    CHECK(parse::parseTime("昨晚没睡好", BASE).remainder == "没睡好");
}

TEST_CASE("昨晚：后面的钟点要按「晚上」修正，不能当早上 8 点")
{
    // 「昨晚8点」是昨天 20:00。如果「昨晚」只是给个日期、丢掉隐含的时段，
    // 那个 8 会被当成 8:00 —— 差 12 小时，而且不报错。
    CHECK(startedAt("昨晚8点") == date(2026, 10, 7) + 20h);
    CHECK(startedAt("昨晚11点") == date(2026, 10, 7) + 23h);
}

TEST_CASE("昨天晚上 == 昨晚，两条路都通")
{
    // 「昨天」+「晚上」是走组合；「昨晚」是走整词。结果必须一样。
    CHECK(startedAt("昨天晚上") == startedAt("昨晚"));
    CHECK(startedAt("昨天晚上8点") == startedAt("昨晚8点"));
}

TEST_CASE("周末：落在那一周的周六")
{
    CHECK(startedAt("上周末") == date(2026, 10, 3));    // 10-10 是周六，减一周
    CHECK(startedAt("这周末") == date(2026, 10, 10));
    CHECK(startedAt("下周末") == date(2026, 10, 17));

    // 裸写「周末」和「周三」同一个规矩：取还没到的那个
    CHECK(startedAt("周末去爬山") == date(2026, 10, 10));
    CHECK(parse::parseTime("周末去爬山", BASE).remainder == "去爬山");
}

TEST_CASE("月初：那个月的 1 号")
{
    CHECK(startedAt("这个月初") == date(2026, 10, 1));
    CHECK(startedAt("上个月初") == date(2026, 9, 1));
    CHECK(startedAt("下个月初") == date(2026, 11, 1));
    CHECK(startedAt("本月初") == startedAt("这个月初"));

    // 「下月初」不带「个」也认
    CHECK(startedAt("下月初") == date(2026, 11, 1));
}

TEST_CASE("月初 / 周末 跨年")
{
    CHECK(startedAt("下个月初", date(2026, 12, 15)) == date(2027, 1, 1));
    CHECK(startedAt("上个月初", date(2027, 1, 15)) == date(2026, 12, 1));
}

// ---------------------------------------------------------------------------
// 形状 4/5/6 该解析失败的样子
// ---------------------------------------------------------------------------

TEST_CASE("形状 4/5/6：写出来了但不合法的，一律不猜")
{
    const std::string texts[] =
    {
        "2026-02-30",     // 没有 2 月 30 日
        "13月1日",        // 没有 13 月
        "3月5",           // 光有「3月5」，没有 日/号
        "25点开会",        // 没有 25 点
        "12:60",          // 没有 12:60
        "9:75",           // 分超了
    };

    for( const auto& t : texts )
    {
        CAPTURE(t);
        const auto r = parse::parseTime(t, BASE);
        CHECK_FALSE(r.occurred.has_value());
        CHECK(r.remainder == t);
    }
}

TEST_CASE("形状 4/5/6：这些看着像但其实不是")
{
    const std::string texts[] =
    {
        "3个苹果",         // 「3」后面不是 月/日/点/:
        "今天买了3个苹果",  // 日期能认，但后面那个 3 不是时间
        "第15名",          // 不是钟点
        "下午茶时间",       // 「下午」后面跟「茶」，时段词后面不该硬吃
    };

    for( const auto& t : texts )
    {
        CAPTURE(t);
        const auto r = parse::parseTime(t, BASE);
        if( t.starts_with("今天") )
        {
            // 「今天」认出来了，但后面的「3个苹果」原样留着
            REQUIRE(r.occurred.has_value());
            CHECK(r.remainder == "买了3个苹果");
        }
        else if( t.starts_with("下午") )
        {
            // 「下午」认出来了（形状 5 本来就是"光写时段词也算"）
            REQUIRE(r.occurred.has_value());
            CHECK(r.occurred->start == date(2026, 10, 8) + 14h);
        }
        else
        {
            CHECK_FALSE(r.occurred.has_value());
            CHECK(r.remainder == t);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 补充用例（黑盒探测打出来的缺口）
//
// 前面那批用例的共同盲区：时段词修正只测了「X点」，没测「X:XX」；
// 12 点 / 0 点没测；区间没测跨零点，也没测第二个钟点自带时段词。
// ═══════════════════════════════════════════════════════════════════════════

// ---------------------------------------------------------------------------
// 冒号形式 + 时段词
//
// 「下午3点」和「下午3:30」是同一个意思的两种写法，修正规则必须一致。
// 之前只有「X点」那条路走了 applyPeriod，「X:XX」直接返回 —— 静默差 12 小时。
// ---------------------------------------------------------------------------

TEST_CASE("形状 6：冒号形式 + 时段词，一样要修正 12 小时")
{
    CHECK(startedAt("下午3:30")  == date(2026, 10, 8) + 15h + 30min);
    CHECK(startedAt("晚上8:30")  == date(2026, 10, 8) + 20h + 30min);
    CHECK(startedAt("晚上11:30") == date(2026, 10, 8) + 23h + 30min);
    CHECK(startedAt("上午9:00")  == date(2026, 10, 8) + 9h);

    // 已经写成 24 小时制的不许再加一遍
    CHECK(startedAt("下午15:30") == date(2026, 10, 8) + 15h + 30min);
    CHECK(startedAt("中午12:30") == date(2026, 10, 8) + 12h + 30min);

    // 这条才是重点：两种写法必须落到同一个时刻
    CHECK(startedAt("下午3:30") == startedAt("下午3点半"));
    CHECK(startedAt("晚上8:30") == startedAt("晚上8点半"));
}

TEST_CASE("时段：冒号形式的区间也吃时段词")
{
    const auto r = parse::parseTime("下午1:30到3:30", BASE);
    REQUIRE(r.occurred.has_value());
    REQUIRE(r.occurred->end.has_value());
    CHECK(r.occurred->start == date(2026, 10, 8) + 13h + 30min);
    CHECK(*r.occurred->end   == date(2026, 10, 8) + 15h + 30min);
}

// ---------------------------------------------------------------------------
// 12 点 / 0 点：边界值不能按"小于 12 就 +12"处理
// ---------------------------------------------------------------------------

TEST_CASE("形状 5/6：12 点和 0 点的边界")
{
    // 上午 / 中午 / 下午 的 12 点都是中午
    CHECK(startedAt("上午12点") == date(2026, 10, 8) + 12h);
    CHECK(startedAt("中午12点") == date(2026, 10, 8) + 12h);
    CHECK(startedAt("下午12点") == date(2026, 10, 8) + 12h);

    // 凌晨说的是后半夜，12 点就是当天零点
    CHECK(startedAt("凌晨12点") == date(2026, 10, 8));
    CHECK(startedAt("凌晨0点")  == date(2026, 10, 8));

    // 晚上 12 点是这一天的 24 点 —— 落到次日零点，不是中午
    CHECK(startedAt("晚上12点")   == date(2026, 10, 9));
    CHECK(startedAt("晚上12点半") == date(2026, 10, 9) + 30min);
    CHECK(startedAt("晚上0点")    == date(2026, 10, 8));

    // 「昨晚」自带「晚上」，所以 12 点同样顺延：10-07 的 24:00 = 10-08 00:00
    CHECK(startedAt("昨晚12点") == date(2026, 10, 8));

    // 写了日期时，顺延是相对那个日期，不是相对基准日
    CHECK(startedAt("明天晚上12点") == date(2026, 10, 10));
}

// ---------------------------------------------------------------------------
// 时段词的有效钟点：不能对所有时段词都用"小于 12 就 +12"
// ---------------------------------------------------------------------------

TEST_CASE("形状 5/6：中午和晚上的有效钟点")
{
    // 中午：11 点、12 点照字面；1 点、2 点是下午
    CHECK(startedAt("中午11点")   == date(2026, 10, 8) + 11h);
    CHECK(startedAt("中午11点半") == date(2026, 10, 8) + 11h + 30min);
    CHECK(startedAt("中午12点")   == date(2026, 10, 8) + 12h);
    CHECK(startedAt("中午1点")    == date(2026, 10, 8) + 13h);
    CHECK(startedAt("中午2点")    == date(2026, 10, 8) + 14h);

    // 中午9点 / 中午10点不是"中午"的说法 —— 不许猜成晚上 9 点 / 10 点
    CHECK_FALSE(parse::parseTime("中午9点", BASE).occurred.has_value());
    CHECK_FALSE(parse::parseTime("中午10点半", BASE).occurred.has_value());

    // 晚上 6..11 点是当天 18..23；1..5 点是次日凌晨
    CHECK(startedAt("晚上6点")   == date(2026, 10, 8) + 18h);
    CHECK(startedAt("晚上11点")  == date(2026, 10, 8) + 23h);
    CHECK(startedAt("晚上1点")   == date(2026, 10, 9) + 1h);
    CHECK(startedAt("晚上2点半") == date(2026, 10, 9) + 2h + 30min);
    CHECK(startedAt("晚上5点")   == date(2026, 10, 9) + 5h);

    // 「昨晚」自带「晚上」：昨晚1点 = 昨天（10-07）的次日凌晨 01:00
    CHECK(startedAt("昨晚1点") == date(2026, 10, 8) + 1h);
}

// ---------------------------------------------------------------------------
// 区间：第二个钟点自带时段词 / 跨零点
// ---------------------------------------------------------------------------

TEST_CASE("时段：第二个钟点可以带自己的时段词")
{
    const auto r = parse::parseTime("晚上11点到凌晨1点", BASE);
    REQUIRE(r.occurred.has_value());
    REQUIRE(r.occurred->end.has_value());
    CHECK(r.occurred->start == date(2026, 10, 8) + 23h);
    CHECK(*r.occurred->end   == date(2026, 10, 9) + 1h);    // 凌晨1点 + 跨零点
    CHECK(r.remainder.empty());

    // 反过来也行：前面不带、后面带
    const auto r2 = parse::parseTime("11点到下午3点", BASE);
    REQUIRE(r2.occurred.has_value());
    REQUIRE(r2.occurred->end.has_value());
    CHECK(r2.occurred->start == date(2026, 10, 8) + 11h);
    CHECK(*r2.occurred->end   == date(2026, 10, 8) + 15h);

    // 两边各带各的
    const auto r3 = parse::parseTime("上午11点半到下午1点", BASE);
    REQUIRE(r3.occurred.has_value());
    REQUIRE(r3.occurred->end.has_value());
    CHECK(r3.occurred->start == date(2026, 10, 8) + 11h + 30min);
    CHECK(*r3.occurred->end   == date(2026, 10, 8) + 13h);
}

TEST_CASE("时段：第二个钟点沿用时段词，但不能把跨零点那句读歪")
{
    // 「下午3点到5点」沿用「下午」= 17:00，比起点晚，不需要跨天 —— 沿用是对的
    const auto same = parse::parseTime("下午3点到5点", BASE);
    REQUIRE(same.occurred.has_value());
    REQUIRE(same.occurred->end.has_value());
    CHECK(*same.occurred->end == date(2026, 10, 8) + 17h);

    // 「晚上11点到1点」如果也沿用「晚上」，1点 会变成 13:00（次日），跨度 14 小时。
    // 这话的意思是凌晨 1 点：跨度 2 小时。
    const auto cross = parse::parseTime("晚上11点到1点", BASE);
    REQUIRE(cross.occurred.has_value());
    REQUIRE(cross.occurred->end.has_value());
    CHECK(cross.occurred->start == date(2026, 10, 8) + 23h);
    CHECK(*cross.occurred->end   == date(2026, 10, 9) + 1h);

    // 明写了时段词就听明的，不回落
    const auto explicitOne = parse::parseTime("晚上11点到凌晨1点", BASE);
    REQUIRE(explicitOne.occurred.has_value());
    CHECK(*explicitOne.occurred->end == date(2026, 10, 9) + 1h);

    // 「12点」自带次日偏移，不能被回落规则当成"比起点早的那半边"
    const auto midnight = parse::parseTime("晚上11点半到12点半", BASE);
    REQUIRE(midnight.occurred.has_value());
    REQUIRE(midnight.occurred->end.has_value());
    CHECK(midnight.occurred->start == date(2026, 10, 8) + 23h + 30min);
    CHECK(*midnight.occurred->end   == date(2026, 10, 9) + 30min);
}

TEST_CASE("时段：终点早于起点就算跨了零点")
{
    const auto r = parse::parseTime("23点到1点", BASE);
    REQUIRE(r.occurred.has_value());
    REQUIRE(r.occurred->end.has_value());
    CHECK(r.occurred->start == date(2026, 10, 8) + 23h);
    CHECK(*r.occurred->end   == date(2026, 10, 9) + 1h);

    // 两边都没写时段词时，先在当天补 12 小时：「9点到8点」是 9:00–20:00，
    // 不是次日 08:00（23 小时）。补完还早于起点（「23点到1点」）才跨零点。
    const auto r2 = parse::parseTime("9点到8点", BASE);
    REQUIRE(r2.occurred.has_value());
    REQUIRE(r2.occurred->end.has_value());
    CHECK(r2.occurred->start == date(2026, 10, 8) + 9h);
    CHECK(*r2.occurred->end   == date(2026, 10, 8) + 20h);

    // 「11点到1点」同理：13:00 结束，跨度 2 小时，不是次日 01:00
    const auto r3 = parse::parseTime("11点到1点", BASE);
    REQUIRE(r3.occurred.has_value());
    REQUIRE(r3.occurred->end.has_value());
    CHECK(r3.occurred->start == date(2026, 10, 8) + 11h);
    CHECK(*r3.occurred->end   == date(2026, 10, 8) + 13h);
}

// ---------------------------------------------------------------------------
// 星期天 / 周天 / 礼拜天
// ---------------------------------------------------------------------------

TEST_CASE("形状 3：星期天 / 周天 / 礼拜天 都算周日")
{
    CHECK(startedAt("星期天") == date(2026, 10, 11));
    CHECK(startedAt("周天")   == date(2026, 10, 11));
    CHECK(startedAt("礼拜天") == date(2026, 10, 11));
    CHECK(startedAt("星期天") == startedAt("周日"));

    CHECK(parse::parseTime("星期天去爬山", BASE).remainder == "去爬山");
    CHECK(startedAt("下周天") == date(2026, 10, 18));
}

TEST_CASE("形状 3：「天气」不是星期词")
{
    // 「天」＝周日会让「下周天气不错」看起来像「下周日」+「气不错」。
    // 那种句子里的「天」根本不是星期，必须整句不认。
    const auto r = parse::parseTime("下周天气不错", BASE);
    CHECK_FALSE(r.occurred.has_value());
    CHECK(r.remainder == "下周天气不错");
}

// ---------------------------------------------------------------------------
// 大前天 / 大后天
// ---------------------------------------------------------------------------

TEST_CASE("形状 1：大前天 / 大后天")
{
    CHECK(startedAt("大前天") == date(2026, 10, 5));
    CHECK(startedAt("大后天") == date(2026, 10, 11));

    CHECK(parse::parseTime("大前天没睡好", BASE).remainder == "没睡好");
    CHECK(parse::parseTime("大后天交作业", BASE).remainder == "交作业");
}

// ---------------------------------------------------------------------------
// 前缀叠两次
// ---------------------------------------------------------------------------

TEST_CASE("形状 2：前缀叠两次 = 再推一周（下下周三 / 上上周三）")
{
    CHECK(startedAt("下下周三") == date(2026, 10, 21));   // 本周三 10-07 + 14
    CHECK(startedAt("下下周一") == date(2026, 10, 19));
    CHECK(startedAt("上上周三") == date(2026, 9, 23));    // 10-07 - 14

    // 「这这周三」不成话：只吃一个「这」，剩下那截不是星期词，整句不认
    const auto weird = parse::parseTime("这这周三", BASE);
    CHECK_FALSE(weird.occurred.has_value());
    CHECK(weird.remainder == "这这周三");
}

// ---------------------------------------------------------------------------
// 今年 / 明年 / 去年
// ---------------------------------------------------------------------------

TEST_CASE("形状 6：今年 / 明年 / 去年 = 那一年的 1 月 1 日")
{
    CHECK(startedAt("今年") == date(2026, 1, 1));
    CHECK(startedAt("明年") == date(2027, 1, 1));
    CHECK(startedAt("去年") == date(2025, 1, 1));

    CHECK(parse::parseTime("今年要努力", BASE).remainder == "要努力");
    CHECK(parse::parseTime("明年的计划", BASE).remainder == "的计划");
}

TEST_CASE("形状 6：[今年|明年|去年] + 月日 = 明确年份，不再按「还没到的那个」猜")
{
    // 光写「3月5日」在 base=2026-10-08 时是 2027（那天今年已经过了）；
    // 写了「今年」就照今年算 —— 即使 2026-03-05 已经过去。
    CHECK(startedAt("今年3月5日")   == date(2026, 3, 5));
    CHECK(startedAt("明年3月5日")   == date(2027, 3, 5));
    CHECK(startedAt("去年12月31日") == date(2025, 12, 31));

    CHECK(parse::parseTime("今年3月5日交作业", BASE).remainder == "交作业");
}

TEST_CASE("形状 6：「今年10月」缺了日 / 号，整段不认")
{
    // 跟「2026年10月」一致：宁可一个字都不解析，
    // 也不许只吃掉「今年」然后留下「10月」这种半截结果。
    const std::string texts[] = { "今年10月", "明年3月" };

    for (const auto& t : texts)
    {
        CAPTURE(t);
        const auto r = parse::parseTime(t, BASE);
        CHECK_FALSE(r.occurred.has_value());
        CHECK(r.remainder == t);
    }
}

// ---------------------------------------------------------------------------
// 日期区间：日期 到 日期
//
// 约定：一条连续跨度，终点含第二个日期整天 ——
// 「3月5日到3月7日」= 3/5 00:00 到 3/8 00:00，不是到 3/7 00:00。
// ---------------------------------------------------------------------------

TEST_CASE("日期区间：日期 到 日期")
{
    const auto r = parse::parseTime("3月5日到3月7日", BASE);
    REQUIRE(r.occurred.has_value());
    REQUIRE(r.occurred->end.has_value());
    CHECK(r.occurred->start == date(2027, 3, 5));
    CHECK(*r.occurred->end   == date(2027, 3, 8));   // 含 3/7 整天
    CHECK(r.remainder.empty());

    CHECK(parse::parseTime("3月5日到3月7日交报告", BASE).remainder == "交报告");
    CHECK(parse::parseTime("3月5日至3月7日", BASE).occurred.has_value());   // 「至」也认

    // 跨年：「12月31日」是今年，「1月2日」已经过了 → 明年
    const auto y = parse::parseTime("12月31日到1月2日", BASE);
    REQUIRE(y.occurred.has_value());
    REQUIRE(y.occurred->end.has_value());
    CHECK(y.occurred->start == date(2026, 12, 31));
    CHECK(*y.occurred->end   == date(2027, 1, 3));   // 含 1/2 整天
}

TEST_CASE("日期区间：第二个日期不许落在第一个之前")
{
    // 不拿第一个日期当上界的话：「周一」会跳到下周（10-12），
    // 「周五」却留在本周（10-09），区间就倒过来了。
    const auto r = parse::parseTime("周一到周五", BASE);
    REQUIRE(r.occurred.has_value());
    REQUIRE(r.occurred->end.has_value());
    CHECK(r.occurred->start == date(2026, 10, 12));
    CHECK(*r.occurred->end   == date(2026, 10, 17));   // 周六 0:00 = 含周五整天

    const auto s = parse::parseTime("这周六到周日", BASE);
    REQUIRE(s.occurred.has_value());
    REQUIRE(s.occurred->end.has_value());
    CHECK(s.occurred->start == date(2026, 10, 10));
    CHECK(*s.occurred->end   == date(2026, 10, 12));   // 周一 0:00 = 含周日整天
}

// ---------------------------------------------------------------------------
// 日期区间 + 钟点：统一成一条跨度
//
// 语义二重性（一条跨度 vs 每天重复）无法在 TimeRange 里表达，
// 这里定死为一条跨度：
//     起点 = 首日 00:00（钟点起点不参与）
//     终点 = 末日 + 钟点终点（没写钟点终点就用钟点起点）
// ---------------------------------------------------------------------------

TEST_CASE("日期区间 + 钟点：统一成一条跨度")
{
    // 「周一到周五下午3点」→ 周一 0:00 到周五 15:00
    const auto point = parse::parseTime("周一到周五下午3点", BASE);
    REQUIRE(point.occurred.has_value());
    REQUIRE(point.occurred->end.has_value());
    CHECK(point.occurred->start == date(2026, 10, 12));
    CHECK(*point.occurred->end   == date(2026, 10, 16) + 15h);

    // 「周一到周五下午3点到5点」→ 周一 0:00 到周五 17:00
    const auto span = parse::parseTime("周一到周五下午3点到5点", BASE);
    REQUIRE(span.occurred.has_value());
    REQUIRE(span.occurred->end.has_value());
    CHECK(span.occurred->start == date(2026, 10, 12));
    CHECK(*span.occurred->end   == date(2026, 10, 16) + 17h);

    // 「3月5日到3月7日下午3点」→ 3/5 0:00 到 3/7 15:00
    const auto abs = parse::parseTime("3月5日到3月7日下午3点", BASE);
    REQUIRE(abs.occurred.has_value());
    REQUIRE(abs.occurred->end.has_value());
    CHECK(abs.occurred->start == date(2027, 3, 5));
    CHECK(*abs.occurred->end   == date(2027, 3, 7) + 15h);

    // 钟点区间跨零点时，终点从**末日**往后推，不是从首日
    // 「周一到周五晚上11点到凌晨1点」→ 周一 0:00 到周六 1:00
    const auto cross = parse::parseTime("周一到周五晚上11点到凌晨1点", BASE);
    REQUIRE(cross.occurred.has_value());
    REQUIRE(cross.occurred->end.has_value());
    CHECK(cross.occurred->start == date(2026, 10, 12));
    CHECK(*cross.occurred->end   == date(2026, 10, 17) + 1h);
}

// ---------------------------------------------------------------------------
// 空格：CLI 把 argv 用空格拼成一条文本，主体和后缀之间可能隔着空格
// ---------------------------------------------------------------------------

TEST_CASE("空格：日期和时刻之间允许有空格")
{
    const auto r = parse::parseTime("明天 下午3点", BASE);
    REQUIRE(r.occurred.has_value());
    CHECK(r.occurred->start == date(2026, 10, 9) + 15h);
    CHECK(r.remainder.empty());

    // 空格算进被吃掉的区间，正文照旧
    const auto tail = parse::parseTime("明天 下午3点 开会", BASE);
    REQUIRE(tail.occurred.has_value());
    CHECK(tail.remainder == " 开会");

    // 「到」两边有空格也认
    const auto range = parse::parseTime("周一 到 周五", BASE);
    REQUIRE(range.occurred.has_value());
    REQUIRE(range.occurred->end.has_value());
    CHECK(range.occurred->start == date(2026, 10, 12));
    CHECK(*range.occurred->end   == date(2026, 10, 17));

    // 只有"日期 + 空格 + 正文"时，正文不能被当成时刻吃掉
    const auto note = parse::parseTime("今天 天气 不错", BASE);
    REQUIRE(note.occurred.has_value());
    CHECK(note.remainder == " 天气 不错");
}

TEST_CASE("日期区间：第二个日期早于第一个时，整段区间不吃")
{
    // 「明天到昨天」没意义。规则：退回单个日期，剩下的原文留给 remainder，
    // 绝不产出 end < start 的区间（那会让下游算出负数时长）。
    const auto r = parse::parseTime("明天到昨天", BASE);
    REQUIRE(r.occurred.has_value());
    CHECK_FALSE(r.occurred->end.has_value());
    CHECK(r.occurred->start == date(2026, 10, 9));
    CHECK(r.remainder == "到昨天");
}

TEST_CASE("日期区间：猜出来的年份被推到下一年、跨度又过大时，整段不吃")
{
    // 「3月5日到3月1日」的第二个日期被 pickYear 推到 2028-03-01（跨了约一年），
    // 那是口误，不是跨年区间。宁可退回单个日期。
    const auto r = parse::parseTime("3月5日到3月1日", BASE);
    REQUIRE(r.occurred.has_value());
    CHECK_FALSE(r.occurred->end.has_value());
    CHECK(r.occurred->start == date(2027, 3, 5));
    CHECK(r.remainder == "到3月1日");

    const auto r2 = parse::parseTime("10月5日到9月1日", BASE);
    REQUIRE(r2.occurred.has_value());
    CHECK_FALSE(r2.occurred->end.has_value());
    CHECK(r2.remainder == "到9月1日");

    // 真正的跨年（12月31日到1月1日）照常放行
    const auto ok = parse::parseTime("12月31日到1月1日", BASE);
    REQUIRE(ok.occurred.has_value());
    REQUIRE(ok.occurred->end.has_value());
    CHECK(ok.occurred->start == date(2026, 12, 31));
    CHECK(*ok.occurred->end   == date(2027, 1, 2));   // 含 1/1 整天

    // 同年内的大跨度也放行：1月1日到12月31日
    const auto year = parse::parseTime("1月1日到12月31日", BASE);
    REQUIRE(year.occurred.has_value());
    REQUIRE(year.occurred->end.has_value());
    CHECK(year.occurred->start == date(2027, 1, 1));
    CHECK(*year.occurred->end   == date(2028, 1, 1));   // 含 12/31 整天

    // 写了年份的长区间不受影响
    const auto explicitYear = parse::parseTime("2026年3月1日到2027年2月1日", BASE);
    REQUIRE(explicitYear.occurred.has_value());
    REQUIRE(explicitYear.occurred->end.has_value());
    CHECK(explicitYear.occurred->start == date(2026, 3, 1));
    CHECK(*explicitYear.occurred->end   == date(2027, 2, 2));   // 含 2/1 整天
}
