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
                                                  std::chrono::sys_seconds createdAt,
                                                  std::string              externalId)
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
    t.external_id = std::move(externalId);

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

    // 按分类汇总（支出 + 收入各一份），金额降序
    const auto aggregate = [&](Direction direction, std::vector<CategorySum>& out) {
        for (const auto& t : visible_)
        {
            if (t.direction != direction) { continue; }

            const std::chrono::zoned_time      zt{ tz, t.occurred };
            const std::chrono::year_month_day  ymd{
                std::chrono::floor<std::chrono::days>(zt.get_local_time()) };
            const std::chrono::year_month      ym{ ymd.year(), ymd.month() };
            if (ym != month) { continue; }

            auto it = std::find_if(out.begin(), out.end(),
                                   [&](CategorySum const& s) { return s.category == t.category; });
            if (it == out.end()) { out.push_back(CategorySum{ t.category, t.amountCents }); }
            else { it->cents += t.amountCents; }
        }

        std::sort(out.begin(), out.end(),
                  [](CategorySum const& a, CategorySum const& b) { return a.cents > b.cents; });
    };

    aggregate(Direction::Expense, result.byCategory);
    aggregate(Direction::Income, result.incomeByCategory);
    return result;
}

std::vector<MonthTotal> monthlyTotals(std::vector<Transaction> const& visible,
                                      std::chrono::year_month          lastMonth,
                                      int                              count)
{
    std::vector<MonthTotal> totals;
    if (count <= 0) { return totals; }

    const auto* tz = std::chrono::current_zone();

    totals.reserve(static_cast<std::size_t>(count));
    for (int i = count - 1; i >= 0; --i)
    {
        MonthTotal total;
        total.month = lastMonth - std::chrono::months{ i };
        totals.push_back(total);
    }

    // 一次遍历把每一笔归到对应月份（避免 rows × months 次重扫）
    for (const auto& t : visible)
    {
        const std::chrono::zoned_time      zt{ tz, t.occurred };
        const std::chrono::year_month_day  ymd{
            std::chrono::floor<std::chrono::days>(zt.get_local_time()) };
        const std::chrono::year_month      ym{ ymd.year(), ymd.month() };

        auto it = std::find_if(totals.begin(), totals.end(),
                               [&](MonthTotal const& m) { return m.month == ym; });
        if (it == totals.end()) { continue; }

        if (t.direction == Direction::Income) { it->incomeCents += t.amountCents; }
        else { it->expenseCents += t.amountCents; }
    }

    return totals;
}

std::vector<DayTotal> dayTotals(std::vector<Transaction> const& visible,
                                std::chrono::year_month          month)
{
    std::vector<DayTotal> totals;

    // 当月天数：用 year_month_day_last，2 月闰年自动对
    const std::chrono::year_month_day_last lastDay{ month.year(), month.month() / std::chrono::last };
    const unsigned                          days = static_cast<unsigned>(lastDay.day());

    totals.reserve(days);
    for (unsigned day = 1; day <= days; ++day) { totals.push_back(DayTotal{ day, 0, 0, 0 }); }

    // 中间那些天没记账也要占格（0），否则柱状图的 x 轴会错位
    const auto* tz = std::chrono::current_zone();

    for (const auto& t : visible)
    {
        const std::chrono::zoned_time     zt{ tz, t.occurred };
        const std::chrono::year_month_day ymd{
            std::chrono::floor<std::chrono::days>(zt.get_local_time()) };
        if (std::chrono::year_month{ ymd.year(), ymd.month() } != month) { continue; }

        const unsigned day = static_cast<unsigned>(ymd.day());
        if (day < 1 || day > days) { continue; }

        DayTotal& total = totals[day - 1];
        ++total.count;
        if (t.direction == Direction::Income) { total.incomeCents += t.amountCents; }
        else { total.expenseCents += t.amountCents; }
    }

    return totals;
}
}  // namespace ledger
