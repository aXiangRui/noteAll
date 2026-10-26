#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "fake_chat_client.hpp"
#include "llm/draft.hpp"

// llm::extractDraft —— 从"模型回复"到"结构化草稿"的那一段。
//
// 全部用 FakeChatClient 的脚本化回复：不联网、秒级、能精确模拟坏 JSON / 越界 kind。
// 真模型的输出质量不在这里断言（那是人工验的事，见 docs/m2-实测.md）。

namespace
{

llm::ChatReply replyWith(std::string text)
{
    return llm::ChatReply{ std::move(text) };
}

llm::ChatError chatError(llm::ChatErrorKind kind, std::string message)
{
    return llm::ChatError{ kind, 0, std::move(message) };
}

}  // namespace

TEST_CASE("draft：合法 JSON 被解析成 summary / tags / kind")
{
    testutil::FakeChatClient client({ replyWith(
        R"({"summary":"与老张开会讨论排期","tags":["会议","排期"],"kind":"event"})") });

    auto fields = llm::extractDraft(client, "明天下午三点跟老张开会讨论排期");

    REQUIRE(fields.has_value());
    CHECK(fields->summary == "与老张开会讨论排期");
    CHECK(fields->kind == "event");
    REQUIRE(fields->tags.size() == 2);
    CHECK(fields->tags[0] == "会议");
    CHECK(client.calls() == 1);
}

TEST_CASE("draft：包了 ```json 围栏也能解析")
{
    testutil::FakeChatClient client({ replyWith(
        "```json\n{\"summary\":\"周五前发周报\",\"tags\":[\"周报\"],\"kind\":\"todo\"}\n```") });

    auto fields = llm::extractDraft(client, "记得周五前把周报发出去");
    REQUIRE(fields.has_value());
    CHECK(fields->kind == "todo");
    CHECK(fields->summary == "周五前发周报");
}

TEST_CASE("draft：坏 JSON / 缺字段 / 越界 kind 都算失败，而且标记可重试")
{
    SUBCASE("不是 JSON")
    {
        testutil::FakeChatClient client({ replyWith("我觉得这件事挺重要的") });
        auto fields = llm::extractDraft(client, "x");
        REQUIRE_FALSE(fields.has_value());
        CHECK(fields.error().retryable);   // 换个采样可能就正常
    }

    SUBCASE("summary 为空")
    {
        testutil::FakeChatClient client({ replyWith(R"({"summary":"","tags":["a"],"kind":"note"})") });
        auto fields = llm::extractDraft(client, "x");
        REQUIRE_FALSE(fields.has_value());
        CHECK(fields.error().message.find("summary") != std::string::npos);
    }

    SUBCASE("tags 为空数组")
    {
        testutil::FakeChatClient client({ replyWith(R"({"summary":"s","tags":[],"kind":"note"})") });
        auto fields = llm::extractDraft(client, "x");
        REQUIRE_FALSE(fields.has_value());
        CHECK(fields.error().message.find("tags") != std::string::npos);
    }

    SUBCASE("kind 不在白名单")
    {
        testutil::FakeChatClient client({ replyWith(R"({"summary":"s","tags":["a"],"kind":"meeting"})") });
        auto fields = llm::extractDraft(client, "x");
        REQUIRE_FALSE(fields.has_value());
        CHECK(fields.error().message.find("kind") != std::string::npos);
    }
}

TEST_CASE("draft：网络错误原样透传 retryable，不再自己判断")
{
    testutil::FakeChatClient client({ std::unexpected(chatError(llm::ChatErrorKind::RateLimited, "429")) });

    auto fields = llm::extractDraft(client, "x");
    REQUIRE_FALSE(fields.has_value());
    CHECK(fields.error().retryable);          // 429 可重试
    CHECK(fields.error().message == "429");

    testutil::FakeChatClient authClient({ std::unexpected(chatError(llm::ChatErrorKind::Auth, "401")) });
    auto                   auth = llm::extractDraft(authClient, "x");
    REQUIRE_FALSE(auth.has_value());
    CHECK_FALSE(auth.error().retryable);      // 401 不可重试
}
