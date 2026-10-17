#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "core/store.hpp"
#include "test_util.hpp"

// store —— JSONL 持久化。对应 ROADMAP §2.3 的 8 条行为规格。
//
// 这一层错了不会崩，只会**静默丢数据**，所以每条规格都直接对着
// "写出去再读回来" 验，而不是只测单个函数。

using namespace std::chrono;

namespace
{
    core::Entry fullEntry()
    {
        core::Entry e;
        e.id         = "0123456789ab";
        e.raw_text   = "和小李讨论了方案";
        e.created_at = testutil::at(2026, 10, 18, 9, 30);
        e.occurred   = core::TimeRange{ testutil::at(2026, 10, 17), std::nullopt };
        e.summary    = "讨论了方案";
        e.tags       = { "工作", "会议" };
        e.kind       = core::Kind::Event;
        e.confirmed  = true;
        return e;
    }

    // 逐字段比。不用 Entry::operator== 的原因：那个将来可能会漏加新字段，
    // 而"往返测试"的意义恰恰是**每个字段**都要对得上。
    void checkSameEntry(const core::Entry& a, const core::Entry& b)
    {
        CHECK(a.id == b.id);
        CHECK(a.raw_text == b.raw_text);
        CHECK(a.created_at == b.created_at);
        CHECK(a.summary == b.summary);
        CHECK(a.tags == b.tags);
        // 多一层括号：Kind 是 enum class，没有 operator<<，
        // doctest 的表达式分解会尝试把它 stringify 而编译不过。
        CHECK((a.kind == b.kind));
        CHECK(a.confirmed == b.confirmed);

        REQUIRE(a.occurred.has_value() == b.occurred.has_value());
        if (a.occurred && b.occurred)
        {
            CHECK(a.occurred->start == b.occurred->start);
            REQUIRE(a.occurred->end.has_value() == b.occurred->end.has_value());
            if (a.occurred->end && b.occurred->end)
            {
                CHECK(*a.occurred->end == *b.occurred->end);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 规格 1：文件不存在
// ---------------------------------------------------------------------------

TEST_CASE("store：文件不存在 -> 成功，空列表，无 warning")
{
    testutil::TempFile f;

    const auto r = core::load(f.path());
    REQUIRE(r.has_value());
    CHECK(r->entries.empty());
    CHECK(r->warnings.empty());
}

// ---------------------------------------------------------------------------
// 规格 2：空文件
// ---------------------------------------------------------------------------

TEST_CASE("store：空文件 -> 成功，空列表")
{
    testutil::TempFile f;
    testutil::writeFile(f.path(), "");

    const auto r = core::load(f.path());
    REQUIRE(r.has_value());
    CHECK(r->entries.empty());
    CHECK(r->warnings.empty());
}

// ---------------------------------------------------------------------------
// 规格 3：正常往返，所有字段逐字段相等
// ---------------------------------------------------------------------------

TEST_CASE("store：save 后 load，所有字段逐字段相等")
{
    testutil::TempFile f;

    auto range = fullEntry();
    range.id       = "000000000001";
    range.occurred = core::TimeRange{ testutil::at(2026, 10, 19, 15),
                                      testutil::at(2026, 10, 19, 17) };

    auto point = fullEntry();
    point.id       = "000000000002";
    point.occurred = core::TimeRange{ testutil::at(2026, 10, 20, 8), std::nullopt };

    auto unknown = fullEntry();
    unknown.id       = "000000000003";
    unknown.occurred = std::nullopt;

    const std::vector<core::Entry> in{ fullEntry(), range, point, unknown };

    REQUIRE(core::save(f.path(), in).has_value());

    const auto r = core::load(f.path());
    REQUIRE(r.has_value());
    CHECK(r->warnings.empty());
    REQUIRE(r->entries.size() == in.size());

    for (std::size_t i = 0; i < in.size(); ++i)
    {
        checkSameEntry(in[i], r->entries[i]);
    }
}

// ---------------------------------------------------------------------------
// 规格 4：特殊字符
// ---------------------------------------------------------------------------

TEST_CASE("store：raw_text 含引号、反斜杠、换行、制表、中文，往返一致")
{
    testutil::TempFile f;

    auto e = fullEntry();
    e.raw_text = "他说：\"明天\"\\见\n第二行\t制表符";

    REQUIRE(core::save(f.path(), { e }).has_value());

    const auto r = core::load(f.path());
    REQUIRE(r.has_value());
    CHECK(r->warnings.empty());
    REQUIRE(r->entries.size() == 1);
    checkSameEntry(e, r->entries[0]);

    // 换行必须被转义成 \n 留在**同一行**里，否则 JSONL 的"一行一条"就破了。
    const std::string text = testutil::readFile(f.path());
    CHECK(text.find('\n') == text.size() - 1);   // 只有一个换行：行尾那个
}

// ---------------------------------------------------------------------------
// 规格 5：坏 JSON 行
// ---------------------------------------------------------------------------

TEST_CASE("store：坏 JSON 行 -> 跳过并 warning，其余行照常读")
{
    testutil::TempFile f;
    testutil::writeFile(
        f.path(),
        "{\"id\":\"a\",\"raw_text\":\"第一行\",\"created_at\":\"2026-10-18T00:00:00Z\"}\n"
        "{这不是 JSON\n"
        "{\"id\":\"b\",\"raw_text\":\"第三行\",\"created_at\":\"2026-10-18T00:00:00Z\"}\n");

    const auto r = core::load(f.path());
    REQUIRE(r.has_value());
    REQUIRE(r->entries.size() == 2);
    CHECK(r->entries[0].id == "a");
    CHECK(r->entries[1].id == "b");
    REQUIRE(r->warnings.size() == 1);
    CHECK(r->warnings[0].find("第 2 行") != std::string::npos);
}

TEST_CASE("store：合法 JSON 但不是对象 -> 跳过并 warning")
{
    testutil::TempFile f;
    testutil::writeFile(
        f.path(),
        "[1,2,3]\n"
        "{\"id\":\"ok\",\"raw_text\":\"好行\",\"created_at\":\"2026-10-18T00:00:00Z\"}\n");

    const auto r = core::load(f.path());
    REQUIRE(r.has_value());
    REQUIRE(r->entries.size() == 1);
    CHECK(r->entries[0].id == "ok");
    REQUIRE(r->warnings.size() == 1);
    CHECK(r->warnings[0].find("不是 JSON 对象") != std::string::npos);
}

TEST_CASE("store：空行和 CRLF 都被正确跳过")
{
    testutil::TempFile f;
    testutil::writeFile(
        f.path(),
        "\r\n"
        "{\"id\":\"a\",\"raw_text\":\"一\",\"created_at\":\"2026-10-18T00:00:00Z\"}\r\n"
        "\r\n"
        "{\"id\":\"b\",\"raw_text\":\"二\",\"created_at\":\"2026-10-18T00:00:00Z\"}\r\n");

    const auto r = core::load(f.path());
    REQUIRE(r.has_value());
    CHECK(r->warnings.empty());
    REQUIRE(r->entries.size() == 2);
    CHECK(r->entries[0].id == "a");
    CHECK(r->entries[1].id == "b");
}

TEST_CASE("store：occurred.start 非法 -> 该行跳过并 warning，好行不受影响")
{
    testutil::TempFile f;
    testutil::writeFile(
        f.path(),
        // 坏 start：以前会静默变成 1970-01-01
        "{\"id\":\"bad\",\"raw_text\":\"坏时间\",\"created_at\":\"2026-10-18T00:00:00Z\","
        "\"occurred\":{\"start\":\"garbage\"}}\n"
        // 缺 start 也算坏
        "{\"id\":\"bad2\",\"raw_text\":\"缺字段\",\"created_at\":\"2026-10-18T00:00:00Z\","
        "\"occurred\":{\"end\":\"2026-10-18T10:00:00Z\"}}\n"
        // 好行
        "{\"id\":\"good\",\"raw_text\":\"好记录\",\"created_at\":\"2026-10-18T00:00:00Z\"}\n");

    const auto r = core::load(f.path());
    REQUIRE(r.has_value());
    REQUIRE(r->entries.size() == 1);
    CHECK(r->entries[0].id == "good");
    REQUIRE(r->warnings.size() == 2);
    CHECK(r->warnings[0].find("第 1 行") != std::string::npos);
    CHECK(r->warnings[1].find("第 2 行") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 规格 6：occurred 为空
// ---------------------------------------------------------------------------

TEST_CASE("store：occurred 为空 -> 文件里没有 occurred 键，读回来仍为空")
{
    testutil::TempFile f;

    auto e = fullEntry();
    e.occurred.reset();

    REQUIRE(core::save(f.path(), { e }).has_value());

    const std::string text = testutil::readFile(f.path());
    CHECK(text.find("\"occurred\"") == std::string::npos);

    const auto r = core::load(f.path());
    REQUIRE(r.has_value());
    REQUIRE(r->entries.size() == 1);
    CHECK_FALSE(r->entries[0].occurred.has_value());
}

// ---------------------------------------------------------------------------
// 规格 7：旧文件缺字段
// ---------------------------------------------------------------------------

TEST_CASE("store：旧文件缺字段 -> 不失败，新字段取默认值")
{
    testutil::TempFile f;
    testutil::writeFile(f.path(), "{\"id\":\"old\",\"raw_text\":\"旧记录\"}\n");

    const auto r = core::load(f.path());
    REQUIRE(r.has_value());
    CHECK(r->warnings.empty());
    REQUIRE(r->entries.size() == 1);

    const core::Entry& e = r->entries[0];
    CHECK(e.id == "old");
    CHECK(e.raw_text == "旧记录");
    CHECK(e.created_at == sys_seconds{});
    CHECK(e.summary.empty());
    CHECK(e.tags.empty());
    CHECK((e.kind == core::Kind::Unknown));
    CHECK_FALSE(e.confirmed);
    CHECK_FALSE(e.occurred.has_value());
}

// ---------------------------------------------------------------------------
// 规格 8：覆盖已存在的文件
// ---------------------------------------------------------------------------

TEST_CASE("store：save 覆盖已存在的文件")
{
    testutil::TempFile f;
    testutil::writeFile(f.path(), "旧的垃圾内容\n");

    REQUIRE(core::save(f.path(), { fullEntry() }).has_value());

    const auto r = core::load(f.path());
    REQUIRE(r.has_value());
    CHECK(r->warnings.empty());
    REQUIRE(r->entries.size() == 1);
    checkSameEntry(fullEntry(), r->entries[0]);
}

// ---------------------------------------------------------------------------
// 其它：ISO 转换、读失败
// ---------------------------------------------------------------------------

TEST_CASE("store：ISO 时间往返")
{
    const sys_seconds t = testutil::at(2026, 10, 18, 9, 30);

    CHECK(core::toIso(t) == "2026-10-18T09:30:00Z");

    sys_seconds out{};
    CHECK(core::fromIso("2026-10-18T09:30:00Z", out));
    CHECK(out == t);
}

TEST_CASE("store：fromIso 失败时不改 out")
{
    const sys_seconds keep = testutil::at(2020, 1, 1);
    sys_seconds       out  = keep;

    CHECK_FALSE(core::fromIso("garbage", out));
    CHECK(out == keep);
}

TEST_CASE("store：路径是目录 -> 读失败，而不是空列表")
{
    // 目录能通过 exists()，但打不开成文件流。以前这种错误会被吞掉。
    const auto r = core::load(std::filesystem::temp_directory_path());
    CHECK_FALSE(r.has_value());
    CHECK_FALSE(r.error().empty());
}
