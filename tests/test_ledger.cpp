#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <string>

#include "ledger/book.hpp"
#include "ledger/ledger.hpp"
#include "ledger/money.hpp"

// 记账的底层：金额解析、方向/分类猜测、账本读写与汇总。
// 全部是纯逻辑 + 文件读写，不碰 UI。

using namespace std::chrono;

namespace
{

std::filesystem::path scratchFile(char const* name)
{
    const auto dir = std::filesystem::temp_directory_path() / "noteall-ledger-test";
    std::filesystem::create_directories(dir);
    const auto file = dir / name;
    std::filesystem::remove(file);
    std::filesystem::remove(std::filesystem::path{ file }.concat(".lock"));
    std::filesystem::remove(std::filesystem::path{ file }.concat(".tmp"));
    return file;
}

sys_seconds utcAt(int day, int hour, int minute = 0)
{
    return sys_days{ year{ 2026 } / 10 / day } + hours{ hour } + minutes{ minute };
}

}  // namespace

// ---------------------------------------------------------------------------
// 金额解析
// ---------------------------------------------------------------------------

TEST_CASE("parseCents：各种写法都换算成整数分")
{
    CHECK(*ledger::parseCents("32") == 3200);
    CHECK(*ledger::parseCents("32.5") == 3250);
    CHECK(*ledger::parseCents("32.50") == 3250);
    CHECK(*ledger::parseCents("1,024.00") == 102400);
    CHECK(*ledger::parseCents("¥32") == 3200);
    CHECK(*ledger::parseCents("32元") == 3200);
    CHECK(*ledger::parseCents("32块") == 3200);

    // 0 元没有记账意义；负数交给方向表达
    CHECK_FALSE(ledger::parseCents("0").has_value());
    CHECK_FALSE(ledger::parseCents("-5").has_value());

    // 超过分的精度不猜
    CHECK_FALSE(ledger::parseCents("32.555").has_value());
    CHECK_FALSE(ledger::parseCents("abc").has_value());
    CHECK_FALSE(ledger::parseCents("").has_value());
}

TEST_CASE("formatCents：两位小数，不丢精度")
{
    CHECK(ledger::formatCents(3200) == "32.00");
    CHECK(ledger::formatCents(3250) == "32.50");
    CHECK(ledger::formatCents(5) == "0.05");
    CHECK(ledger::formatCents(102400) == "1024.00");
}

TEST_CASE("parseAmountNote：金额取句末数字，其余是备注")
{
    // 中间没有空格也行
    auto lunch = ledger::parseAmountNote("午饭 32");
    REQUIRE(lunch.has_value());
    CHECK(lunch->amountCents == 3200);
    CHECK(lunch->note == "午饭");

    auto taxi = ledger::parseAmountNote("打车12块");
    REQUIRE(taxi.has_value());
    CHECK(taxi->amountCents == 1200);
    CHECK(taxi->note == "打车");

    auto coffee = ledger::parseAmountNote("早饭 ¥12.5");
    REQUIRE(coffee.has_value());
    CHECK(coffee->amountCents == 1250);
    CHECK(coffee->note == "早饭");

    // 日期里的数字不会被误当成金额（取句末那个）
    auto dated = ledger::parseAmountNote("10月30日 午饭 32");
    REQUIRE(dated.has_value());
    CHECK(dated->amountCents == 3200);
    CHECK(dated->note == "10月30日 午饭");

    CHECK_FALSE(ledger::parseAmountNote("没有金额的一句话").has_value());
    CHECK_FALSE(ledger::parseAmountNote("").has_value());
}

// ---------------------------------------------------------------------------
// 方向与分类猜测
// ---------------------------------------------------------------------------

TEST_CASE("guessDirection / guessCategory：关键词表，确定性")
{
    CHECK(ledger::toString(ledger::guessDirection("午饭")) == "expense");
    CHECK(ledger::toString(ledger::guessDirection("工资到账")) == "income");
    CHECK(ledger::toString(ledger::guessDirection("报销 200")) == "income");

    CHECK(ledger::guessCategory("午饭", ledger::Direction::Expense) == "餐饮");
    CHECK(ledger::guessCategory("打车", ledger::Direction::Expense) == "交通");
    CHECK(ledger::guessCategory("超市", ledger::Direction::Expense) == "购物");
    CHECK(ledger::guessCategory("房租", ledger::Direction::Expense) == "居家");
    CHECK(ledger::guessCategory("电影", ledger::Direction::Expense) == "娱乐");
    CHECK(ledger::guessCategory("随便什么", ledger::Direction::Expense) == "其他");

    CHECK(ledger::guessCategory("工资", ledger::Direction::Income) == "工资");
    CHECK(ledger::guessCategory("退款", ledger::Direction::Income) == "退款");
}

// ---------------------------------------------------------------------------
// 账本
// ---------------------------------------------------------------------------

TEST_CASE("Book：记一笔 → 读回 → 金额和方向都对")
{
    const auto file = scratchFile("basic.jsonl");

    ledger::Book book{ file };
    REQUIRE(book.load().has_value());

    auto added = book.add(3200, ledger::Direction::Expense, utcAt(30, 12), "餐饮", "午饭",
                          utcAt(30, 12));
    REQUIRE(added.has_value());
    CHECK(added->amountCents == 3200);

    // 新实例读盘，确认真的落盘了
    ledger::Book reopened{ file };
    REQUIRE(reopened.load().has_value());
    REQUIRE(reopened.list().size() == 1);
    CHECK(reopened.list()[0].amountCents == 3200);
    CHECK(reopened.list()[0].category == "餐饮");
    CHECK(ledger::toString(reopened.list()[0].direction) == "expense");
}

TEST_CASE("Book：删除是软删除，汇总里不再计入")
{
    const auto file = scratchFile("remove.jsonl");

    ledger::Book book{ file };
    REQUIRE(book.load().has_value());

    auto added = book.add(10000, ledger::Direction::Expense, utcAt(30, 10), "其他", "测试", utcAt(30, 10));
    REQUIRE(added.has_value());
    REQUIRE(book.remove(added->id).has_value());

    CHECK(book.list().empty());             // 可见列表里没有
    CHECK(book.listAll().size() == 1);      // 墓碑还在

    const auto summary = book.summary(year{ 2026 } / 10);
    CHECK(summary.expenseCents == 0);
    CHECK(summary.count == 0);
}

TEST_CASE("Book::summary：按本地月份汇总支出/收入/分类")
{
    const auto file = scratchFile("summary.jsonl");

    ledger::Book book{ file };
    REQUIRE(book.load().has_value());

    // 上海时间 10-30 12:00 = UTC 04:00
    REQUIRE(book.add(3200, ledger::Direction::Expense, utcAt(30, 4), "餐饮", "午饭", utcAt(30, 4)).has_value());
    REQUIRE(book.add(1200, ledger::Direction::Expense, utcAt(30, 5), "交通", "打车", utcAt(30, 5)).has_value());
    REQUIRE(book.add(500000, ledger::Direction::Income, utcAt(30, 6), "工资", "工资到账", utcAt(30, 6)).has_value());

    // 下个月的一笔，不该算进 10 月（10-31 还是 10 月，必须写 11 月）
    const auto november = sys_days{ year{ 2026 } / 11 / 5 } + hours{ 4 };
    REQUIRE(book.add(9900, ledger::Direction::Expense, november, "餐饮", "11月", november).has_value());

    const auto october = book.summary(year{ 2026 } / 10);
    CHECK(october.expenseCents == 4400);    // 32 + 12
    CHECK(october.incomeCents == 500000);
    CHECK(october.count == 3);
    REQUIRE(october.byCategory.size() == 2);
    CHECK(october.byCategory[0].category == "餐饮");   // 金额大的在前
    CHECK(october.byCategory[0].cents == 3200);
    CHECK(october.byCategory[1].category == "交通");
}

TEST_CASE("Book::summary：收入也按分类汇总（图表要用）")
{
    const auto file = scratchFile("income-cats.jsonl");

    ledger::Book book{ file };
    REQUIRE(book.load().has_value());

    REQUIRE(book.add(500000, ledger::Direction::Income, utcAt(30, 2), "工资", "工资", utcAt(30, 2)).has_value());
    REQUIRE(book.add(20000, ledger::Direction::Income, utcAt(30, 3), "报销", "报销", utcAt(30, 3)).has_value());
    REQUIRE(book.add(3200, ledger::Direction::Expense, utcAt(30, 4), "餐饮", "午饭", utcAt(30, 4)).has_value());

    const auto summary = book.summary(year{ 2026 } / 10);

    REQUIRE(summary.incomeByCategory.size() == 2);
    CHECK(summary.incomeByCategory[0].category == "工资");      // 金额大的在前
    CHECK(summary.incomeByCategory[0].cents == 500000);
    CHECK(summary.incomeByCategory[1].category == "报销");

    // 支出分类不受影响
    REQUIRE(summary.byCategory.size() == 1);
    CHECK(summary.byCategory[0].category == "餐饮");
}

TEST_CASE("monthlyTotals：近 6 个月按月分桶，最老在前")
{
    const auto file = scratchFile("monthly.jsonl");

    ledger::Book book{ file };
    REQUIRE(book.load().has_value());

    // 上海 8 月 / 9 月 / 10 月各一笔（UTC 用当天 04:00 = 上海 12:00）
    const auto august    = sys_days{ year{ 2026 } / 8 / 15 } + hours{ 4 };
    const auto september = sys_days{ year{ 2026 } / 9 / 10 } + hours{ 4 };

    REQUIRE(book.add(1000, ledger::Direction::Expense, august,    "餐饮", "8月", august).has_value());
    REQUIRE(book.add(2000, ledger::Direction::Expense, september, "餐饮", "9月", september).has_value());
    REQUIRE(book.add(3000, ledger::Direction::Expense, utcAt(30, 4), "餐饮", "10月", utcAt(30, 4)).has_value());
    REQUIRE(book.add(9000, ledger::Direction::Income,  utcAt(30, 5), "工资", "10月收入", utcAt(30, 5)).has_value());

    const auto months = ledger::monthlyTotals(book.list(), year{ 2026 } / 10, 6);
    REQUIRE(months.size() == 6);

    CHECK(months.front().month == year{ 2026 } / 5);    // 5、6、7、8、9、10
    CHECK(months.back().month == year{ 2026 } / 10);

    CHECK(months[3].expenseCents == 1000);              // 8 月
    CHECK(months[4].expenseCents == 2000);              // 9 月
    CHECK(months[5].expenseCents == 3000);              // 10 月
    CHECK(months[5].incomeCents == 9000);

    // 没记账的月份是 0，不是漏掉
    CHECK(months[0].expenseCents == 0);
}

TEST_CASE("dayTotals：每一天一个桶，没记账的天是 0")
{
    const auto file = scratchFile("daily.jsonl");

    ledger::Book book{ file };
    REQUIRE(book.load().has_value());

    // 上海时间：10-30 两笔支出、10-01 一笔收入、9 月一笔（不该计入 10 月）
    REQUIRE(book.add(3200, ledger::Direction::Expense, utcAt(30, 4), "餐饮", "午饭", utcAt(30, 4)).has_value());
    REQUIRE(book.add(1200, ledger::Direction::Expense, utcAt(30, 5), "交通", "打车", utcAt(30, 5)).has_value());
    REQUIRE(book.add(500000, ledger::Direction::Income,
                     sys_days{ year{ 2026 } / 10 / 1 } + hours{ 4 }, "工资", "工资", utcAt(1, 4)).has_value());
    REQUIRE(book.add(9999, ledger::Direction::Expense,
                     sys_days{ year{ 2026 } / 9 / 30 } + hours{ 4 }, "餐饮", "9月", utcAt(30, 4)).has_value());

    const auto days = ledger::dayTotals(book.list(), year{ 2026 } / 10);
    REQUIRE(days.size() == 31);                 // 10 月有 31 天

    CHECK(days[0].day == 1);
    CHECK(days[0].incomeCents == 500000);
    CHECK(days[0].expenseCents == 0);

    CHECK(days[29].day == 30);
    CHECK(days[29].expenseCents == 4400);       // 32 + 12
    CHECK(days[29].count == 2);

    CHECK(days[15].expenseCents == 0);          // 16 号没记账 → 0，不是漏掉
    CHECK(days[15].count == 0);
}

TEST_CASE("dayTotals：2 月天数按闰年算")
{
    ledger::Book empty{ scratchFile("daily-feb.jsonl") };
    REQUIRE(empty.load().has_value());

    CHECK(ledger::dayTotals(empty.list(), year{ 2026 } / 2).size() == 28);   // 平年
    CHECK(ledger::dayTotals(empty.list(), year{ 2028 } / 2).size() == 29);   // 闰年
}
