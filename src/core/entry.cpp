#include "core/entry.hpp"

#include <array>
#include <cassert>
#include <string>
#include <string_view>

namespace core
{

    std::string toString(Kind k)
    {
        switch (k)
        {
            case Kind::Unknown: return "unknown";
            case Kind::Event:   return "event";
            case Kind::Note:    return "note";
            case Kind::Todo:    return "todo";
            case Kind::Thought: return "thought";
        }

        // 走不到这里：上面的 case 已经覆盖了 Kind 的全部值。
        // 但 MSVC 不会据此推断"所有路径都返回了"，不给兜底就报 C4715。
        // assert 兜住真出了意外的情况（比如以后加了枚举值忘了加 case）。
        assert(false && "toString: Kind 有未处理的新值");
        return "unknown";
    }

    Kind kindFromString(std::string_view s)
    {
        constexpr std::array<Kind, 5> ALL_KINDS = {
            Kind::Unknown, Kind::Event, Kind::Note, Kind::Todo, Kind::Thought,
        };

        for (const Kind k : ALL_KINDS)
        {
            if (toString(k) == s)
            {
                return k;
            }
        }
        return Kind::Unknown;
    }

    std::string toString(Source s)
    {
        switch (s)
        {
            case Source::Manual: return "manual";
            case Source::Import: return "import";
            case Source::WeChat: return "wechat";
        }

        // 同 toString(Kind)：MSVC 不给兜底会报 C4715。
        assert(false && "toString: Source 有未处理的新值");
        return "manual";
    }

    Source sourceFromString(std::string_view s)
    {
        constexpr std::array<Source, 3> ALL_SOURCES = {
            Source::Manual, Source::Import, Source::WeChat,
        };

        for (const Source src : ALL_SOURCES)
        {
            if (toString(src) == s)
            {
                return src;
            }
        }
        // 认不出按"手敲"处理：历史数据没有这个字段，本来也都是手敲的。
        return Source::Manual;
    }
}
