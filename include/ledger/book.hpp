#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ledger/ledger.hpp"

// 账本门面：和 core::Core 同一个套路（锁 → 重读 → 改 → 落盘 → 失败回滚）。
namespace ledger
{

struct CategorySum
{
    std::string  category;
    std::int64_t cents = 0;
};

struct Summary
{
    std::int64_t              expenseCents = 0;
    std::int64_t              incomeCents  = 0;
    std::vector<CategorySum>  byCategory;        // 支出分类，金额降序
    std::vector<CategorySum>  incomeByCategory;  // 收入分类，金额降序
    std::size_t               count = 0;
};

/** 某个月的合计（给趋势图用）。 */
struct MonthTotal
{
    std::chrono::year_month month{};
    std::int64_t            expenseCents = 0;
    std::int64_t            incomeCents  = 0;
};

/**
 * 最近 count 个月的合计（含 lastMonth 往前数）。
 *
 * 按月聚合必须走**本地时区** —— 和 summary 一致，否则跨时区会差一个月。
 */
std::vector<MonthTotal> monthlyTotals(std::vector<Transaction> const& visible,
                                      std::chrono::year_month          lastMonth,
                                      int                              count);

/** 某个月的每一天（1..当月天数）合计，给"每日"图表用。 */
struct DayTotal
{
    unsigned     day          = 0;
    std::int64_t expenseCents = 0;
    std::int64_t incomeCents  = 0;
    std::size_t  count        = 0;
};

std::vector<DayTotal> dayTotals(std::vector<Transaction> const& visible,
                                std::chrono::year_month          month);

class Book
{
public:
    explicit Book(std::filesystem::path file);

    std::expected<void, std::string> load();

    /** 可见流水（不含已删除），按发生时间倒序。 */
    const std::vector<Transaction>& list() const { return visible_; }

    /** 含墓碑的全量。 */
    const std::vector<Transaction>& listAll() const { return all_; }

    const std::vector<std::string>& warnings() const { return warnings_; }

    std::expected<Transaction, std::string> add(std::int64_t              amountCents,
                                                Direction                 direction,
                                                std::chrono::sys_seconds  occurred,
                                                std::string               category,
                                                std::string               note,
                                                std::chrono::sys_seconds  createdAt,
                                                std::string               externalId = {});

    std::expected<void, std::string> remove(
        std::string_view                        id,
        std::optional<std::chrono::sys_seconds> deletedAt = std::nullopt);

    /** 某个月的汇总（按**本地**月份过滤）。 */
    Summary summary(std::chrono::year_month month) const;

    /**
     * 把分类为「其他」的记录重新猜一遍（补关键词之后用来刷历史数据）。
     *
     * 只动「其他」——已经分好类的绝不覆盖（可能是用户手动改过的）。
     * 备注以 " /" 结尾的是微信转账（商品列填了 "/"），顺便把尾巴去掉并归到「转账」。
     *
     * @param commit false = 只统计不落盘
     */
    struct RecategorizeResult
    {
        std::size_t scanned   = 0;   // 扫到的「其他」条数
        std::size_t changed   = 0;   // 成功归类
        std::size_t remaining = 0;   // 仍然归不出来的

        // 归入明细：category → 条数（预览时也能看清"会变成什么样"）
        std::vector<std::pair<std::string, std::size_t>> moved;
    };

    std::expected<RecategorizeResult, std::string> recategorizeOthers(bool commit);

private:
    std::filesystem::path          file_;
    std::vector<Transaction>       all_;
    std::vector<Transaction>       visible_;
    std::vector<std::string>       warnings_;
};

}  // namespace ledger
