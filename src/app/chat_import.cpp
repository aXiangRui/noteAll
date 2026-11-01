#include "app/chat_import.hpp"

#include <cstdio>
#include <set>

#include "app/entry_service.hpp"

namespace app
{

namespace
{

// FNV-1a：只用来做去重指纹，不是加密。
std::string fingerprint(std::string_view sender, std::chrono::sys_seconds occurred,
                        std::string_view text)
{
    std::uint64_t hash = 1469598103934665603ULL;

    const auto mix = [&hash](std::string_view bytes) {
        for (const char byte : bytes)
        {
            hash ^= static_cast<unsigned char>(byte);
            hash *= 1099511628211ULL;
        }
    };

    mix(sender);
    mix(std::to_string(occurred.time_since_epoch().count()));
    mix(text.substr(0, std::min<std::size_t>(text.size(), 256)));   // 前 256 字节足够区分

    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return std::string{ "qq:" } + buffer;
}

// 纯媒体占位：[图片] [表情] [语音] [视频] [文件] [QQ红包] …
bool isMediaOnly(std::string_view text)
{
    std::string rest;
    rest.reserve(text.size());

    std::size_t index = 0;
    while (index < text.size())
    {
        if (text[index] == '[')
        {
            const auto close = text.find(']', index);
            if (close != std::string_view::npos)
            {
                index = close + 1;
                continue;
            }
        }
        rest.push_back(text[index]);
        ++index;
    }

    for (const char c : rest)
    {
        if (c != ' ' && c != '\t' && c != '\n') { return false; }
    }
    return true;
}

// 字符数（UTF-8 码点），不按字节 —— 中文一个字 3 字节，按字节算会把"好的"当 6 字。
std::size_t codePointCount(std::string_view text)
{
    std::size_t count = 0;
    for (const char c : text)
    {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) { ++count; }
    }
    return count;
}

}  // namespace

std::expected<ChatImportResult, std::string> importChat(core::Core&                    note,
                                                        import::ChatParseResult const& chat,
                                                        ChatImportOptions const&       options,
                                                        bool                           commit)
{
    ChatImportResult result;
    result.scanned = chat.messages.size();

    // 已有的去重键（含软删除的：用户删掉的聊天不该被重新导回来）
    std::set<std::string> known;
    for (const auto& entry : note.listAll())
    {
        if (!entry.source_key.empty()) { known.insert(entry.source_key); }
    }

    const auto skip = [&result](std::string reason) {
        ++result.filtered;
        if (result.notes.size() < 8) { result.notes.push_back(std::move(reason)); }
    };

    for (const auto& message : chat.messages)
    {
        if (!options.fromMe.empty() && message.sender != options.fromMe)
        {
            skip("不是自己发的（" + message.sender + "）");
            continue;
        }

        if (options.skipMedia && isMediaOnly(message.text))
        {
            skip("纯媒体占位");
            continue;
        }

        if (codePointCount(message.text) < options.minLength)
        {
            skip("太短：" + message.text.substr(0, 12));
            continue;
        }

        if (options.since && message.occurred < *options.since)
        {
            skip("早于起点时间");
            continue;
        }

        if (options.until && message.occurred > *options.until)
        {
            skip("晚于终点时间");
            continue;
        }

        const std::string key = fingerprint(message.sender, message.occurred, message.text);
        if (known.contains(key))
        {
            ++result.duplicates;
            continue;
        }

        if (commit)
        {
            // 时间怎么定：
            //   * 消息正文里写了时间（"明天下午三点开会"）→ 用解析出来的（相对**发言时刻**算）
            //   * 没写 → 就用发言时刻（比"导入时刻"准得多）
            const auto parsed = occurredFromText(message.text, message.occurred);

            const auto occurred = parsed ? parsed
                                         : std::optional<core::TimeRange>{ core::TimeRange{
                                               message.occurred, std::nullopt } };

            // 参数顺序：rawText, createdAt, occurred, TimeSource, sourceKey, Source
            const auto timeSource = parsed ? core::TimeSource::Rule : core::TimeSource::Manual;

            auto added = note.add(message.text, message.occurred, occurred, timeSource, key,
                                  core::Source::Qq);
            if (!added) { return std::unexpected("写入失败: " + added.error()); }
        }

        known.insert(key);
        ++result.added;
    }

    return result;
}

}  // namespace app
