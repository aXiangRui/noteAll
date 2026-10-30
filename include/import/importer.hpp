#pragma once

#include <expected>
#include <string>
#include <vector>

#include "import/bill.hpp"
#include "ledger/book.hpp"

// 把解析结果落进账本，并做去重。
//
// 去重规则：按账单里的「交易号 / 交易单号」。
// 不能按"时间 + 金额"猜 —— 同一天同一金额的两笔（两杯一样的咖啡）是完全正常的。
namespace import
{

struct ImportOutcome
{
    std::size_t              added      = 0;
    std::size_t              duplicates = 0;
    std::size_t              skipped    = 0;

    // 人话版说明（前若干条），给界面显示
    std::vector<std::string> notes;

    /** 一句话总结："新增 42 笔，跳过重复 3 笔，无法解析 1 行" */
    std::string summary() const;
};

/**
 * @param commit false = 只统计（预览），不写盘
 */
std::expected<ImportOutcome, std::string> importBill(ledger::Book&              book,
                                                     const BillParseResult&     parsed,
                                                     bool                       commit);

}  // namespace import
