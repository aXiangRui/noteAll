#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "app/corpus.hpp"
#include "app/time_input.hpp"

// 语料回流：append → load → run 一整圈。
// 用临时目录，不污染仓库；时区固定 Asia/Shanghai，断言才确定。

using namespace std::chrono;

namespace
{

const time_zone& shanghai()
{
    static const time_zone* tz = locate_zone("Asia/Shanghai");
    return *tz;
}

std::filesystem::path scratchFile(char const* name)
{
    const auto dir = std::filesystem::temp_directory_path() / "noteall-corpus-test";
    std::filesystem::create_directories(dir);
    const auto file = dir / name;
    std::filesystem::remove(file);
    return file;
}

// 上海本地时间 → UTC
sys_seconds shanghaiUtc(int y, unsigned m, unsigned d, int h, int mi)
{
    const sys_seconds fallback = sys_seconds{ sys_days{ year{ 2026 } / 10 / 30 } };

    return app::parseLocalDateTime(
               std::to_string(y) + "-" + (m < 10 ? "0" : "") + std::to_string(m) + "-" +
                   (d < 10 ? "0" : "") + std::to_string(d) + " " +
                   (h < 10 ? "0" : "") + std::to_string(h) + ":" + (mi < 10 ? "0" : "") +
                   std::to_string(mi),
               fallback, shanghai())
        .value();
}

}  // namespace

TEST_CASE("corpus：追加之后能原样读回来")
{
    const auto file = scratchFile("roundtrip.txt");

    app::ParseCase first;
    first.text  = "明天下午三点跟老张开会";
    first.base  = shanghaiUtc(2026, 10, 30, 8, 0);
    first.start = shanghaiUtc(2026, 10, 31, 15, 0);

    app::ParseCase second;
    second.text  = "8：30~10：30开会";
    second.base  = first.base;
    second.start = shanghaiUtc(2026, 10, 30, 8, 30);
    second.end   = shanghaiUtc(2026, 10, 30, 10, 30);

    CHECK(app::appendParseCase(file, first, shanghai()));
    CHECK(app::appendParseCase(file, second, shanghai()));

    auto loaded = app::loadParseCases(file, shanghai());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);

    CHECK((*loaded)[0].text == first.text);
    CHECK((*loaded)[0].base == first.base);
    REQUIRE((*loaded)[0].start.has_value());
    CHECK(*(*loaded)[0].start == *first.start);
    CHECK_FALSE((*loaded)[0].end.has_value());

    CHECK((*loaded)[1].text == second.text);
    REQUIRE((*loaded)[1].end.has_value());
    CHECK(*(*loaded)[1].end == *second.end);
}

TEST_CASE("corpus：runParseCases 用规则解析器判定通过/失败")
{
    const auto file = scratchFile("run.txt");
    const auto base  = shanghaiUtc(2026, 10, 30, 8, 0);

    app::ParseCase good;
    good.text  = "8：30~10：30开会";
    good.base  = base;
    good.start = shanghaiUtc(2026, 10, 30, 8, 30);
    good.end   = shanghaiUtc(2026, 10, 30, 10, 30);

    app::ParseCase bad;
    bad.text  = "8：30~10：30开会";
    bad.base  = base;
    bad.start = shanghaiUtc(2026, 10, 30, 9, 30);   // 故意写错

    const std::vector<app::ParseCase> cases{ good, bad };
    const auto outcomes = app::runParseCases(cases, shanghai());

    REQUIRE(outcomes.size() == 2);
    CHECK(outcomes[0].startOk);
    CHECK(outcomes[0].endOk);
    CHECK_FALSE(outcomes[1].startOk);
    CHECK(outcomes[0].gotStart == "2026-10-30 08:30");
}

TEST_CASE("corpus：没有时间的原文，期望为空时算通过")
{
    app::ParseCase none;
    none.text = "记一个想法，完全没时间";
    none.base = shanghaiUtc(2026, 10, 30, 8, 0);

    const std::vector<app::ParseCase> cases{ none };
    const auto outcomes = app::runParseCases(cases, shanghai());

    REQUIRE(outcomes.size() == 1);
    CHECK(outcomes[0].startOk);   // 期望没时间，实际也没时间
    CHECK(outcomes[0].gotStart.empty());
}
