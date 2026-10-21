#include "flow/draft_queue.hpp"

#include <utility>

// ===========================================================================
// 草稿队列 —— 实现留给你。
//
// 现在三个函数都是"能编、能链、跑不通"的桩：测试会红，红的地方就是你要写的。
//
// 建议实现顺序（每一步都能单独跑测试）：
//   1. drain 的串行版：只处理 1 条 -> 3 条；跑通"成功 -> markDone"
//   2. 失败分支：retryable=true 时重试，attempts 累计到 maxAttempts 转 abandoned
//   3. 预算：maxItems / maxTime 到点就停，未处理的计入 left
//   4. 陈旧：结果回来重算哈希，不一致 -> skipped，不写回
//   5. workers > 1：并发跑但不超过 workers 个
//   6. runOne / pending
//
// 三条容易踩的坑：
//   * 别在 generator 里写存储 —— 写回只走 store，否则并发下两个线程改同一条；
//   * markDone / markFailed 返回 expected，失败要计入 failed 而不是抛；
//   * maxTime 是**总预算**，不是单条超时。
// ===========================================================================

namespace flow
{

std::uint64_t hashRawText(std::string_view text) noexcept
{
    // FNV-1a 64：够用、无依赖、跨平台稳定。
    // 只用来判"这条笔记改过没有"，不是安全哈希。
    std::uint64_t hash = 1469598103934665603ULL;
    for (char ch : text)
    {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= 1099511628211ULL;
    }
    return hash;
}

DraftQueue::DraftQueue(DraftGenerator generator, DraftStore store, std::size_t workers)
    : generator_(std::move(generator)), store_(std::move(store)), workers_(workers == 0 ? 1 : workers)
{
}

DrainStats DraftQueue::drain(DrainOptions const& options)
{
    (void)options;

    // TODO: 取任务 -> 逐个生成 -> 写回 -> 统计。
    // 现在直接返回全 0，测试会因此变红（这是预期的）。
    return DrainStats{};
}

std::expected<void, std::string> DraftQueue::runOne(std::string_view id)
{
    (void)id;
    return std::unexpected(std::string{"未实现：DraftQueue::runOne"});
}

std::size_t DraftQueue::pending() const
{
    // TODO: 用 store_.load 取一次（limit 给个大值），返回条数。
    return 0;
}

}  // namespace flow
