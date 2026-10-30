#include <doctest/doctest.h>

#include <chrono>

#include "app/time_input.hpp"

// 手动改时间的文本解析。时区固定 Asia/Shanghai，断言才是确定的。

using namespace std::chrono;

namespace
{

const time_zone& shanghai()
{
    static const time_zone* tz = locate_zone("Asia/Shanghai");
    return *tz;
}

// 上海 2026-10-28 14:00 作为 fallback（"只写时刻"时的日期来源）
constexpr sys_seconds FALLBACK = sys_days{ year{ 2026 } / 10 / 28 } + hours{ 14 };

}  // namespace

TEST_CASE("parseLocalDateTime：完整日期+时刻（全角/半角冒号都认）")
{
    const auto utc = app::parseLocalDateTime("2026-10-30 08:30", FALLBACK, shanghai());
    REQUIRE(utc.has_value());
    // 上海 08:30 = UTC 00:30
    CHECK(*utc == sys_days{ year{ 2026 } / 10 / 30 } + hours{ 0 } + minutes{ 30 });
}

TEST_CASE("parseLocalDateTime：只有日期 → 当天 00:00（本地）")
{
    const auto utc = app::parseLocalDateTime("2026-10-30", FALLBACK, shanghai());
    REQUIRE(utc.has_value());
    // 上海 10-30 00:00 = UTC 10-29 16:00
    CHECK(*utc == sys_days{ year{ 2026 } / 10 / 29 } + hours{ 16 });
}

TEST_CASE("parseLocalDateTime：只有时刻 → 用 fallback 的日期")
{
    const auto utc = app::parseLocalDateTime("08:30", FALLBACK, shanghai());
    REQUIRE(utc.has_value());
    CHECK(*utc == sys_days{ year{ 2026 } / 10 / 28 } + hours{ 0 } + minutes{ 30 });

    // 8:30（一位数小时）也认
    const auto shortHour = app::parseLocalDateTime("8:30", FALLBACK, shanghai());
    REQUIRE(shortHour.has_value());
    CHECK(*shortHour == *utc);
}

TEST_CASE("parseLocalDateTime：缺年份 → 用 fallback 的年份")
{
    const auto utc = app::parseLocalDateTime("11-02 09:00", FALLBACK, shanghai());
    REQUIRE(utc.has_value());
    CHECK(*utc == sys_days{ year{ 2026 } / 11 / 2 } + hours{ 1 });   // 上海 11-02 09:00 = UTC 01:00
}

TEST_CASE("parseLocalDateTime：非法输入一律返回空，不猜")
{
    CHECK_FALSE(app::parseLocalDateTime("", FALLBACK, shanghai()).has_value());
    CHECK_FALSE(app::parseLocalDateTime("明天下午三点", FALLBACK, shanghai()).has_value());
    CHECK_FALSE(app::parseLocalDateTime("2026-02-30 08:30", FALLBACK, shanghai()).has_value());  // 没这天
    CHECK_FALSE(app::parseLocalDateTime("2026-10-30 25:00", FALLBACK, shanghai()).has_value());  // 没这钟点
    CHECK_FALSE(app::parseLocalDateTime("2026-13-01", FALLBACK, shanghai()).has_value());        // 没这月
}

TEST_CASE("formatLocalDateTime：UTC → 本地可读文本，能原样解析回去")
{
    // 上海 2026-10-30 08:30 = UTC 00:30
    const sys_seconds utc = sys_days{ year{ 2026 } / 10 / 30 } + hours{ 0 } + minutes{ 30 };

    CHECK(app::formatLocalDateTime(utc, shanghai()) == "2026-10-30 08:30");

    // 往返：格式化 → 解析 → 同一个 UTC
    const auto back = app::parseLocalDateTime(app::formatLocalDateTime(utc, shanghai()), FALLBACK,
                                              shanghai());
    REQUIRE(back.has_value());
    CHECK(*back == utc);
}
