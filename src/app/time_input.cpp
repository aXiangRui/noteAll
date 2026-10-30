#include "app/time_input.hpp"

#include <cstdio>

namespace app
{

namespace
{

std::string trimCopy(std::string_view text)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };

    std::size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) { ++begin; }
    std::size_t end = text.size();
    while (end > begin && isSpace(text[end - 1])) { --end; }
    return std::string{ text.substr(begin, end - begin) };
}

// "本地墙上时间" → UTC。和 app::localWallToUtc 同一个算法（夏令时边界也一样），
// 只是时区由参数给，方便测试。
std::chrono::sys_seconds localWallToUtcIn(std::chrono::sys_seconds       wall,
                                          const std::chrono::time_zone& tz)
{
    const std::chrono::local_time<std::chrono::seconds> local{ wall.time_since_epoch() };
    const std::chrono::local_info info = tz.get_info(local);

    const auto offset = info.result == std::chrono::local_info::nonexistent
                            ? info.second.offset
                            : info.first.offset;
    return wall - offset;
}

// 组装成 UTC。日期/时刻非法（2月30日、25:00）返回空。
std::optional<std::chrono::sys_seconds> makeUtc(int year, int month, int day, int hour, int minute,
                                                const std::chrono::time_zone& tz)
{
    if (month < 1 || month > 12 || day < 1 || day > 31) { return std::nullopt; }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) { return std::nullopt; }

    const std::chrono::year_month_day date{
        std::chrono::year{ year }, std::chrono::month{ static_cast<unsigned>(month) },
        std::chrono::day{ static_cast<unsigned>(day) }
    };
    if (!date.ok()) { return std::nullopt; }   // 2月30日 这种

    const auto localWall = std::chrono::sys_days{ date } + std::chrono::hours{ hour } +
                           std::chrono::minutes{ minute };

    return localWallToUtcIn(std::chrono::sys_seconds{ localWall.time_since_epoch() }, tz);
}

}  // namespace

std::string formatLocalDateTime(std::chrono::sys_seconds utc, const std::chrono::time_zone& tz)
{
    const std::chrono::zoned_time zt{ &tz, utc };
    const auto                   local = zt.get_local_time();
    const std::chrono::year_month_day day{ std::chrono::floor<std::chrono::days>(local) };

    const auto sinceMidnight = std::chrono::floor<std::chrono::minutes>(local) -
                               std::chrono::floor<std::chrono::days>(local);
    const auto hms = std::chrono::hh_mm_ss{ sinceMidnight };

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u %02d:%02d", static_cast<int>(day.year()),
                  static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()),
                  static_cast<int>(hms.hours().count()),
                  static_cast<int>(hms.minutes().count()));
    return buffer;
}

std::optional<std::chrono::sys_seconds> parseLocalDateTime(std::string_view               text,
                                                           std::chrono::sys_seconds      fallback,
                                                           const std::chrono::time_zone& tz)
{
    const std::string trimmed = trimCopy(text);
    if (trimmed.empty()) { return std::nullopt; }

    // fallback 的本地日期 / 年份，供"只写时刻"或"缺年份"时补全
    const std::chrono::zoned_time      fallbackZoned{ &tz, fallback };
    const std::chrono::year_month_day  fallbackDay{
        std::chrono::floor<std::chrono::days>(fallbackZoned.get_local_time())
    };

    int y = 0;
    int mo = 0;
    int d = 0;
    int h = 0;
    int mi = 0;

    // 1) 2026-10-30 08:30
    if (std::sscanf(trimmed.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5)
    {
        return makeUtc(y, mo, d, h, mi, tz);
    }

    // 2) 2026-10-30（只有日期）
    if (std::sscanf(trimmed.c_str(), "%d-%d-%d", &y, &mo, &d) == 3)
    {
        return makeUtc(y, mo, d, 0, 0, tz);
    }

    // 3) 10-30 08:30（缺年份）
    if (std::sscanf(trimmed.c_str(), "%d-%d %d:%d", &mo, &d, &h, &mi) == 4)
    {
        return makeUtc(static_cast<int>(fallbackDay.year()), mo, d, h, mi, tz);
    }

    // 4) 8:30（只有时刻，日期取 fallback 那天）
    if (std::sscanf(trimmed.c_str(), "%d:%d", &h, &mi) == 2)
    {
        return makeUtc(static_cast<int>(fallbackDay.year()),
                       static_cast<int>(static_cast<unsigned>(fallbackDay.month())),
                       static_cast<int>(static_cast<unsigned>(fallbackDay.day())), h, mi, tz);
    }

    return std::nullopt;
}

std::optional<std::chrono::sys_seconds> parseLocalDateTime(std::string_view          text,
                                                           std::chrono::sys_seconds fallback)
{
    return parseLocalDateTime(text, fallback, *std::chrono::current_zone());
}

}  // namespace app
