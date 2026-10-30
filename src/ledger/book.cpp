#include "ledger/book.hpp"

#include <algorithm>
#include <cstdio>
#include <random>

#include "core/file_lock.hpp"
#include "core/note_log.hpp"
#include "ledger/store.hpp"

namespace ledger
{

namespace
{

std::string makeId()
{
    static std::mt19937_64 rng{ std::random_device{}() };
    char                  buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%012llx",
                  static_cast<unsigned long long>(rng() & 0xFFFFFFFFFFFFULL));
    return buffer;
}

bool newestFirst(Transaction const& a, Transaction const& b)
{
    if (a.occurred != b.occurred) { return a.occurred > b.occurred; }
    return a.created_at > b.created_at;
}

void reindex(std::vector<Transaction> const& all, std::vector<Transaction>& visible)
{
    visible.clear();
    for (const auto& t : all)
    {
        if (!t.deleted_at) { visible.push_back(t); }
    }
    std::sort(visible.begin(), visible.end(), newestFirst);
}

}  // namespace

Book::Book(std::filesystem::path file) : file_(std::move(file))
{
}

std::expected<void, std::string> Book::load()
{
    auto loaded = ledger::load(file_);
    if (!loaded) { return std::unexpected(loaded.error()); }

    all_      = std::move(loaded->transactions);
    warnings_ = std::move(loaded->warnings);
    reindex(all_, visible_);
    return {};
}

std::expected<Transaction, std::string> Book::add(std::int64_t             amountCents,
                                                  Direction                direction,
                                                  std::chrono::sys_seconds occurred,
                                                  std::string              category,
                                                  std::string              note,
                                                  std::chrono::sys_seconds createdAt)
{
    if (amountCents <= 0) { return std::unexpected("金额必须大于 0"); }

    auto lock = core::FileLock::acquire(file_);
    if (!lock)
    {
        core::appendLog(file_, core::LogLevel::Error, lock.error());
        return std::unexpected(lock.error());
    }

    // 拿磁盘上的最新状态（别的窗口可能刚记了一笔）
    if (auto reloaded = ledger::load(file_); !reloaded) { return std::unexpected(reloaded.error()); }

    Transaction t;
    t.id          = makeId();
    t.amountCents = amountCents;
    t.direction   = direction;
    t.occurred    = occurred;
    t.category    = category.empty() ? "其他" : std::move(category);
    t.note        = std::move(note);
    t.created_at  = createdAt;

    const auto before = all_;
    all_.push_back(t);

    if (auto saved = save(file_, all_); !saved)
    {
        all_ = before;
        reindex(all_, visible_);
        core::appendLog(file_, core::LogLevel::Error, saved.error());
        return std::unexpected(saved.error());
    }

    reindex(all_, visible_);
    return t;
}

std::expected<void, std::string> Book::remove(std::string_view                        id,
                                              std::optional<std::chrono::sys_seconds> deletedAt)
{
    auto lock = core::FileLock::acquire(file_);
    if (!lock)
    {
        core::appendLog(file_, core::LogLevel::Error, lock.error());
        return std::unexpected(lock.error());
    }

    if (auto reloaded = ledger::load(file_); !reloaded) { return std::unexpected(reloaded.error()); }

    auto it = std::find_if(all_.begin(), all_.end(), [&](Transaction const& t) {
        return t.id == id && !t.deleted_at;
    });
    if (it == all_.end()) { return std::unexpected("没有这笔记录: " + std::string{ id }); }

    const auto before = all_;
    it->deleted_at = deletedAt.value_or(std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now()));

    if (auto saved = save(file_, all_); !saved)
    {
        all_ = before;
        reindex(all_, visible_);
        core::appendLog(file_, core::LogLevel::Error, saved.error());
        return std::unexpected(saved.error());
    }

    reindex(all_, visible_);
    return {};
}

Summary Book::summary(std::chrono::year_month month) const
{
    Summary result;

    // 用本地时区判"哪个月" —— 账是给人看的，永远按本地说
    const auto* tz = std::chrono::current_zone();

    for (const auto& t : visible_)
    {
        const std::chrono::zoned_time      zt{ tz, t.occurred };
        const std::chrono::year_month_day  ymd{
            std::chrono::floor<std::chrono::days>(zt.get_local_time()) };
        const std::chrono::year_month      ym{ ymd.year(), ymd.month() };

        if (ym != month) { continue; }

        ++result.count;
        if (t.direction == Direction::Income) { result.incomeCents += t.amountCents; }
        else { result.expenseCents += t.amountCents; }
    }

    // 按分类汇总（支出），金额降序
    for (const auto& t : visible_)
    {
        if (t.direction != Direction::Expense) { continue; }

        const std::chrono::zoned_time      zt{ tz, t.occurred };
        const std::chrono::year_month_day  ymd{
            std::chrono::floor<std::chrono::days>(zt.get_local_time()) };
        const std::chrono::year_month      ym{ ymd.year(), ymd.month() };
        if (ym != month) { continue; }

        auto it = std::find_if(result.byCategory.begin(), result.byCategory.end(),
                               [&](CategorySum const& s) { return s.category == t.category; });
        if (it == result.byCategory.end())
        {
            result.byCategory.push_back(CategorySum{ t.category, t.amountCents });
        }
        else
        {
            it->cents += t.amountCents;
        }
    }

    std::sort(result.byCategory.begin(), result.byCategory.end(),
              [](CategorySum const& a, CategorySum const& b) { return a.cents > b.cents; });
    return result;
}

}  // namespace ledger
