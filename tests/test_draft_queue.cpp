#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "flow/draft_queue.hpp"

// workflow —— 草稿队列。
//
// 这些用例**现在应该全是红的**（实现是桩），红的地方就是 src/workflow/draft_queue.cpp
// 要写的东西。每修好一块就绿一块。
//
// 全部用内存 store + 假 generator：不碰磁盘、不发请求，毫秒级。

namespace
{

using flow::Draft;
using flow::DraftFailure;
using flow::DraftJob;
using flow::DraftQueue;
using flow::DraftStore;

/** 内存版存储：够测试用，也顺便演示 core 那边大概要怎么接。 */
struct MemoryStore
{
    struct Row
    {
        std::string id;
        std::string text;
        int         attempts = 0;
        bool        hasDraft = false;
        std::string lastError;
    };

    std::vector<Row> rows;

    // 观测用
    int  markDoneCalls   = 0;
    int  markFailedCalls = 0;
    bool failWrites      = false;

    // 陈旧测试用：模拟"生成期间用户改了原文"
    void editText(std::string const& id, std::string newText)
    {
        for (Row& row : rows)
        {
            if (row.id == id) { row.text = std::move(newText); }
        }
    }

    DraftStore hooks()
    {
        DraftStore store;

        store.load = [this](std::size_t limit, bool /*includeFailed*/) {
            std::vector<DraftJob> jobs;
            for (Row const& row : rows)
            {
                if (row.hasDraft) { continue; }  // 已有草稿的不算欠账
                if (limit != 0 && jobs.size() >= limit) { break; }
                jobs.push_back(DraftJob{row.id, row.text, flow::hashRawText(row.text), row.attempts});
            }
            return jobs;
        };

        store.currentRawText = [this](std::string const& id) -> std::optional<std::string> {
            for (Row const& row : rows)
            {
                if (row.id == id) { return row.text; }
            }
            return std::nullopt;
        };

        store.markDone = [this](DraftJob const& job, Draft const& draft) -> std::expected<void, std::string> {
            ++markDoneCalls;
            if (failWrites) { return std::unexpected(std::string{"模拟写盘失败"}); }
            for (Row& row : rows)
            {
                if (row.id != job.id) { continue; }
                row.hasDraft  = true;
                row.lastError = draft.summary;  // 借位存一下，方便断言
                return {};
            }
            return std::unexpected(std::string{"找不到条目"});
        };

        store.markFailed = [this](DraftJob const& job, DraftFailure const& failure) -> std::expected<void, std::string> {
            ++markFailedCalls;
            if (failWrites) { return std::unexpected(std::string{"模拟写盘失败"}); }
            for (Row& row : rows)
            {
                if (row.id != job.id) { continue; }
                row.attempts += 1;
                row.lastError = failure.message;
                return {};
            }
            return std::unexpected(std::string{"找不到条目"});
        };

        return store;
    }
};

/** 永远成功的生成器。 */
Draft alwaysOk(DraftJob const&)
{
    return Draft{"摘要", {"工作"}, core::Kind::Event};
}

/** 记录调用次数的生成器。 */
struct CountingGenerator
{
    int calls = 0;

    std::expected<Draft, DraftFailure> operator()(DraftJob const&)
    {
        ++calls;
        return Draft{"摘要", {}, core::Kind::Note};
    }
};

}  // namespace

TEST_CASE("draft_queue：串行跑完所有欠账，每条都写回成功")
{
    MemoryStore store;
    store.rows = {{"a", "第一条", 0, false, ""},
                  {"b", "第二条", 0, false, ""},
                  {"c", "第三条", 0, false, ""}};

    CountingGenerator generator;
    DraftQueue       queue(generator, store.hooks(), /*workers=*/1);

    flow::DrainOptions options;
    options.maxItems = 0;  // 不限条数
    options.maxTime  = std::chrono::milliseconds{0};

    const auto stats = queue.drain(options);

    // 桩实现返回全 0 —— 这个用例现在就是红的。
    CHECK(stats.done == 3);
    CHECK(stats.failed == 0);
    CHECK(store.markDoneCalls == 3);
    CHECK(generator.calls == 3);
    CHECK_EQ(queue.pending(), 0u);
}

TEST_CASE("draft_queue：maxItems 到点就停，剩下的仍欠账")
{
    MemoryStore store;
    for (int i = 0; i < 4; ++i)
    {
        store.rows.push_back({"id" + std::to_string(i), "正文", 0, false, ""});
    }

    DraftQueue queue(alwaysOk, store.hooks(), 1);

    flow::DrainOptions options;
    options.maxItems = 2;
    options.maxTime  = std::chrono::milliseconds{0};

    const auto stats = queue.drain(options);

    CHECK(stats.done == 2);
    CHECK(stats.left == 2);
    CHECK_EQ(queue.pending(), 2u);
}

TEST_CASE("draft_queue：不可重试失败只写一次 markFailed，不进重试")
{
    MemoryStore store;
    store.rows = {{"a", "会失败的一条", 0, false, ""}};

    DraftQueue queue(
        [](DraftJob const&) -> std::expected<Draft, DraftFailure> {
            return std::unexpected(DraftFailure{"密钥无效", /*retryable=*/false});
        },
        store.hooks(),
        1);

    flow::DrainOptions options;
    options.maxItems  = 0;
    options.maxTime   = std::chrono::milliseconds{0};
    options.maxAttempts = 3;

    const auto stats = queue.drain(options);

    CHECK(store.markFailedCalls == 1);   // 不可重试 -> 只记一次
    CHECK(stats.abandoned == 1);
    CHECK(stats.done == 0);
}

TEST_CASE("draft_queue：可重试失败会重试，第 2 次成功算 done")
{
    MemoryStore store;
    store.rows = {{"a", "第一次超时", 0, false, ""}};

    int calls = 0;
    DraftQueue queue(
        [&calls](DraftJob const&) -> std::expected<Draft, DraftFailure> {
            ++calls;
            if (calls == 1) { return std::unexpected(DraftFailure{"超时", /*retryable=*/true}); }
            return Draft{"第二次成功", {}, core::Kind::Event};
        },
        store.hooks(),
        1);

    flow::DrainOptions options;
    options.maxItems    = 0;
    options.maxTime     = std::chrono::milliseconds{0};
    options.maxAttempts = 3;

    const auto stats = queue.drain(options);

    CHECK(stats.done == 1);
    CHECK(stats.failed == 1);        // 第一次失败计一次
    CHECK(calls == 2);
    CHECK(store.markDoneCalls == 1);
}

TEST_CASE("draft_queue：一直失败，达到 maxAttempts 后放弃")
{
    MemoryStore store;
    store.rows = {{"a", "永不成功", 0, false, ""}};

    DraftQueue queue(
        [](DraftJob const&) -> std::expected<Draft, DraftFailure> {
            return std::unexpected(DraftFailure{"一直坏", /*retryable=*/true});
        },
        store.hooks(),
        1);

    flow::DrainOptions options;
    options.maxItems    = 0;
    options.maxTime     = std::chrono::milliseconds{0};
    options.maxAttempts = 3;

    const auto stats = queue.drain(options);

    // 最多试 3 次就停，不能无限重试（毒丸）
    CHECK(stats.abandoned == 1);
    CHECK(stats.done == 0);
    CHECK(store.rows[0].attempts >= 1);
    CHECK(store.rows[0].attempts <= 3);
}

TEST_CASE("draft_queue：生成期间原文被改，草稿作废（skipped，不写回）")
{
    MemoryStore store;
    store.rows = {{"a", "原始正文", 0, false, ""}};

    DraftQueue queue(
        [&store](DraftJob const& job) -> std::expected<Draft, DraftFailure> {
            // 模拟"生成期间用户编辑了这条笔记"
            store.editText(job.id, "用户改过的正文");
            return Draft{"基于旧正文的摘要", {}, core::Kind::Note};
        },
        store.hooks(),
        1);

    flow::DrainOptions options;
    options.maxItems = 0;
    options.maxTime  = std::chrono::milliseconds{0};

    const auto stats = queue.drain(options);

    CHECK(stats.skipped == 1);
    CHECK(stats.done == 0);
    CHECK(store.markDoneCalls == 0);  // 关键：绝不能把过期草稿写回去
}

TEST_CASE("draft_queue：写盘失败计入 failed，不吞掉")
{
    MemoryStore store;
    store.rows      = {{"a", "正文", 0, false, ""}};
    store.failWrites = true;

    DraftQueue queue(alwaysOk, store.hooks(), 1);

    flow::DrainOptions options;
    options.maxItems = 0;
    options.maxTime  = std::chrono::milliseconds{0};

    const auto stats = queue.drain(options);

    CHECK(stats.failed >= 1);
    CHECK(stats.done == 0);
}

TEST_CASE("draft_queue：maxTime 预算到点就收手")
{
    MemoryStore store;
    for (int i = 0; i < 6; ++i)
    {
        store.rows.push_back({"id" + std::to_string(i), "正文", 0, false, ""});
    }

    DraftQueue queue(
        [](DraftJob const&) -> std::expected<Draft, DraftFailure> {
            std::this_thread::sleep_for(std::chrono::milliseconds{60});
            return Draft{"慢", {}, core::Kind::Note};
        },
        store.hooks(),
        1);

    flow::DrainOptions options;
    options.maxItems = 0;
    options.maxTime  = std::chrono::milliseconds{100};  // 大约只够 1~2 条

    const auto stats = queue.drain(options);

    CHECK(stats.done >= 1);
    CHECK(stats.done <= 3);
    CHECK(stats.left >= 1);
}

TEST_CASE("draft_queue：workers=2 时并发不超过 2")
{
    MemoryStore store;
    for (int i = 0; i < 4; ++i)
    {
        store.rows.push_back({"id" + std::to_string(i), "正文", 0, false, ""});
    }

    std::atomic<int> inFlight{0};
    std::atomic<int> peak{0};
    std::mutex       mutex;

    DraftQueue queue(
        [&](DraftJob const&) -> std::expected<Draft, DraftFailure> {
            const int now = inFlight.fetch_add(1) + 1;
            int       seen = peak.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
            inFlight.fetch_sub(1);
            return Draft{"并发", {}, core::Kind::Note};
        },
        store.hooks(),
        2);

    flow::DrainOptions options;
    options.maxItems = 0;
    options.maxTime  = std::chrono::milliseconds{0};

    const auto stats = queue.drain(options);
    (void)mutex;

    CHECK(stats.done == 4);
    CHECK(peak.load() <= 2);   // 不能超过 workers
    CHECK(peak.load() >= 2);   // 也不该退化成串行
}

TEST_CASE("draft_queue：runOne 能单独补一条")
{
    MemoryStore store;
    store.rows = {{"a", "正文", 0, false, ""}};

    DraftQueue queue(alwaysOk, store.hooks(), 1);

    auto single = queue.runOne("a");
    CHECK(single.has_value());
    CHECK(store.markDoneCalls == 1);

    auto missing = queue.runOne("不存在");
    CHECK_FALSE(missing.has_value());
}
