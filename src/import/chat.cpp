#include "import/chat.hpp"

#include <cstdio>
#include <optional>
#include <string>

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

// 本地墙上时间 → UTC。和 import::bill / app 用的是同一套换算。
std::chrono::sys_seconds localToUtc(std::chrono::sys_seconds wall)
{
    const auto* tz = std::chrono::current_zone();
    const std::chrono::local_time<std::chrono::seconds> local{ wall.time_since_epoch() };
    const std::chrono::local_info                       info = tz->get_info(local);

    const auto offset = info.result == std::chrono::local_info::nonexistent ? info.second.offset
                                                                           : info.first.offset;
    return wall - offset;
}

struct Header
{
    std::chrono::sys_seconds occurred{};
    std::string              sender;
    std::size_t              consumed = 0;   // 这一行用掉了多少个字符
};

/**
 * 认消息头：`2026-10-30 12:10:33 张三` / `2026-10-30 12:10 张三`
 *
 * 为什么要返回 consumed：QQ 的时间戳后面紧跟发送者，中间可能是**全角空格**，
 * 按字节跳会错位，所以按字符位置切。
 */
std::optional<Header> parseHeader(std::string_view line)
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;

    // 用 %n 让 scanf 告诉我们时间部分占了多少字符 —— 否则切出来的发送者会带上
    // "12:10:33 " 这段（我第一版就是这么错的）。
    int consumed = 0;

    int fields = std::sscanf(std::string{ line }.c_str(), "%d-%d-%d %d:%d:%d%n", &year, &month,
                             &day, &hour, &minute, &second, &consumed);

    if (fields < 5)
    {
        // 没有秒的写法：2026-10-30 12:10 张三
        consumed = 0;
        fields   = std::sscanf(std::string{ line }.c_str(), "%d-%d-%d %d:%d%n", &year, &month, &day,
                               &hour, &minute, &consumed);
        second   = 0;
        if (fields < 5) { return std::nullopt; }
    }

    if (month < 1 || month > 12 || day < 1 || day > 31) { return std::nullopt; }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60)
    {
        return std::nullopt;
    }

    const std::chrono::year_month_day date{
        std::chrono::year{ year }, std::chrono::month{ static_cast<unsigned>(month) },
        std::chrono::day{ static_cast<unsigned>(day) }
    };
    if (!date.ok()) { return std::nullopt; }

    if (consumed <= 0 || static_cast<std::size_t>(consumed) >= line.size())
    {
        return std::nullopt;
    }

    // 跳掉时间后面的空格（半角 + 全角）
    std::size_t senderBegin = static_cast<std::size_t>(consumed);
    while (senderBegin < line.size() &&
           (line[senderBegin] == ' ' || line[senderBegin] == '\t' ||
            line.compare(senderBegin, 3, "\xE3\x80\x80") == 0))   // 全角空格 U+3000
    {
        senderBegin += (line.compare(senderBegin, 3, "\xE3\x80\x80") == 0) ? 3 : 1;
    }

    if (senderBegin >= line.size()) { return std::nullopt; }

    const auto wall = std::chrono::sys_days{ date } + std::chrono::hours{ hour } +
                      std::chrono::minutes{ minute } + std::chrono::seconds{ second };

    Header header;
    header.occurred = localToUtc(std::chrono::sys_seconds{ wall.time_since_epoch() });
    header.sender   = trim(line.substr(senderBegin));
    header.consumed = senderBegin;
    return header;
}

// 去掉发送者后面的 QQ 号：`张三(12345678)` → `张三`
std::string stripQqNumber(std::string sender)
{
    const auto open = sender.rfind('(');
    if (open == std::string::npos || open == 0) { return sender; }

    const auto close = sender.rfind(')');
    if (close == std::string::npos || close < open) { return sender; }

    // 括号里必须全是数字，才当 QQ 号剥掉（别把「小明(组长)」搞坏）
    for (std::size_t i = open + 1; i < close; ++i)
    {
        if (sender[i] < '0' || sender[i] > '9') { return sender; }
    }

    return trim(std::string_view{ sender }.substr(0, open));
}

bool isBoilerplateLine(std::string_view line)
{
    const std::string text = trim(line);
    if (text.empty()) { return true; }
    if (text.find("消息记录") != std::string::npos) { return true; }
    if (text.rfind("消息分组:", 0) == 0 || text.rfind("消息对象:", 0) == 0) { return true; }
    if (text.rfind("====", 0) == 0) { return true; }
    return false;
}

}  // namespace

ChatParseResult parseQqChat(std::string_view utf8Text)
{
    ChatParseResult result;

    std::vector<std::string> lines;
    std::size_t              begin = 0;
    while (begin <= utf8Text.size())
    {
        const auto end = utf8Text.find('\n', begin);
        if (end == std::string_view::npos)
        {
            lines.emplace_back(utf8Text.substr(begin));
            break;
        }
        lines.emplace_back(utf8Text.substr(begin, end - begin));
        begin = end + 1;
    }

    ChatMessage current;
    bool        hasCurrent = false;

    const auto flush = [&](int lineNumber) {
        if (!hasCurrent) { return; }
        current.text = trim(current.text);
        if (!current.text.empty())
        {
            current.lineNumber = lineNumber;
            result.messages.push_back(current);
        }
        hasCurrent = false;
        current     = ChatMessage{};
    };

    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const int         lineNumber = static_cast<int>(index) + 1;
        const std::string line       = trim(std::string_view{ lines[index] }.substr(
            0, lines[index].find_last_not_of('\r') + 1));

        // 消息对象（保留，界面上显示"来自谁"）
        if (line.rfind("消息对象:", 0) == 0)
        {
            result.peer = trim(std::string_view{ line }.substr(std::string{ "消息对象:" }.size()));
            continue;
        }

        if (isBoilerplateLine(line)) { continue; }

        if (const auto header = parseHeader(line))
        {
            flush(lineNumber);

            current.occurred = header->occurred;
            current.sender   = stripQqNumber(header->sender);
            hasCurrent       = true;
            continue;
        }

        if (!hasCurrent)
        {
            // 第一条消息之前还有零散文字，跳过但留个警告（便于发现格式变了）
            if (result.warnings.size() < 5)
            {
                result.warnings.push_back("第 " + std::to_string(lineNumber) + " 行无法识别，已跳过: " +
                                          (line.size() > 40 ? line.substr(0, 40) + "…" : line));
            }
            continue;
        }

        // 续行（多行消息）
        if (!current.text.empty()) { current.text += '\n'; }
        current.text += line;
    }

    flush(static_cast<int>(lines.size()));

    return result;
}

}  // namespace import
