#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// 金额：**永远用整数分**（int64），不用 double。
//
// 为什么这不是洁癖：0.1 + 0.2 != 0.3。账本里浮点误差会在月度汇总上
// 累积成"差几毛钱"，而记账软件最不能错的就是数。
namespace ledger
{

/** 单个金额文本 → 分。认 "32" / "32.5" / "32.50" / "¥32" / "32元" / "1,024.00"。 */
std::optional<std::int64_t> parseCents(std::string_view text);

/** 分 → "1024.00"（不带货币符号，符号由显示层决定）。 */
std::string formatCents(std::int64_t cents);

/**
 * 从一句话里拆出「金额 + 备注」。
 *
 * 规则（故意做得简单可预测）：金额取**句末**那个数字，前后允许 ¥/￥/元/块/圆。
 *   午饭 32      → 3200，备注「午饭」
 *   打车12块      → 1200，备注「打车」
 *   早饭 ¥12.5    → 1250，备注「早饭」
 *   10月30日 午饭 32 → 3200，备注「10月30日 午饭」
 * 取句末而不是"第一个数字"，是为了避开日期里的数字（10月30日 里的 30）。
 */
struct AmountNote
{
    std::int64_t amountCents = 0;
    std::string  note;
};

std::optional<AmountNote> parseAmountNote(std::string_view text);

}  // namespace ledger
