#include "ledger/money.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace ledger
{

namespace
{

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

std::string trimCopy(std::string_view text)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };

    std::size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) { ++begin; }
    std::size_t end = text.size();
    while (end > begin && isSpace(text[end - 1])) { --end; }
    return std::string{ text.substr(begin, end - begin) };
}

// 把 "1,024.5" 这样的数字串转成分。非法返回空。
std::optional<std::int64_t> toCents(std::string_view number)
{
    std::string digits;          // 去掉逗号的纯数字
    std::string fraction;        // 小数部分
    bool        seenDot = false;

    for (const char c : number)
    {
        if (c == ',') { continue; }          // 千分位
        if (c == '.')
        {
            if (seenDot) { return std::nullopt; }
            seenDot = true;
            continue;
        }
        if (!isDigit(c)) { return std::nullopt; }

        (seenDot ? fraction : digits).push_back(c);
    }

    if (digits.empty() && fraction.empty()) { return std::nullopt; }
    if (fraction.size() > 2) { return std::nullopt; }   // 超过分的精度：不猜，直接拒

    std::int64_t cents = 0;
    for (const char c : digits)
    {
        if (cents > 100'000'000'000LL) { return std::nullopt; }   // 防溢出：10 亿上限
        cents = cents * 10 + (c - '0');
    }
    cents *= 100;

    if (fraction.size() >= 1) { cents += (fraction[0] - '0') * 10; }
    if (fraction.size() >= 2) { cents += (fraction[1] - '0'); }

    if (cents <= 0) { return std::nullopt; }   // 0 元没有记账意义
    return cents;
}

// 从右往左找一段"像金额"的子串，遇到非数字/逗号/点就停。
std::optional<std::pair<std::string_view, std::size_t>> trailingNumber(std::string_view text)
{
    std::size_t end = text.size();
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) { --end; }

    std::size_t begin = end;
    while (begin > 0)
    {
        const char c = text[begin - 1];
        if (isDigit(c) || c == ',' || c == '.') { --begin; continue; }
        break;
    }

    if (begin == end) { return std::nullopt; }
    return std::make_pair(text.substr(begin, end - begin), begin);
}

bool endsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

}  // namespace

std::optional<std::int64_t> parseCents(std::string_view text)
{
    std::string value = trimCopy(text);
    if (value.empty()) { return std::nullopt; }

    // 前后缀：¥ ￥ 元 块 圆
    const auto stripPrefix = [](std::string& s, std::string_view prefix) {
        if (s.rfind(prefix.data(), 0) == 0) { s.erase(0, prefix.size()); }
    };
    stripPrefix(value, "\xC2\xA5");        // ¥ (U+00A5)
    stripPrefix(value, "\xEF\xBF\xA5");    // ￥ (U+FFE5)
    for (const std::string_view suffix : { "\xE5\x85\x83", "\xE5\x9D\x97", "\xE5\x9C\x86" })   // 元/块/圆
    {
        if (endsWith(value, suffix)) { value.erase(value.size() - suffix.size()); }
    }

    return toCents(trimCopy(value));
}

std::string formatCents(std::int64_t cents)
{
    const bool negative = cents < 0;
    const auto absolute = negative ? -cents : cents;

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%s%lld.%02lld", negative ? "-" : "",
                  static_cast<long long>(absolute / 100), static_cast<long long>(absolute % 100));
    return buffer;
}

std::optional<AmountNote> parseAmountNote(std::string_view text)
{
    std::string value = trimCopy(text);
    if (value.empty()) { return std::nullopt; }

    // 1) 去掉句末的货币词（元 / 块 / 圆）
    for (const std::string_view suffix : { "\xE5\x85\x83", "\xE5\x9D\x97", "\xE5\x9C\x86" })
    {
        if (endsWith(value, suffix))
        {
            value.erase(value.size() - suffix.size());
            value = trimCopy(value);
            break;
        }
    }

    // 2) 取句末数字
    const auto number = trailingNumber(value);
    if (!number) { return std::nullopt; }

    const auto cents = toCents(number->first);
    if (!cents) { return std::nullopt; }

    // 3) 前面的部分当备注，去掉首尾的空白和货币符号
    std::string note = trimCopy(std::string_view{ value }.substr(0, number->second));
    for (const std::string_view prefix : { "\xC2\xA5", "\xEF\xBF\xA5" })
    {
        if (endsWith(note, prefix)) { note.erase(note.size() - prefix.size()); }
    }
    note = trimCopy(note);

    return AmountNote{ *cents, note };
}

}  // namespace ledger
