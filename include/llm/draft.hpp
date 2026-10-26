#pragma once

#include <expected>
#include <string>
#include <vector>

#include "llm/chat.hpp"

// 草稿抽取：一段原文 -> { summary, tags, kind }。
//
// 放在 llm 层而不是 flow 层，是因为它的核心是**提示词 + 输出校验**，
// 属于协议/语义，不是调度。调度（什么时候生成、失败重试几次）在 flow 层，
// 它只认一个 std::function，不需要知道这里的存在。
namespace llm
{

struct DraftFields
{
    std::string              summary;
    std::vector<std::string> tags;
    std::string              kind;   // event / note / todo / thought（原样保留字符串）
};

struct DraftError
{
    bool        retryable = false;   // 网络/限流/坏 JSON 可重试；鉴权、越界不可重试
    std::string message;
};

/**
 * 让模型把一段话整理成草稿，并**严格校验**输出。
 *
 * 校验规则（任何一条不满足都算失败，不猜、不降级）：
 *   * 必须是一个 JSON 对象（允许被 ```json 围栏包住）；
 *   * summary 非空字符串；
 *   * tags 是字符串数组且非空（空数组算失败 —— 空标签没有信息量）；
 *   * kind 必须是 event / note / todo / thought 之一。
 *
 * 失败时 retryable 由底层 ChatError 决定；校验失败按"坏响应"处理（可重试，
 * 因为换个采样可能就正常了）。
 */
std::expected<DraftFields, DraftError> extractDraft(IChatClient& client, std::string const& rawText);

}  // namespace llm
