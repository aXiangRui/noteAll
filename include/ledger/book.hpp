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
    std::vector<CategorySum>  byCategory;   // 支出，按金额从大到小
    std::size_t               count = 0;
};

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

private:
    std::filesystem::path          file_;
    std::vector<Transaction>       all_;
    std::vector<Transaction>       visible_;
    std::vector<std::string>       warnings_;
};

}  // namespace ledger
