#include "import/importer.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <string>

namespace import
{

std::string ImportOutcome::summary() const
{
    std::string text = "新增 " + std::to_string(added) + " 笔";
    if (duplicates > 0) { text += "，跳过重复 " + std::to_string(duplicates) + " 笔"; }
    if (skipped > 0) { text += "，无法解析 " + std::to_string(skipped) + " 行"; }
    return text;
}

std::expected<ImportOutcome, std::string> importBill(ledger::Book&          book,
                                                     const BillParseResult& parsed,
                                                     bool                   commit)
{
    ImportOutcome outcome;

    // 已有交易号（含软删除的：用户删掉的账不该被重新导回来）
    std::set<std::string> known;
    for (const auto& existing : book.listAll())
    {
        if (!existing.external_id.empty()) { known.insert(existing.external_id); }
    }

    outcome.skipped = parsed.skipped.size();
    for (const auto& note : parsed.skipped)
    {
        if (outcome.notes.size() < 5) { outcome.notes.push_back(note); }
    }

    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

    for (const auto& row : parsed.rows)
    {
        // 没有交易号的，退回"时间+金额+备注"当指纹，避免同一文件导两次翻倍
        std::string key = row.externalId;
        if (key.empty())
        {
            key = "fingerprint:" + std::to_string(row.occurred.time_since_epoch().count()) + ":" +
                  std::to_string(row.amountCents) + ":" + row.note;
        }

        if (known.contains(key))
        {
            ++outcome.duplicates;
            continue;
        }

        if (commit)
        {
            auto added = book.add(row.amountCents, row.direction, row.occurred, row.category, row.note,
                                  now, row.externalId);
            if (!added) { return std::unexpected("写入账本失败: " + added.error()); }
        }

        known.insert(key);
        ++outcome.added;
    }

    return outcome;
}

}  // namespace import
