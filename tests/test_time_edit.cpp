#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "app/entry_service.hpp"
#include "core/core.hpp"

// 「改一条记录的时间」在数据层到底能不能落盘。
//
// 这条用例是用户报的 bug 的直接回归：如果原来就有时间，保存新时间不生效 ——
// 先在最底层把这件事钉死，再去看 UI 是不是传错了值。

using namespace std::chrono;

namespace
{

std::filesystem::path scratchFile(char const* name)
{
    const auto dir = std::filesystem::temp_directory_path() / "noteall-time-edit-test";
    std::filesystem::create_directories(dir);
    const auto file = dir / name;
    std::filesystem::remove(file);
    std::filesystem::remove(std::filesystem::path{ file }.concat(".lock"));
    return file;
}

sys_seconds at(int day, int hour)
{
    return sys_days{ year{ 2026 } / 10 / day } + hours{ hour };
}

}  // namespace

TEST_CASE("update：已有时间的记录可以改成新时间，并且能落盘读回")
{
    const auto file = scratchFile("change-time.jsonl");

    core::Core note{ file };
    REQUIRE(note.load().has_value());

    auto added = note.add("开会", at(30, 0), core::TimeRange{ at(30, 1), std::nullopt });
    REQUIRE(added.has_value());
    CHECK(added->occurred->start == at(30, 1));

    // 改成另一个时间（相当于用户在详情面板里手改）
    core::Entry changed = *added;
    changed.occurred    = core::TimeRange{ at(31, 9), at(31, 11) };
    changed.occurred_source = core::TimeSource::Manual;
    REQUIRE(note.update(changed).has_value());

    // 重新读盘（新实例，确保不是内存里的假象）
    core::Core reopened{ file };
    REQUIRE(reopened.load().has_value());

    const auto reread = reopened.findAny(added->id);
    REQUIRE(reread.has_value());
    REQUIRE(reread->occurred.has_value());
    CHECK(reread->occurred->start == at(31, 9));
    REQUIRE(reread->occurred->end.has_value());
    CHECK(*reread->occurred->end == at(31, 11));
    CHECK(core::toString(reread->occurred_source) == std::string{"manual"});
}

TEST_CASE("update：把时间清空也能落盘")
{
    const auto file = scratchFile("clear-time.jsonl");

    core::Core note{ file };
    REQUIRE(note.load().has_value());

    auto added = note.add("没时间了", at(30, 0), core::TimeRange{ at(30, 1), std::nullopt });
    REQUIRE(added.has_value());

    core::Entry cleared = *added;
    cleared.occurred.reset();
    cleared.occurred_source = core::TimeSource::None;
    REQUIRE(note.update(cleared).has_value());

    core::Core reopened{ file };
    REQUIRE(reopened.load().has_value());

    const auto reread = reopened.findAny(added->id);
    REQUIRE(reread.has_value());
    CHECK_FALSE(reread->occurred.has_value());   // ★ 真的清掉了
    CHECK(core::toString(reread->occurred_source) == std::string{"none"});
}

TEST_CASE("recomputeTimeForContent：手改/AI 的时间不被改内容覆盖")
{
    core::Entry entry;
    entry.created_at = sys_days{ year{ 2026 } / 10 / 30 } + hours{ 8 };
    entry.raw_text   = "周三读书会";                      // 只有日期 → 规则会解析成 0 点

    // 用户手改成了 20:00
    entry.occurred        = core::TimeRange{ sys_days{ year{ 2026 } / 11 / 4 } + hours{ 12 },
                                             std::nullopt };   // UTC 12:00 = 上海 20:00
    entry.occurred_source = core::TimeSource::Manual;

    const auto kept = app::recomputeTimeForContent(entry, "周三读书会（改个错别字）");
    CHECK(kept.preservedManual);                            // ★ 保留
    REQUIRE(kept.occurred.has_value());
    CHECK(kept.occurred->start == entry.occurred->start);   // 还是 20:00
    CHECK(core::toString(kept.source) == std::string{"manual"});

    // AI 来源同样保留
    entry.occurred_source = core::TimeSource::Ai;
    const auto keptAi = app::recomputeTimeForContent(entry, entry.raw_text);
    CHECK(keptAi.preservedManual);
    CHECK(keptAi.occurred->start == entry.occurred->start);
}

TEST_CASE("recomputeTimeForContent：规则来源的时间按新原文重算")
{
    core::Entry entry;
    entry.created_at      = sys_days{ year{ 2026 } / 10 / 30 } + hours{ 8 };
    entry.raw_text        = "周三读书会";
    entry.occurred        = core::TimeRange{ sys_days{ year{ 2026 } / 11 / 4 }, std::nullopt };
    entry.occurred_source = core::TimeSource::Rule;

    // 新原文仍然只有日期 → 重算成那天 0 点（上海 11-04 00:00 = UTC 11-03 16:00）
    const auto recalculated = app::recomputeTimeForContent(entry, "周三读书会");
    REQUIRE(recalculated.occurred.has_value());
    CHECK(recalculated.occurred->start == sys_days{ year{ 2026 } / 11 / 3 } + hours{ 16 });
    CHECK(core::toString(recalculated.source) == std::string{"rule"});
    CHECK_FALSE(recalculated.preservedManual);

    // 新原文完全没有时间 → 清空
    const auto cleared = app::recomputeTimeForContent(entry, "随手记一句，没有任何时间");
    CHECK_FALSE(cleared.occurred.has_value());
    CHECK(core::toString(cleared.source) == std::string{"none"});
}
