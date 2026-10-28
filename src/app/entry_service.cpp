#include "app/entry_service.hpp"

#include <optional>
#include <string>

#include "parse/timeParse.hpp"

namespace app
{

std::chrono::sys_seconds nowUtc()
{
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

std::chrono::sys_seconds utcToLocalWall(std::chrono::sys_seconds utc)
{
    const std::chrono::zoned_time zt{ std::chrono::current_zone(), utc };
    return std::chrono::sys_seconds{ zt.get_local_time().time_since_epoch() };
}

std::chrono::sys_seconds localWallToUtc(std::chrono::sys_seconds wall)
{
    const std::chrono::local_time<std::chrono::seconds> local{ wall.time_since_epoch() };
    const std::chrono::local_info info = std::chrono::current_zone()->get_info(local);

    // local_info 三种结果：
    //   unique      -> first.offset
    //   nonexistent -> second.offset（跳变之后的偏移，标准推荐）
    //   ambiguous   -> first.offset（跳变之前的偏移，也就是较早那个时刻）
    const auto offset = info.result == std::chrono::local_info::nonexistent
                            ? info.second.offset
                            : info.first.offset;
    return wall - offset;
}

std::expected<core::Entry, std::string> addFromText(core::Core& note, std::string_view text)
{
    const std::string raw{ text };

    // created_at 先定下来再解析 —— 「上周三」相对的就是它。
    // 顺序反过来（先 add 再 parse），两次 now() 之间可能已经跨了一天。
    const auto createdAt = nowUtc();

    // parseTime 吃的是墙上时钟，所以 base 要换成本地墙上时间。
    const auto parsed = parse::parseTime(raw, utcToLocalWall(createdAt));

    // 解析结果是墙上时钟，落盘前换回 UTC。
    std::optional<core::TimeRange> occurred;
    if (parsed.occurred)
    {
        occurred = core::TimeRange{
            localWallToUtc(parsed.occurred->start),
            parsed.occurred->end
                ? std::optional<std::chrono::sys_seconds>{ localWallToUtc(*parsed.occurred->end) }
                : std::nullopt,
        };
    }

    return note.add(raw, createdAt, occurred);
}

}  // namespace app
