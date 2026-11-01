#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

// QQ 聊天记录解析（消息管理器 → 导出 .txt）。
//
// 为什么要单独一层"中间格式"：
//   微信根本不能导出，QQ 能；将来还会冒出新工具。所以解析器只认
//   「时间 + 发送者 + 正文」这一件事，转换/导出环节由用户或别的工具搞定。
//
// QQ 导出的 txt 大致长这样（前面有一堆 ==== 和"消息对象:"之类的表头）：
//
//   消息记录（此消息记录为文本格式，不支持重新导入）
//   ================================================
//   消息分组:我的好友
//   消息对象:张三
//   ================================================
//
//   2026-10-30 12:10:33 张三
//   明天下午三点开会，讨论排期
//
//   2026-10-30 12:11:02 我
//   好的
//
// 判定规则：一行以「YYYY-MM-DD HH:MM(:SS)」开头 → 新消息；其余行 → 上一条的续行
// （聊天里多行消息很常见，不能丢）。
namespace import
{

struct ChatMessage
{
    std::chrono::sys_seconds occurred{};   // 本地时间已按系统时区转成 UTC
    std::string              sender;       // 发送者（已去掉 "(123456)" 这类 QQ 号）
    std::string              text;         // 正文（多行已用 \n 拼好）
    int                      lineNumber = 0;
};

struct ChatParseResult
{
    /// 消息对象（"消息对象:张三" 里的名字），可空
    std::string              peer;
    std::vector<ChatMessage> messages;
    std::vector<std::string> warnings;   // 坏行只警告，不中断
};

/**
 * 解析 QQ 导出文本（UTF-8）。
 *
 * 编码转换由调用方负责（QQ 导出在中文 Windows 上通常是 GBK，
 * 用 import::readTextFile 读，它会自动识别）。
 */
ChatParseResult parseQqChat(std::string_view utf8Text);

}  // namespace import
