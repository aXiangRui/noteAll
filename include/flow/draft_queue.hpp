#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/entry.hpp"

// workflow —— 草稿的调度层。
//
// 它回答的问题只有一个：**哪些条目还欠草稿、现在补几条、失败的怎么办。**
//
// 它**不认识**：
//   * HTTP / JSON / model 名（那是 llm 层，通过 DraftGenerator 注入进来）；
//   * 数据结构长什么样（那是 core，通过 DraftStore 注入进来）。
//
// 这么切的好处：调度逻辑（重试、放弃、并发、时间预算、陈旧丢弃）全部可以用
// 内存里的假 store + 假 generator 测，不碰磁盘、不发请求。
//
// 依赖方向：workflow -> core（只用 core::Kind）。**不依赖** llm / UI。
namespace flow
{

// ---------------------------------------------------------------------------
// 数据
// ---------------------------------------------------------------------------

/** 一条"欠草稿"的任务。生成前后用 rawHash 校验，防止把过期草稿写回去。 */
struct DraftJob
{
    std::string   id;
    std::string   rawText;
    std::uint64_t rawHash  = 0;   // rawText 的哈希；回来时再算一次比对
    int           attempts = 0;   // 已经失败过几次
};

/** 生成成功的产物。怎么从模型回复里抠出这些字段，是 DraftGenerator 的事。 */
struct Draft
{
    std::string              summary;
    std::vector<std::string> tags;
    core::Kind               kind = core::Kind::Unknown;
};

/**
 * 生成失败。
 *
 * retryable 由**生成器**决定（它负责把 HTTP/解析错误翻译成"值不值得再试"）：
 *   - 网络超时、429、坏 JSON -> true
 *   - 401/403、请求过长被拒、没配置 -> false
 * 队列只认这个布尔值，不看任何状态码。
 */
struct DraftFailure
{
    std::string message;
    bool        retryable = false;
};

/**
 * 一次"条目 -> 草稿"的转换。
 *
 * 纯函数式注入：真实实现里它构造 prompt、调用 llm::IChatClient、解析回复；
 * 测试里它就是一个 lambda。
 *
 * 注意：生成器内部**不许**写存储 —— 写回由队列通过 DraftStore 统一做，
 * 否则并发下会出现两个地方改同一条记录。
 */
using DraftGenerator = std::function<std::expected<Draft, DraftFailure>(DraftJob const&)>;

/**
 * 存储钩子。
 *
 * 为什么用钩子而不是直接拿 core::Core：
 *   1. 队列不需要知道 JSONL / 原子写 / 锁，那是 store 的事；
 *   2. 测试用内存实现，毫秒级验证"失败 3 次后放弃"这种时序；
 *   3. core 的字段（status/attempts/lastError）由你加，队列不强制它长什么样。
 *
 * 真实实现（建议放在 core 或 CLI 层）大致是：
 *   load        : 扫 entries，取 draftStatus==Pending（includeFailed 时连 Failed），按 created_at 升序，最多 limit 条
 *   markDone    : 找到 id，写入 summary/tags/kind，draftStatus=Done，draftError 清空
 *   markFailed  : 找到 id，draftAttempts+1、draftError=message；
 *                 若 !retryable 或 attempts+1 >= maxAttempts 则 draftStatus=Abandoned，否则 =Failed
 */
struct DraftStore
{
    std::function<std::vector<DraftJob>(std::size_t limit, bool includeFailed)> load;

    // 取某条笔记**当前**的正文；不存在返回 nullopt。
    // 队列用它做陈旧校验：生成期间你改了原文，草稿就作废。
    std::function<std::optional<std::string>(std::string const& id)> currentRawText;

    std::function<std::expected<void, std::string>(DraftJob const& job, Draft const& draft)> markDone;

    std::function<std::expected<void, std::string>(DraftJob const& job, DraftFailure const& failure)> markFailed;
};

struct DrainOptions
{
    // 最多处理几条；0 = 不限（`note draft --all` 用）。
    std::size_t maxItems = 3;

    // 时间预算；0 = 不限。机会式 drain 用它保证"顺手补一点就走"。
    std::chrono::milliseconds maxTime{1500};

    // 是否把 status=Failed 的也捡起来重试。
    bool includeFailed = false;

    // 失败累计到这个次数就放弃（abandoned）。
    int maxAttempts = 3;
};

struct DrainStats
{
    int done      = 0;
    int failed    = 0;   // 本次失败但还会再试
    int abandoned = 0;   // 本次失败且已达上限
    int skipped   = 0;   // 陈旧（rawText 变了）而丢弃
    int left      = 0;   // 预算用尽后仍未处理的数量
};

// ---------------------------------------------------------------------------
// 队列
// ---------------------------------------------------------------------------

/**
 * 草稿队列。
 *
 * 线程模型：调用方从**一个**线程调 drain()/runOne()（CLI 就是这样）；
 * 内部若要开 workers 个线程并发跑任务，那是它自己的事，但对外不承诺线程安全。
 * workers > 1 时每个任务仍由同一个 generator 处理，所以 generator 必须线程安全
 * （真实实现里通常是"每个 worker 一个 llm client"，由上层闭包保证）。
 */
class DraftQueue
{
public:
    DraftQueue(DraftGenerator generator, DraftStore store, std::size_t workers = 1);

    /**
     * 跑一轮。
     *
     * 必须保证：
     *   - 成功 -> markDone；失败 -> markFailed（并把 retryable / maxAttempts 的判定结果体现出来）；
     *   - 任务开始前与结果回来各算一次 rawText 哈希，不一致就 skipped，不写回；
     *   - 达到 maxItems 或 maxTime 后停止，未处理的计入 left（它们仍是 pending）；
     *   - store 的写失败不能吞掉：计入 failed 并保留错误（至少写日志）。
     *
     * 返回 DrainStats 而不是 expected：单条失败不是"整轮失败"，用计数表达。
     */
    DrainStats drain(DrainOptions const& options);

    /** 立刻补一条（`note draft <id>`）。找不到 / 存储写失败 -> error。 */
    std::expected<void, std::string> runOne(std::string_view id);

    /** 还剩多少条欠账（`note status` 用）。 */
    std::size_t pending() const;

private:
    DraftGenerator generator_;
    DraftStore     store_;
    std::size_t    workers_ = 1;
};

/** 计算 rawText 的哈希（供 store 与队列用同一个口径）。 */
std::uint64_t hashRawText(std::string_view text) noexcept;

}  // namespace flow
