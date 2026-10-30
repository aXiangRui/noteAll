#include "import/bill.hpp"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "import/csv.hpp"
#include "ledger/money.hpp"

namespace import
{

namespace
{

std::string trim(std::string_view text)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };

    std::size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) { ++begin; }
    std::size_t end = text.size();
    while (end > begin && isSpace(text[end - 1])) { --end; }
    return std::string{ text.substr(begin, end - begin) };
}

bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

// 本地墙上时间 → UTC（时区和 UI 一致，用系统时区）
std::chrono::sys_seconds localToUtc(std::chrono::sys_seconds wall)
{
    const auto* tz = std::chrono::current_zone();
    const std::chrono::local_time<std::chrono::seconds> local{ wall.time_since_epoch() };
    const std::chrono::local_info info = tz->get_info(local);

    const auto offset = info.result == std::chrono::local_info::nonexistent ? info.second.offset
                                                                           : info.first.offset;
    return wall - offset;
}

// 认 "2026-10-30 12:00:00" / "2026-10-30 12:00" / "2026/10/30 12:00:00"
std::optional<std::chrono::sys_seconds> parseBillTime(std::string_view text)
{
    std::string value = trim(text);
    for (char& c : value)
    {
        if (c == '/') { c = '-'; }
    }

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;

    if (std::sscanf(value.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute,
                    &second) >= 5)
    {
        if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59) { return std::nullopt; }

        const std::chrono::year_month_day date{
            std::chrono::year{ year }, std::chrono::month{ static_cast<unsigned>(month) },
            std::chrono::day{ static_cast<unsigned>(day) }
        };
        if (!date.ok()) { return std::nullopt; }

        const auto wall = std::chrono::sys_days{ date } + std::chrono::hours{ hour } +
                          std::chrono::minutes{ minute } + std::chrono::seconds{ second };
        return localToUtc(std::chrono::sys_seconds{ wall.time_since_epoch() });
    }

    return std::nullopt;
}

// 在表头里找列：给定候选名，返回第一个"表头里包含它"的下标
int findColumn(const std::vector<std::string>& header, std::initializer_list<std::string_view> names)
{
    for (const auto name : names)
    {
        for (std::size_t i = 0; i < header.size(); ++i)
        {
            if (contains(header[i], name)) { return static_cast<int>(i); }
        }
    }
    return -1;
}

std::string cell(const std::vector<std::string>& row, int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= row.size()) { return {}; }
    return trim(row[static_cast<std::size_t>(index)]);
}

// 金额文本 → 分。处理前导负号（支付宝早期用负数表示支出）。
std::optional<std::int64_t> parseSignedCents(std::string_view text, bool& negative)
{
    std::string value = trim(text);
    negative          = false;

    if (!value.empty() && (value.front() == '-' || value.front() == '\xEF\xBC\x8D'))   // - 或 －
    {
        negative = true;
        value.erase(0, value.size() >= 3 && value.front() == static_cast<char>(0xEF) ? 3 : 1);
    }
    else if (!value.empty() && value.front() == '+')
    {
        value.erase(0, 1);
    }

    return ledger::parseCents(value);
}

struct ColumnMap
{
    int time         = -1;
    int amount       = -1;
    int direction    = -1;
    int externalId   = -1;
    int counterparty = -1;
    int goods        = -1;
    int status       = -1;
    int type         = -1;
};

bool looksLikeHeader(const std::vector<std::string>& row)
{
    if (row.size() < 3) { return false; }

    bool hasTime = false;
    bool hasMoney = false;
    for (const auto& cellText : row)
    {
        if (contains(cellText, "交易时间") || contains(cellText, "交易创建时间") ||
            contains(cellText, "付款时间"))
        {
            hasTime = true;
        }
        if (contains(cellText, "金额")) { hasMoney = true; }
    }
    return hasTime && hasMoney;
}

}  // namespace

std::string toString(BillSource source)
{
    switch (source)
    {
        case BillSource::WeChat: return "微信";
        case BillSource::Alipay: return "支付宝";
        case BillSource::Unknown: break;
    }
    return "未知";
}

BillParseResult parseBill(std::string_view utf8Csv)
{
    BillParseResult result;

    const CsvTable table = parseCsv(utf8Csv);
    for (const auto& warning : table.warnings) { result.skipped.push_back(warning); }

    if (table.rows.empty())
    {
        result.skipped.push_back("文件是空的，或者一行数据都没有");
        return result;
    }

    // ---- 1) 找表头：账单前面有一堆垃圾行，不能假设第一行是表头 ----
    std::size_t headerIndex = 0;
    bool        found       = false;
    for (std::size_t i = 0; i < table.rows.size(); ++i)
    {
        if (looksLikeHeader(table.rows[i]))
        {
            headerIndex = i;
            found       = true;
            break;
        }
    }

    if (!found)
    {
        result.skipped.push_back(
            "没找到表头行（需要同时含「交易时间/交易创建时间」和「金额」两列）—— "
            "这个账单格式我不认识，可能导出方式变了");
        return result;
    }

    const auto& header = table.rows[headerIndex];
    result.headerLineNumber = static_cast<int>(headerIndex) + 1;

    std::string headerText;
    for (std::size_t i = 0; i < header.size(); ++i)
    {
        if (i > 0) { headerText += ","; }
        headerText += header[i];
    }
    result.headerLine = headerText;

    // ---- 2) 认来源 + 建列映射 ----
    ColumnMap map;
    const bool hasWeChatId = findColumn(header, { "交易单号" }) >= 0;

    if (hasWeChatId)
    {
        result.source = BillSource::WeChat;
        map.time         = findColumn(header, { "交易时间" });
        map.amount       = findColumn(header, { "金额(元)", "金额" });
        map.direction    = findColumn(header, { "收/支" });
        map.externalId   = findColumn(header, { "交易单号" });
        map.counterparty = findColumn(header, { "交易对方" });
        map.goods        = findColumn(header, { "商品" });
        map.status       = findColumn(header, { "当前状态" });
        map.type         = findColumn(header, { "交易类型" });
    }
    else
    {
        result.source = BillSource::Alipay;
        map.time         = findColumn(header, { "交易创建时间", "付款时间" });
        map.amount       = findColumn(header, { "金额(元)", "金额" });
        map.direction    = findColumn(header, { "收/支" });
        map.externalId   = findColumn(header, { "交易号" });
        map.counterparty = findColumn(header, { "交易对方" });
        map.goods        = findColumn(header, { "商品名称", "商品" });
        map.status       = findColumn(header, { "交易状态" });
        map.type         = findColumn(header, { "类型" });
    }

    if (map.time < 0 || map.amount < 0)
    {
        result.skipped.push_back("表头缺「时间」或「金额」列，无法导入");
        return result;
    }

    // ---- 3) 逐行转成记账数据 ----
    for (std::size_t i = headerIndex + 1; i < table.rows.size(); ++i)
    {
        const auto& row        = table.rows[i];
        const int   lineNumber = static_cast<int>(i) + 1;

        const std::string timeText   = cell(row, map.time);
        const std::string amountText = cell(row, map.amount);

        // 账单尾部的说明行（"共 N 笔"、虚线）没有时间，直接跳过，不当错误
        if (timeText.empty() && amountText.empty()) { continue; }

        const auto occurred = parseBillTime(timeText);
        if (!occurred)
        {
            result.skipped.push_back("第 " + std::to_string(lineNumber) + " 行：时间看不懂「" +
                                     timeText + "」");
            continue;
        }

        bool              negative = false;
        const auto        cents    = parseSignedCents(amountText, negative);
        if (!cents)
        {
            result.skipped.push_back("第 " + std::to_string(lineNumber) + " 行：金额看不懂「" +
                                     amountText + "」");
            continue;
        }

        // 方向：优先看「收/支」列；没有就看金额符号
        const std::string directionText = cell(row, map.direction);
        ledger::Direction direction     = ledger::Direction::Expense;

        if (contains(directionText, "收入"))
        {
            direction = ledger::Direction::Income;
        }
        else if (contains(directionText, "支出"))
        {
            direction = ledger::Direction::Expense;
        }
        else if (directionText == "/" || contains(directionText, "不计"))
        {
            // 明确表示「不计收支」（零钱提现、还款、内部转账）：记账里没有意义，
            // 跳过但要说清楚 —— 静默少几笔比多几笔更难查。
            result.skipped.push_back("第 " + std::to_string(lineNumber) + " 行：不计收支（" +
                                     directionText + "）");
            continue;
        }
        else
        {
            // 没有「收/支」列的老账单：用金额符号判断
            direction = negative ? ledger::Direction::Expense : ledger::Direction::Income;
        }

        // 备注：交易对方 + 商品
        const std::string counterparty = cell(row, map.counterparty);
        const std::string goods        = cell(row, map.goods);

        std::string note = counterparty;
        if (!goods.empty() && goods != counterparty)
        {
            note += note.empty() ? goods : " " + goods;
        }
        if (note.empty()) { note = cell(row, map.type); }

        BillRow billRow;
        billRow.occurred    = *occurred;
        billRow.amountCents = *cents;
        billRow.direction   = direction;
        billRow.category    = ledger::guessCategory(note, direction);
        billRow.note        = note;
        billRow.externalId  = cell(row, map.externalId);
        billRow.lineNumber  = lineNumber;

        result.rows.push_back(std::move(billRow));
    }

    return result;
}

}  // namespace import
