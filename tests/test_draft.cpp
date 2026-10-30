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

TEST_CASE("draft：合法 JSON 被解析成 title / summary / tags / kind")
{
    testutil::FakeChatClient client({ replyWith(
        R"({"title":"老张开会","summary":"与老张开会讨论排期","tags":["会议","排期"],"kind":"event"})") });

    auto fields = llm::extractDraft(client, "明天下午三点跟老张开会讨论排期");

    REQUIRE(fields.has_value());
    CHECK(fields->title == "老张开会");
    CHECK(fields->summary == "与老张开会讨论排期");
    CHECK(fields->kind == "event");
    REQUIRE(fields->tags.size() == 2);
    CHECK(fields->tags[0] == "会议");
    CHECK(client.calls() == 1);
}

TEST_CASE("draft：回复里没有 title 时退回 summary（不算失败）")
{
    testutil::FakeChatClient client({ replyWith(
        R"({"summary":"周五前发周报","tags":["周报"],"kind":"todo"})") });

    auto fields = llm::extractDraft(client, "记得周五前把周报发出去");
    REQUIRE(fields.has_value());
    CHECK(fields->title == "周五前发周报");   // ★ 老模型/老提示词也能用
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

// ---------------------------------------------------------------------------
// timeDraft：只推时间，不碰标题。结果只填编辑框，不自动落盘。
// ---------------------------------------------------------------------------

TEST_CASE("timeDraft：正常返回 start / end")
{
    testutil::FakeChatClient client({ replyWith(
        R"({"start":"2026-10-30 08:30","end":"2026-10-30 10:00"})") });

    auto fields = llm::extractTimeDraft(client, "明天八点半到十点开会", "2026-10-29");
    REQUIRE(fields.has_value());
    REQUIRE(fields.value().startLocal.has_value());
    CHECK(*fields.value().startLocal == "2026-10-30 08:30");
    REQUIRE(fields.value().endLocal.has_value());
    CHECK(*fields.value().endLocal == "2026-10-30 10:00");
}

TEST_CASE("timeDraft：没有结束时间时 end 为 null（不编造时长）")
{
    testutil::FakeChatClient client({ replyWith(R"({"start":"2026-10-30 08:30","end":null})") });

    auto fields = llm::extractTimeDraft(client, "8:30开会", "2026-10-29");
    REQUIRE(fields.has_value());
    REQUIRE(fields.value().startLocal.has_value());
    CHECK_FALSE(fields.value().endLocal.has_value());   // ★ 不假设 1 小时
}

TEST_CASE("timeDraft：模型说「这句没时间」是合法结果，不是错误")
{
    testutil::FakeChatClient client({ replyWith(R"({"start":null,"end":null})") });

    auto fields = llm::extractTimeDraft(client, "记一个想法", "2026-10-29");
    REQUIRE(fields.has_value());
    CHECK_FALSE(fields.value().startLocal.has_value());
    CHECK_FALSE(fields.value().endLocal.has_value());
}

TEST_CASE("timeDraft：有 end 没 start 属于坏响应")
{
    testutil::FakeChatClient client({ replyWith(R"({"start":null,"end":"2026-10-30 10:00"})") });

    auto fields = llm::extractTimeDraft(client, "x", "2026-10-29");
    REQUIRE_FALSE(fields.has_value());
    CHECK(fields.error().message.find("end") != std::string::npos);
}

TEST_CASE("timeDraft：坏 JSON / 网络错误照旧透传")
{
    testutil::FakeChatClient badJson({ replyWith("我觉得是明天") });
    CHECK_FALSE(llm::extractTimeDraft(badJson, "x", "2026-10-29").has_value());

    testutil::FakeChatClient auth({ std::unexpected(chatError(llm::ChatErrorKind::Auth, "401")) });
    auto failed = llm::extractTimeDraft(auth, "x", "2026-10-29");
    REQUIRE_FALSE(failed.has_value());
    CHECK_FALSE(failed.error().retryable);
}
