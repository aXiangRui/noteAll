#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "app/chat_import.hpp"
#include "core/core.hpp"
#include "import/chat.hpp"

// QQ 聊天记录导入：解析（格式）→ 过滤（哪些值得记）→ 去重。
//
// 这里的重点不是"能不能解析"，而是"会不会把 note 淹没" —— 聊天记录 90% 是
// "好的""收到"，全导进去这个功能就是负价值。

using namespace std::chrono;

namespace
{

std::filesystem::path scratchFile(char const* name)
{
    const auto dir = std::filesystem::temp_directory_path() / "noteall-chat-test";
    std::filesystem::create_directories(dir);
    const auto file = dir / name;
    std::filesystem::remove(file);
    std::filesystem::remove(std::filesystem::path{ file }.concat(".lock"));
    std::filesystem::remove(std::filesystem::path{ file }.concat(".tmp"));
    return file;
}

const char* kQqExport =
    "消息记录（此消息记录为文本格式，不支持重新导入）\n"
    "\n"
    "================================================================\n"
    "消息分组:我的好友\n"
    "================================================================\n"
    "消息对象:张三\n"
    "================================================================\n"
    "\n"
    "2026-10-30 12:10:33 张三(12345678)\n"
    "明天下午三点开会，讨论排期\n"
    "\n"
    "2026-10-30 12:11:02 我(87654321)\n"
    "好的\n"
    "\n"
    "2026-10-30 12:15:40 我(87654321)\n"
    "记得带上上周的周报\n"
    "还有预算表，财务那边催了\n"
    "\n"
    "2026-10-30 12:16:10 张三(12345678)\n"
    "[图片]\n"
    "\n"
    "2026-10-31 09:02:11 我(87654321)\n"
    "下周三前把二期需求文档写完，周五评审\n";

}  // namespace

TEST_CASE("parseQqChat：跳过表头、剥掉 QQ 号、多行消息合并")
{
    const auto chat = import::parseQqChat(kQqExport);

    CHECK(chat.peer == "张三");
    REQUIRE(chat.messages.size() == 5);

    // 第一条：别人发的
    CHECK(chat.messages[0].sender == "张三");   // 不是 "张三(12345678)"
    CHECK(chat.messages[0].text == "明天下午三点开会，讨论排期");

    // 发送者也要处理 "我(87654321)" → "我"
    CHECK(chat.messages[1].sender == "我");
    CHECK(chat.messages[1].text == "好的");

    // 多行消息必须合并（聊天里非常常见，丢了就少半条信息）
    CHECK(chat.messages[2].text == "记得带上上周的周报\n还有预算表，财务那边催了");

    // 媒体占位保留原文，由过滤器负责跳过
    CHECK(chat.messages[3].text == "[图片]");
}

TEST_CASE("importChat：只导自己发的、够长的，并给出过滤原因")
{
    const auto chat = import::parseQqChat(kQqExport);

    core::Core note{ scratchFile("import-1.jsonl") };
    REQUIRE(note.load().has_value());

    app::ChatImportOptions options;   // 默认：只导"我"、>=8 字、跳过媒体
    const auto result = app::importChat(note, chat, options, false);

    REQUIRE(result.has_value());
    CHECK(result->scanned == 5);
    CHECK(result->added == 2);        // 多行那条 + 需求文档那条
    CHECK(result->filtered == 3);     // 张三两条 + "好的" + "[图片]"
    CHECK(note.list().empty());       // 预览不写盘
}

TEST_CASE("importChat：真正写入后，来源/时间/去重键都对")
{
    const auto file = scratchFile("import-2.jsonl");
    const auto chat = import::parseQqChat(kQqExport);

    core::Core note{ file };
    REQUIRE(note.load().has_value());

    app::ChatImportOptions options;
    const auto first = app::importChat(note, chat, options, true);
    REQUIRE(first.has_value());
    CHECK(first->added == 2);
    REQUIRE(note.list().size() == 2);

    // 来源是 QQ，去重键非空
    for (const auto& entry : note.list())
    {
        CHECK(core::toString(entry.source) == "qq");
        CHECK_FALSE(entry.source_key.empty());
        CHECK(entry.raw_text.size() > 4);
    }

    // 再导一次不能翻倍
    core::Core reopened{ file };
    REQUIRE(reopened.load().has_value());
    const auto second = app::importChat(reopened, chat, options, true);
    REQUIRE(second.has_value());
    CHECK(second->added == 0);
    CHECK(second->duplicates == 2);
    CHECK(reopened.list().size() == 2);
}

TEST_CASE("importChat：--all 时不限发送者，--min-len 可调")
{
    const auto chat = import::parseQqChat(kQqExport);

    core::Core note{ scratchFile("import-3.jsonl") };
    REQUIRE(note.load().has_value());

    app::ChatImportOptions options;
    options.fromMe    = "";    // 谁都导
    options.minLength = 2;     // "好的" 只有 2 字，放行

    const auto result = app::importChat(note, chat, options, false);
    REQUIRE(result.has_value());
    CHECK(result->added == 4);   // 5 条里只有 "[图片]" 被媒体规则挡掉
    CHECK(result->filtered == 1);
}
