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

std::optional<core::TimeRange> occurredFromText(std::string_view text,
                                                std::chrono::sys_seconds createdAt)
{
    // parseTime 吃的是墙上时钟，所以 base 要换成本地墙上时间。
    const auto parsed = parse::parseTime(text, utcToLocalWall(createdAt));

    if (!parsed.occurred)
    {
        return std::nullopt;   // 认不出就留空，不猜（记录优先）
    }

    // 解析结果是墙上时钟，落盘前换回 UTC。
    return core::TimeRange{
        localWallToUtc(parsed.occurred->start),
        parsed.occurred->end
            ? std::optional<std::chrono::sys_seconds>{ localWallToUtc(*parsed.occurred->end) }
            : std::nullopt,
    };
}

std::expected<core::Entry, std::string> addFromText(core::Core& note, std::string_view text)
{
    const std::string raw{ text };

    // created_at 先定下来再解析 —— 「上周三」相对的就是它。
    // 顺序反过来（先 add 再 parse），两次 now() 之间可能已经跨了一天。
    const auto createdAt = nowUtc();

    return note.add(raw, createdAt, occurredFromText(raw, createdAt));
}

ContentTimeUpdate recomputeTimeForContent(core::Entry const& original, std::string_view newText)
{
    const bool userOwnedTime =
        original.occurred && (original.occurred_source == core::TimeSource::Manual ||
                              original.occurred_source == core::TimeSource::Ai);

    if (userOwnedTime)
    {
        // 手改 / AI 定的时间优先：改内容不该把它打回 0 点。
        ContentTimeUpdate keep;
        keep.occurred        = original.occurred;
        keep.source          = original.occurred_source;
        keep.preservedManual = true;
        return keep;
    }

    ContentTimeUpdate updated;
    if (const auto reparsed = occurredFromText(newText, original.created_at))
    {
        updated.occurred = reparsed;
        updated.source   = core::TimeSource::Rule;
    }
    // 解析不出 → 时间清空（source 保持 None）
    return updated;
}

}  // namespace app
