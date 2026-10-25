#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include "core/core.hpp"
#include "core/file_lock.hpp"
#include "core/store.hpp"
#include "test_util.hpp"

// 记录的"基石"三件套：格式版本、来源与墓碑、并发写不丢数据。
//
// 这一组用例的意义和别处不同：它们保护的是**数据**，不是功能。
// 坏了不会报错，只会悄悄少一条记录 —— 所以每条都必须在 CI 里常驻。

namespace
{

core::Entry makeEntry(std::string id, std::string text, std::chrono::sys_seconds at)
{
    core::Entry e;
    e.id         = std::move(id);
    e.raw_text   = std::move(text);
    e.created_at = at;
    return e;
}

}  // namespace

TEST_CASE("durability：文件第一行是格式版本头，旧文件照样能读")
{
    testutil::TempFile f;

    // 1) 新写的文件带版本头
    REQUIRE(core::save(f.path(), { makeEntry("a", "正文", testutil::at(2026, 10, 1)) }).has_value());
    const std::string text = testutil::readFile(f.path());
    CHECK(text.rfind("{\"schema\":", 0) == 0);

    // 2) 没有头部的"上古文件"（版本 1）也能读，且缺字段用默认值补齐
    testutil::writeFile(f.path(),
                        R"({"id":"old","raw_text":"老数据","created_at":"2026-09-01T00:00:00Z","tags":[],"kind":"note","confirmed":false})" "\n");

    const auto loaded = core::load(f.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 1);
    CHECK(loaded->entries[0].id == "old");
    CHECK(core::toString(loaded->entries[0].source) == std::string{"manual"});   // 老数据默认手敲
    CHECK_FALSE(loaded->entries[0].deleted_at.has_value());

    core::Core c{ f.path() };
    REQUIRE(c.load().has_value());
    CHECK(c.list().size() == 1);
}

TEST_CASE("durability：文件版本比程序新时拒绝读取，绝不写坏它")
{
    testutil::TempFile f;
    testutil::writeFile(f.path(), "{\"schema\":999}\n{\"id\":\"a\",\"raw_text\":\"x\",\"created_at\":\"2026-10-01T00:00:00Z\"}\n");

    const auto loaded = core::load(f.path());
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().find("升级") != std::string::npos);

    // 关键：拒绝之后文件一个字节都没被改。
    const std::string after = testutil::readFile(f.path());
    CHECK(after.find("\"schema\":999") != std::string::npos);
}

TEST_CASE("durability：source 字段往返，未知值降级为 manual")
{
    testutil::TempFile f;

    auto e = makeEntry("a", "微信里的一句", testutil::at(2026, 10, 2));
    e.source = core::Source::WeChat;

    REQUIRE(core::save(f.path(), { e }).has_value());

    const auto loaded = core::load(f.path());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 1);
    CHECK(core::toString(loaded->entries[0].source) == std::string{"wechat"});

    // 未知来源不报错、不丢行 —— 历史数据可能来自更老的版本
    testutil::writeFile(f.path(),
                        R"({"id":"b","raw_text":"x","created_at":"2026-10-02T00:00:00Z","source":"从未来来的"})" "\n");
    const auto again = core::load(f.path());
    REQUIRE(again.has_value());
    REQUIRE(again->entries.size() == 1);
    CHECK(core::toString(again->entries[0].source) == std::string{"manual"});
}

TEST_CASE("durability：删除是打墓碑，能恢复，purge 才是真删")
{
    testutil::TempFile f;
    core::Core c{ f.path() };
    REQUIRE(c.load().has_value());

    const auto a = c.add("要删的", testutil::at(2026, 10, 3));
    REQUIRE(a.has_value());

    SUBCASE("墓碑对上层不可见，但还在文件里")
    {
        REQUIRE(c.remove(a->id).has_value());

        CHECK(c.list().empty());                       // list 不含墓碑
        CHECK_FALSE(c.find(a->id).has_value());        // find 只查活的
        REQUIRE(c.findAny(a->id).has_value());         // findAny 能看见
        CHECK(c.findAny(a->id)->deleted_at.has_value());

        // 换实例（= 重新读盘）依然能看见墓碑 —— 说明真的落盘了
        core::Core reloaded{ f.path() };
        REQUIRE(reloaded.load().has_value());
        CHECK(reloaded.list().empty());
        REQUIRE(reloaded.findAny(a->id).has_value());
    }

    SUBCASE("restore 恢复")
    {
        REQUIRE(c.remove(a->id).has_value());
        REQUIRE(c.restore(a->id).has_value());

        CHECK(c.list().size() == 1);
        REQUIRE(c.find(a->id).has_value());
        CHECK_FALSE(c.find(a->id)->deleted_at.has_value());

        // 恢复一条没删过的记录是错误
        CHECK_FALSE(c.restore(a->id).has_value());
    }

    SUBCASE("purge 物理删除")
    {
        REQUIRE(c.remove(a->id).has_value());
        REQUIRE(c.purge(a->id).has_value());

        CHECK_FALSE(c.findAny(a->id).has_value());

        core::Core reloaded{ f.path() };
        REQUIRE(reloaded.load().has_value());
        CHECK_FALSE(reloaded.findAny(a->id).has_value());
    }
}

TEST_CASE("durability：update 改字段并落盘，找不到就报错")
{
    testutil::TempFile f;
    core::Core c{ f.path() };
    REQUIRE(c.load().has_value());

    const auto added = c.add("原文不变", testutil::at(2026, 10, 4));
    REQUIRE(added.has_value());

    auto entry      = *c.find(added->id);
    entry.summary   = "补上的摘要";
    entry.tags      = { "工作", "项目A" };
    entry.kind      = core::Kind::Event;
    entry.confirmed = true;

    REQUIRE(c.update(entry).has_value());

    core::Core reloaded{ f.path() };
    REQUIRE(reloaded.load().has_value());

    const auto persisted = reloaded.find(added->id);
    REQUIRE(persisted.has_value());
    CHECK(persisted->summary == "补上的摘要");
    CHECK(persisted->tags.size() == 2);
    CHECK(core::toString(persisted->kind) == std::string{"event"});
    CHECK(persisted->confirmed);
    CHECK(persisted->raw_text == "原文不变");   // 原文一个字都没动

    // id 不存在 -> 错误
    auto ghost      = entry;
    ghost.id        = "不存在的id";
    CHECK_FALSE(c.update(ghost).has_value());
}

TEST_CASE("durability：两个实例交错写，不丢更新（锁 + 重读）")
{
    testutil::TempFile f;

    core::Core first{ f.path() };
    core::Core second{ f.path() };
    REQUIRE(first.load().has_value());
    REQUIRE(second.load().has_value());

    // 两个实例都基于"空文件"启动，然后交错写。
    // 如果实现是"用启动时的内存快照覆盖"，第二次写会把第一条冲掉。
    REQUIRE(first.add("第一条", testutil::at(2026, 10, 5, 9)).has_value());
    REQUIRE(second.add("第二条", testutil::at(2026, 10, 5, 10)).has_value());

    core::Core reloaded{ f.path() };
    REQUIRE(reloaded.load().has_value());
    CHECK(reloaded.list().size() == 2);   // 两条都要在
}

TEST_CASE("durability：文件锁是独占的，第二个持有者会超时")
{
    testutil::TempFile f;

    auto acquired = core::FileLock::acquire(f.path(), std::chrono::milliseconds{200});
    REQUIRE(acquired.has_value());

    // expected<FileLock> 不能 reset（FileLock 不可赋值），用 optional 管生命周期。
    std::optional<core::FileLock> held = std::move(*acquired);

    auto second = core::FileLock::acquire(f.path(), std::chrono::milliseconds{200});
    CHECK_FALSE(second.has_value());
    if (!second) { CHECK(second.error().find("正在写") != std::string::npos); }

    // 释放之后能再拿到
    held.reset();
    auto third = core::FileLock::acquire(f.path(), std::chrono::milliseconds{200});
    CHECK(third.has_value());
}
