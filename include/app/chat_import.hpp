#pragma once

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "core/core.hpp"
#include "import/chat.hpp"

// 把聊天记录变成 note。
//
// 关键判断：**过滤器比解析器更重要**。聊天记录 90% 是"好的""收到"，
// 全导进去会把 note 淹掉 —— 那样这个功能就是负价值。
namespace app
{

struct ChatImportOptions
{
    /// 只导这个发送者说的话（"我"）。空 = 不限。
    std::string fromMe = "我";

    /// 少于这个字数的消息跳过（"好的"/"收到"/"嗯"）。
    std::size_t minLength = 8;

    /// 纯媒体占位（[图片][表情][语音][文件]）跳过。
    bool skipMedia = true;

    std::optional<std::chrono::sys_seconds> since;
    std::optional<std::chrono::sys_seconds> until;
};

struct ChatImportResult
{
    std::size_t scanned    = 0;   // 解析出的消息数
    std::size_t filtered   = 0;   // 被规则过滤掉的
    std::size_t duplicates = 0;   // 已经导过的（source_key 命中）
    std::size_t added      = 0;   // 真正新增
    std::vector<std::string> notes;   // 前几条被过滤的原因（给人看）
};

/**
 * @param commit false = 只统计不写盘（预览）
 */
std::expected<ChatImportResult, std::string> importChat(core::Core&                     note,
                                                        import::ChatParseResult const&  chat,
                                                        ChatImportOptions const&        options,
                                                        bool                            commit);

}  // namespace app
