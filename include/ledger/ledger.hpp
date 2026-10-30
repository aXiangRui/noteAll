#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// 记账的领域类型（流水）。
//
// 和 core::Entry 的区别：
//   * 金额是**整数分**，不是文本；
//   * 没有"解析时间"的模糊地带 —— 发生时间必填（默认现在）；
//   * 分类是必填的短字符串（从预置表来），不是自由标签。
namespace ledger
{

enum class Direction
{
    Expense,   // 支出
    Income,    // 收入
};

std::string  toString(Direction direction);
Direction    directionFromString(std::string_view text);

struct Transaction
{
    std::string              id;
    std::int64_t             amountCents = 0;                 // 永远正数，方向由 direction 决定
    Direction                direction = Direction::Expense;
    std::chrono::sys_seconds occurred{};                      // 什么时候花的（UTC）
    std::string              category;                        // 餐饮 / 交通 / …
    std::string              note;                            // 备注（原文去掉金额）
    std::chrono::sys_seconds created_at{};                    // 记录时刻（UTC）
    std::optional<std::chrono::sys_seconds> deleted_at;       // 软删除，和 note 一致
};

/** 预置分类（支出）。收入另有一小张表。 */
std::vector<std::string> expenseCategories();
std::vector<std::string> incomeCategories();

/**
 * 从备注里猜方向：出现"工资/奖金/报销/退款/利息"这类词算收入，否则支出。
 * 猜错也不要紧 —— CLI 有 --income 显式指定，以后模型也能给建议。
 */
Direction guessDirection(std::string_view note);

/** 从备注里猜分类（关键词表，确定性、可测）。猜不出返回「其他」。 */
std::string guessCategory(std::string_view note, Direction direction);

}  // namespace ledger
