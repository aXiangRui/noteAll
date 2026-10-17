#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "core/core.hpp"
#include "test_util.hpp"

// Core —— 门面。重点不是"能不能存"，而是三条不变量：
//   1. raw_text 原样保存，一个字都不改；
//   2. list() 永远 created_at 倒序，同一秒按 id 稳定排序；
//   3. 所有改动都落盘，换一个 Core 实例还读得回来。

using namespace std::chrono;

TEST_CASE("core：add 保存原文、created_at 和解析结果")
{
    testutil::TempFile f;
    core::Core          c{ f.path() };
    REQUIRE(c.load().has_value());

    const sys_seconds created = testutil::at(2026, 10, 18, 9, 30);
    const core::TimeRange occurred{ testutil::at(2026, 10, 19, 15),
                                    testutil::at(2026, 10, 19, 17) };

    // 前后故意留空格：add 不许 trim。
    const auto added = c.add("  明天 下午3点 开会  ", created, occurred);
    REQUIRE(added.has_value());

    CHECK(added->id.size() == 12);
    CHECK(added->raw_text == "  明天 下午3点 开会  ");
    CHECK(added->created_at == created);
    REQUIRE(added->occurred.has_value());
    CHECK(added->occurred->start == occurred.start);
    CHECK(added->occurred->end == occurred.end);

    // 还没做的字段保持默认，不许瞎填。
    CHECK(added->summary.empty());
    CHECK(added->tags.empty());
    CHECK((added->kind == core::Kind::Unknown));
    CHECK_FALSE(added->confirmed);
}

TEST_CASE("core：add 之后换一个实例还能读回来")
{
    testutil::TempFile f;

    const sys_seconds created = testutil::at(2026, 10, 18, 9, 30);
    std::string       id;

    {
        core::Core c{ f.path() };
        REQUIRE(c.load().has_value());

        const auto added = c.add("持久化测试", created);
        REQUIRE(added.has_value());
        id = added->id;
    }

    core::Core reloaded{ f.path() };
    REQUIRE(reloaded.load().has_value());
    REQUIRE(reloaded.list().size() == 1);
    CHECK(reloaded.list()[0].id == id);
    CHECK(reloaded.list()[0].raw_text == "持久化测试");
    CHECK(reloaded.list()[0].created_at == created);
}

TEST_CASE("core：list 按 created_at 倒序")
{
    testutil::TempFile f;
    core::Core          c{ f.path() };
    REQUIRE(c.load().has_value());

    REQUIRE(c.add("早", testutil::at(2026, 10, 18, 9)).has_value());
    REQUIRE(c.add("晚", testutil::at(2026, 10, 18, 10)).has_value());

    REQUIRE(c.list().size() == 2);
    CHECK(c.list()[0].raw_text == "晚");
    CHECK(c.list()[1].raw_text == "早");
}

TEST_CASE("core：同一秒的记录按 id 倒序，顺序稳定")
{
    testutil::TempFile f;
    core::Core          c{ f.path() };
    REQUIRE(c.load().has_value());

    const sys_seconds same = testutil::at(2026, 10, 18, 9, 30);

    const auto a = c.add("A", same);
    const auto b = c.add("B", same);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    REQUIRE(c.list().size() == 2);
    // 同一秒内 std::sort 不稳定，所以 core 用 id 做次级键。
    CHECK(c.list()[0].id > c.list()[1].id);
    CHECK(c.list()[0].id == std::max(a->id, b->id));
}

TEST_CASE("core：find 找得到，找不到返回空")
{
    testutil::TempFile f;
    core::Core          c{ f.path() };
    REQUIRE(c.load().has_value());

    const auto added = c.add("找得到", testutil::at(2026, 10, 18, 9));
    REQUIRE(added.has_value());

    const auto found = c.find(added->id);
    REQUIRE(found.has_value());
    CHECK(found->raw_text == "找得到");

    CHECK_FALSE(c.find("000000000000").has_value());
}

TEST_CASE("core：remove 删掉并落盘，删不存在的 id 报错")
{
    testutil::TempFile f;
    core::Core          c{ f.path() };
    REQUIRE(c.load().has_value());

    const auto a = c.add("留下", testutil::at(2026, 10, 18, 9));
    const auto b = c.add("删掉", testutil::at(2026, 10, 18, 10));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    REQUIRE(c.remove(b->id).has_value());
    REQUIRE(c.list().size() == 1);
    CHECK(c.list()[0].id == a->id);
    CHECK_FALSE(c.find(b->id).has_value());

    // 再删一次：不存在是错误，不是静默成功。
    const auto again = c.remove(b->id);
    CHECK_FALSE(again.has_value());
    CHECK_FALSE(again.error().empty());

    // 换实例验证删除也落盘了。
    core::Core reloaded{ f.path() };
    REQUIRE(reloaded.load().has_value());
    REQUIRE(reloaded.list().size() == 1);
    CHECK(reloaded.list()[0].id == a->id);
}

TEST_CASE("core：makeId 在同一批记录里不重复")
{
    testutil::TempFile f;
    core::Core          c{ f.path() };
    REQUIRE(c.load().has_value());

    const sys_seconds same = testutil::at(2026, 10, 18, 9, 30);

    std::vector<std::string> ids;
    for (int i = 0; i < 50; ++i)
    {
        const auto added = c.add("批量", same);
        REQUIRE(added.has_value());
        ids.push_back(added->id);
    }

    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

TEST_CASE("core：load 的 warning 会通过 warnings() 透出来")
{
    testutil::TempFile f;
    testutil::writeFile(f.path(), "{这不是 JSON\n");

    core::Core c{ f.path() };
    REQUIRE(c.load().has_value());
    REQUIRE(c.warnings().size() == 1);
    CHECK(c.warnings()[0].find("第 1 行") != std::string::npos);
}
