#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ledger/ledger.hpp"

// 账单适配：把微信/支付宝导出的 CSV 变成 ledger::Transaction 的原料。
//
// 关键点（比 CSV 切分难得多）：
//   * 账单前面有 4–6 行垃圾（账号、日期范围、分隔虚线），**不能假设第一行是表头**；
//   * 两家的列名完全不同（交易时间/交易创建时间、收/支、金额(元)…）；
//   * 列名变了必须明确报"不认识这个格式"，而不是静默读出 0 笔。
namespace import
{

enum class BillSource
{
    Unknown,
    WeChat,
    Alipay,
};

std::string toString(BillSource source);

struct BillRow
{
    std::chrono::sys_seconds occurred{};
    std::int64_t             amountCents = 0;
    ledger::Direction        direction = ledger::Direction::Expense;
    std::string              category;
    std::string              note;
    std::string              externalId;   // 交易单号 / 交易号（去重靠它）
    int                      lineNumber = 0;
};

struct BillParseResult
{
    BillSource                source = BillSource::Unknown;
    std::string               headerLine;      // 识别到的表头（原文，给人看）
    int                       headerLineNumber = 0;
    std::vector<BillRow>      rows;
    std::vector<std::string>  skipped;         // 「第 12 行：金额缺失」这种
};

/**
 * 解析账单 CSV（已是 UTF-8）。
 *
 * 认不出格式时 source = Unknown、rows 为空，并在 skipped 里说明原因 ——
 * 上层要能看到"为什么 0 笔"，而不是只拿到一个空列表。
 */
BillParseResult parseBill(std::string_view utf8Csv);

}  // namespace import
