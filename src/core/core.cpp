#include "core/core.hpp"

#include "core/store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <utility>
#include <vector>

// Core —— 门面实现。
//
// core.hpp 对**上层**藏住了 store，但这里当然要用它 —— 这正是分层的意义：
// 依赖被限制在实现文件里，不会顺着头文件泄漏给所有人。
//
// 下面函数的顺序和 core.hpp 里声明的顺序一致，方便并排对账。

namespace core
{
namespace
{
    // entries_ 的不变量：**永远按 created_at 倒序**。
    // 在这个地方维持住，list() 就不用每次现排序 —— 也才能返回 const&。
    //
    // 为什么要拿 id 做次级键：std::sort **不稳定**，同一秒内加的多条
    // 每次运行顺序可能不一样。加个稳定的次级键，顺序就固定了。
    void sortNewestFirst(std::vector<Entry>& entries)
    {
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b)
                  {
                      if (a.created_at != b.created_at)
                      {
                          return a.created_at > b.created_at;   // 新的在前
                      }
                      return a.id > b.id;
                  });
    }
}   // namespace

// ---------------------------------------------------------------------------
// 构造 / 读盘
// ---------------------------------------------------------------------------

Core::Core(std::filesystem::path dataFile)
    : path_(std::move(dataFile))
{
}

std::expected<void, std::string> Core::load()
{
    // 这个 core:: 前缀不能省。
    // 类成员名 load 会**把外层的自由函数 load 藏起来**（成员优先），
    // 不加限定的话 load(path_) 会去找 Core::load 自己。
    auto loaded = core::load(path_);
    if (!loaded)
    {
        return std::unexpected(loaded.error());
    }

    entries_  = std::move(loaded->entries);
    warnings_ = std::move(loaded->warnings);

    sortNewestFirst(entries_);

    return {};
}

const std::vector<std::string>& Core::warnings() const
{
    return warnings_;
}

// ---------------------------------------------------------------------------
// 增删查
// ---------------------------------------------------------------------------

std::expected<Entry, std::string> Core::add(std::string_view         rawText,
                                            std::chrono::sys_seconds createdAt,
                                            std::optional<TimeRange> occurred)
{
    Entry e;

    // raw_text **原样存**：不 trim、不去空格、不改一个字。
    // 这是整个项目的核心不变量 —— 别的字段以后都能重新生成，原文不能。
    e.raw_text   = std::string{rawText};
    e.id         = makeId(entries_);
    e.created_at = createdAt;

    // 解析结果原样收下。空 = 解析器没认出来，保持"时间未知"，不猜。
    e.occurred = std::move(occurred);

    // tags / kind / confirmed 保持默认（空、Unknown、false）——
    // 那些是 M2 的事。

    // 先改内存
    entries_.push_back(e);
    sortNewestFirst(entries_);

    // 再落盘；失败就回滚
    if (const auto saved = save(path_, entries_); !saved)
    {
        // 回滚：把刚加的那条删掉。
        // 注意是按 id 找而不是 pop_back —— 上面排过序了，它不一定在末尾。
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [&](const Entry& x) { return x.id == e.id; });
        if (it != entries_.end())
        {
            entries_.erase(it);
        }
        return std::unexpected(saved.error());
    }

    return e;
}

const std::vector<Entry>& Core::list() const
{
    // 直接返回：entries_ 一直维持着"倒序"这个不变量（见文件开头的注释）。
    return entries_;
}

std::optional<Entry> Core::find(std::string_view id) const
{
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const Entry& e) { return e.id == id; });

    if (it == entries_.end())
    {
        // 找不到是预期内的结果，不是错误 —— 空 optional 比抛异常合适。
        return std::nullopt;
    }
    return *it;
}

std::expected<void, std::string> Core::remove(std::string_view id)
{
    // 1. 先只找，不删。边遍历边删会让迭代器失效。
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const Entry& e) { return e.id == id; });

    // 2. 没找到 —— expected 的失败，把原因带上
    if (it == entries_.end())
    {
        return std::unexpected("没有这条记录: " + std::string{id});
    }

    // 3. 删之前先记住内容**和位置**：
    //    内容 —— 落盘失败要原样塞回去
    //    位置 —— 塞回去还得在原来的位置，不能跑到末尾
    const std::size_t pos     = static_cast<std::size_t>(it - entries_.begin());
    const Entry       removed = *it;

    // 4. 改内存
    entries_.erase(it);

    // 5. 落盘；失败就把内存改回去，保证内存和文件一致
    if (const auto saved = save(path_, entries_); !saved)
    {
        entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(pos), removed);
        return std::unexpected(saved.error());
    }

    return {};
}

// ---------------------------------------------------------------------------
// id 生成
// ---------------------------------------------------------------------------

std::string makeId(const std::vector<Entry>& existing)
{
    // static：随机数引擎只初始化一次，别每次调用都重建（重建可能拿到相同的种子）。
    static std::mt19937_64                              rng{std::random_device{}()};
    static std::uniform_int_distribution<std::uint64_t> dist;

    for (;;)
    {
        // 48 bit 随机，格式化成 12 位十六进制（不够 12 位前面补 0）
        const std::string id = std::format("{:012x}", dist(rng) & 0xFFFFFFFFFFFFull);

        const bool dup = std::any_of(existing.begin(), existing.end(),
                                     [&](const Entry& e) { return e.id == id; });
        if (!dup)
        {
            return id;
        }
        // 撞了就重摇。48 bit 撞一次的概率可以忽略，但赌注是数据，所以还是查。
    }
}

}   // namespace core
