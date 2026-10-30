#pragma once

#include <expected>
#include <optional>
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
    std::string              title;   // 短标题（≤14 字，日历卡片只用它）
    std::string              summary;
    std::vector<std::string> tags;
    std::string              kind;    // event / note / todo / thought（原样保留字符串）
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

// ---------------------------------------------------------------------------
// 第二种草稿：只处理"时间"（timeDraft）
//
// 为什么单独做一个：内容和时间是两类判断，塞进一个 prompt 会互相干扰
// （模型为了写标题，容易把时间细节丢掉）。分开之后，用户也能**自己选**跑哪个：
//   - 内容草稿：标题 / 摘要 / 标签 / 类型
//   - 时间草稿：只推 开始 / 结束
//
// timeDraft 的输出**不自动落盘**：时间改错比标题改错严重得多，
// UI 把它填进编辑框，让用户看一眼再点保存。
// ---------------------------------------------------------------------------
struct TimeDraftFields
{
    std::optional<std::string> startLocal;   // "2026-10-30 08:30"（本地时间文本）
    std::optional<std::string> endLocal;     // 可为空 = 没有结束时间
};

/**
 * 从原文里抽时间，输出本地时间文本（"YYYY-MM-DD HH:MM"）。
 *
 * @param todayLocal 今天的本地日期（"2026-10-28"），模型靠它把「明天」算出来。
 *                   必须由调用方给，不能让模型自己"猜今天"。
 */
std::expected<TimeDraftFields, DraftError> extractTimeDraft(IChatClient&    client,
                                                            std::string const& rawText,
                                                            std::string const& todayLocal);

}  // namespace llm
