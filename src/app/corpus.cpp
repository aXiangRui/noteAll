#include "app/corpus.hpp"

#include <fstream>
#include <sstream>

#include "app/entry_service.hpp"
#include "app/time_input.hpp"
#include "parse/timeParse.hpp"

namespace app
{

namespace
{

// 一行长这样：
//   明天下午三点跟老张开会 => base=2026-10-30 08:00 | start=2026-10-30 15:00 | end=
constexpr const char* kArrow = " => ";
constexpr const char* kHeader =
    "# 解析语料：规则解析器的回归用例\n"
    "# 格式：原文 => base=<本地时间> | start=<本地时间> | end=<本地时间，可空>\n"
    "# 由 GUI 在「采纳 AI 时间建议」时自动追加，也可以手写。\n";

std::string trimCopy(std::string_view text)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };

    std::size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) { ++begin; }
    std::size_t end = text.size();
    while (end > begin && isSpace(text[end - 1])) { --end; }
    return std::string{ text.substr(begin, end - begin) };
}

std::string localText(std::chrono::sys_seconds utc, const std::chrono::time_zone& tz)
{
    return formatLocalDateTime(utc, tz);
}

}  // namespace

bool appendParseCase(std::filesystem::path const& file, ParseCase const& parseCase,
                     const std::chrono::time_zone& tz)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    const bool needsHeader = !std::filesystem::exists(file, ec) ||
                             std::filesystem::file_size(file, ec) == 0;

    std::ofstream out(file, std::ios::app | std::ios::binary);
    if (!out) { return false; }

    if (needsHeader) { out << kHeader; }

    out << parseCase.text << kArrow << "base=" << localText(parseCase.base, tz) << " | start="
        << (parseCase.start ? localText(*parseCase.start, tz) : std::string{}) << " | end="
        << (parseCase.end ? localText(*parseCase.end, tz) : std::string{}) << "\n";

    return static_cast<bool>(out);
}

std::expected<std::vector<ParseCase>, std::string> loadParseCases(
    const std::filesystem::path& file, const std::chrono::time_zone& tz)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) { return std::unexpected("打不开语料文件: " + file.string()); }

    std::vector<ParseCase> cases;
    std::string            line;

    while (std::getline(in, line))
    {
        const std::string trimmed = trimCopy(line);
        if (trimmed.empty() || trimmed.front() == '#') { continue; }

        const auto arrow = trimmed.find(kArrow);
        if (arrow == std::string::npos) { continue; }

        ParseCase parseCase;
        parseCase.text = trimCopy(trimmed.substr(0, arrow));

        // 后半段：base=... | start=... | end=...
        std::string        rest = trimmed.substr(arrow + std::string{ kArrow }.size());
        std::istringstream fields(rest);
        std::string        field;

        while (std::getline(fields, field, '|'))
        {
            const std::string item = trimCopy(field);
            const auto        eq   = item.find('=');
            if (eq == std::string::npos) { continue; }

            const std::string key   = trimCopy(item.substr(0, eq));
            const std::string value = trimCopy(item.substr(eq + 1));

            if (key == "base")
            {
                // base 必须能解析，否则整条用例跳过（不猜）
                const auto baseUtc = parseLocalDateTime(value, parseCase.base, tz);
                if (!baseUtc) { return std::unexpected("用例 base 无法解析: " + value); }
                parseCase.base = *baseUtc;
            }
            else if (key == "start" && !value.empty())
            {
                parseCase.start = parseLocalDateTime(value, parseCase.base, tz);
            }
            else if (key == "end" && !value.empty())
            {
                parseCase.end = parseLocalDateTime(value, parseCase.base, tz);
            }
        }

        if (!parseCase.text.empty()) { cases.push_back(std::move(parseCase)); }
    }

    return cases;
}

std::vector<CaseOutcome> runParseCases(const std::vector<ParseCase>&  cases,
                                       const std::chrono::time_zone& tz)
{
    std::vector<CaseOutcome> outcomes;
    outcomes.reserve(cases.size());

    for (const auto& parseCase : cases)
    {
        CaseOutcome outcome;
        outcome.parseCase = parseCase;

        // 用规则解析器（和 add 完全同一条路径）
        const auto parsed = parse::parseTime(parseCase.text, utcToLocalWall(parseCase.base));

        if (!parsed.occurred)
        {
            outcome.startOk = !parseCase.start.has_value();
        }
        else
        {
            const auto gotStart = localWallToUtc(parsed.occurred->start);
            outcome.gotStart    = localText(gotStart, tz);
            outcome.startOk     = parseCase.start && *parseCase.start == gotStart;

            const std::optional<std::chrono::sys_seconds> gotEnd =
                parsed.occurred->end ? std::optional<std::chrono::sys_seconds>{
                                           localWallToUtc(*parsed.occurred->end) }
                                     : std::nullopt;

            outcome.gotEnd = gotEnd ? localText(*gotEnd, tz) : std::string{};
            outcome.endOk  = (parseCase.end.has_value() == gotEnd.has_value()) &&
                            (!parseCase.end || *parseCase.end == *gotEnd);
        }

        outcomes.push_back(std::move(outcome));
    }

    return outcomes;
}

}  // namespace app
