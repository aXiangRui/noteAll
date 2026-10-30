#include "llm/draft.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace llm
{

namespace
{

/**
 * 提示词。三条约束：只输出 JSON、summary 保留关键信息、tags 别写废话。
 *
 * 「tags 不要重复 kind 的含义」是实测加的：给"记得周五前发周报"生成草稿时，
 * 模型会把「待办」塞进 tags —— 和 kind=todo 完全重复。
 */
std::string draftPrompt(std::string const& rawText)
{
    return "你是笔记整理助手。把用户的一句话整理成 JSON 对象，只输出 JSON，不要解释、不要代码块。\n"
           "\n"
           "字段要求：\n"
           "- title：**只概括内容，绝对不要写时间**（时间已经画在日历时间轴上了，"
           "卡片上再写一遍是重复信息）；不超过 10 个字，不要句号、不要引号。\n"
           "  例：原文「明天下午三点跟老张开会讨论排期」→ title「跟老张讨论排期」，"
           "不要写成「明天下午开会」。\n"
           "  如果原文只有时间、没有具体内容，就用最接近的事由（比如「开会」），"
           "实在没有就写「记录」。\n"
           "- summary：不超过 30 字的中文摘要，保留原文里的关键信息（人名、项目、时间）\n"
           "- tags：2 到 4 个中文标签，字符串数组；不要重复 kind 的含义（如「待办」「想法」），"
           "也不要用「工作」「生活」「阅读」这类空泛词\n"
           "- kind：只能是 event / note / todo / thought 之一\n"
           "  （event=发生或将要发生的事，note=信息/资料，todo=待办，thought=感受想法）\n"
           "\n"
           "用户记录：" + rawText;
}

std::string trimCopy(std::string text)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };

    std::size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) { ++begin; }
    std::size_t end = text.size();
    while (end > begin && isSpace(text[end - 1])) { --end; }
    return text.substr(begin, end - begin);
}

/** 模型偶尔会包一层 ```json 围栏，去掉再解析。 */
std::string stripFence(std::string text)
{
    text = trimCopy(std::move(text));

    if (text.rfind("```", 0) == 0)
    {
        const auto firstNewline = text.find('\n');
        if (firstNewline != std::string::npos) { text = text.substr(firstNewline + 1); }
        if (const auto fenceEnd = text.rfind("```"); fenceEnd != std::string::npos)
        {
            text = text.substr(0, fenceEnd);
        }
        text = trimCopy(std::move(text));
    }
    return text;
}

DraftError badResponse(std::string message)
{
    // 坏响应按可重试处理：换个采样（temperature>0）或重发一次常常就好了。
    return DraftError{ true, std::move(message) };
}

}  // namespace

std::expected<DraftFields, DraftError> extractDraft(IChatClient& client, std::string const& rawText)
{
    ChatRequest request;
    request.messages.push_back({ Message::Role::User, draftPrompt(rawText) });
    request.maxTokens   = 256;
    request.temperature = 0.2;   // 结构化抽取要稳，不要发散

    auto reply = client.complete(request);
    if (!reply)
    {
        return std::unexpected(DraftError{ reply.error().retryable(), reply.error().message });
    }

    const std::string jsonText = stripFence(reply->text);

    nlohmann::json parsed;
    try
    {
        parsed = nlohmann::json::parse(jsonText);
    }
    catch (std::exception const& ex)
    {
        return std::unexpected(badResponse(std::string{"回复不是合法 JSON: "} + ex.what()));
    }

    if (!parsed.is_object()) { return std::unexpected(badResponse("回复顶层不是 JSON 对象")); }

    DraftFields fields;
    fields.summary = parsed.value("summary", std::string{});
    if (fields.summary.empty()) { return std::unexpected(badResponse("summary 缺失或为空")); }

    // title 缺失不算失败：老模型/老提示词的回复里没有它也照样能用，
    // 退回 summary 就行（卡片上显示层还会再截一次）。
    fields.title = parsed.value("title", std::string{});
    if (fields.title.empty()) { fields.title = fields.summary; }

    if (!parsed.contains("tags") || !parsed["tags"].is_array())
    {
        return std::unexpected(badResponse("tags 缺失或不是数组"));
    }
    for (auto const& tag : parsed["tags"])
    {
        if (tag.is_string() && !tag.get<std::string>().empty())
        {
            fields.tags.push_back(tag.get<std::string>());
        }
    }
    if (fields.tags.empty()) { return std::unexpected(badResponse("tags 为空")); }

    fields.kind = parsed.value("kind", std::string{});
    if (fields.kind != "event" && fields.kind != "note" && fields.kind != "todo" &&
        fields.kind != "thought")
    {
        return std::unexpected(badResponse("kind 不在白名单: " + fields.kind));
    }

    return fields;
}

namespace
{

std::string timeDraftPrompt(std::string const& rawText, std::string const& todayLocal)
{
    return "你只负责从一句话里读出时间，输出 JSON，不要解释、不要代码块。\n"
           "\n"
           "今天是（本地日期）：" + todayLocal + "。据此把「明天」「下周三」这类相对说法换算成具体日期。\n"
           "\n"
           "输出格式（两个字段，没有就写 null）：\n"
           "{\"start\":\"YYYY-MM-DD HH:MM\",\"end\":\"YYYY-MM-DD HH:MM\"}\n"
           "\n"
           "规则：\n"
           "- 一律 24 小时制，两位数补零（08:30、17:00）\n"
           "- 原文只写了日期、没写时刻 → 时刻用 00:00\n"
           "- 原文没有结束时间 → end 为 null，**不要自己编一个时长**\n"
           "- 原文里根本没有时间信息 → start 和 end 都为 null\n"
           "- 拿不准就写 null，绝不猜\n"
           "\n"
           "用户记录：" + rawText;
}

/** 从 JSON 里取一个"时间字符串或 null"。非字符串一律当空。 */
std::optional<std::string> optionalString(nlohmann::json const& object, char const* key)
{
    if (!object.contains(key)) { return std::nullopt; }

    const auto& value = object[key];
    if (!value.is_string()) { return std::nullopt; }

    std::string text = value.get<std::string>();
    if (text.empty()) { return std::nullopt; }
    return text;
}

}  // namespace

std::expected<TimeDraftFields, DraftError> extractTimeDraft(IChatClient&       client,
                                                            std::string const& rawText,
                                                            std::string const& todayLocal)
{
    ChatRequest request;
    request.messages.push_back({ Message::Role::User, timeDraftPrompt(rawText, todayLocal) });
    request.maxTokens   = 128;
    request.temperature = 0.0;   // 抽时间不需要创意，要稳定

    auto reply = client.complete(request);
    if (!reply)
    {
        return std::unexpected(DraftError{ reply.error().retryable(), reply.error().message });
    }

    nlohmann::json parsed;
    try
    {
        parsed = nlohmann::json::parse(stripFence(reply->text));
    }
    catch (std::exception const& ex)
    {
        return std::unexpected(badResponse(std::string{"回复不是合法 JSON: "} + ex.what()));
    }

    if (!parsed.is_object()) { return std::unexpected(badResponse("回复顶层不是 JSON 对象")); }

    TimeDraftFields fields;
    fields.startLocal = optionalString(parsed, "start");
    fields.endLocal   = optionalString(parsed, "end");

    if (!fields.startLocal && !fields.endLocal)
    {
        // 模型说"这句子没有时间" —— 这是**合法**结果，不是错误。
        // 内容草稿那边不同：summary 为空要报错。区别在于这里"没有时间"是正常语义。
        fields.startLocal = std::nullopt;
    }
    else if (!fields.startLocal && fields.endLocal)
    {
        return std::unexpected(badResponse("有 end 却没有 start"));
    }

    return fields;
}

}  // namespace llm
