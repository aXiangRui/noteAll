#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 整个 core 都在这个命名空间里。
// 理由见 docs/ROADMAP.md §2.0(b)：windows.h 往全局命名空间塞了几千个名字，
// 你再往全局放一个带 Event / Note 的 Kind，撞名是迟早的事。
namespace core
{
    enum class Kind
    {
        Unknown,
        Event,
        Note,
        Todo,
        Thought
    };

    /**
     * 这条记录从哪来。
     *
     * 为什么现在就要：以后会批量导入微信/别的笔记应用，没有来源字段，
     * 脏数据永远无法回溯（"哪些是导入的、能不能整批重来"）。
     * 存字符串不存数字（同 Kind 的理由，见 docs/nlohmann-json.md §4.3）。
     */
    enum class Source
    {
        Manual,   // 手敲
        Import,   // 文件 / 批量导入
        WeChat,   // 微信聊天记录导出
    };

    struct TimeRange
    {
        std::chrono::sys_seconds                start;
        std::optional<std::chrono::sys_seconds> end;   // 空 = 时间点，非空 = 时段
    };

    struct Entry
    {
        std::string              id;           // 短随机 id，见 ROADMAP.md §2.5
        std::string              raw_text;     // 原始输入 —— 不可变，永不丢
        std::chrono::sys_seconds created_at;   // 记录时刻（UTC）
        std::optional<TimeRange> occurred;     // 事情发生的时刻/时段（UTC）；空 = 时间未知
        std::string              summary;      // LLM 生成摘要，可为空
        std::vector<std::string> tags;         // 集合语义
        Kind                     kind = Kind::Unknown;
        bool                     confirmed = false;   // 用户是否确认过解析结果
        Source                   source = Source::Manual;

        // 墓碑：非空 = 已删除。
        // 记录系统不物理删除（误删没救），删除只是打标记；真要清掉用 purge()。
        std::optional<std::chrono::sys_seconds> deleted_at;
    };

    // 枚举 <-> 字符串。存字符串不存数字，见 docs/nlohmann-json.md §4.3。
    std::string toString(Kind k);

    // 认不出时返回 Kind::Unknown，**不抛异常** —— 读历史数据时宁可降级，
    // 也不要因为一个陌生值让整个文件读不出来。
    Kind kindFromString(std::string_view s);

    std::string toString(Source s);

    // 同上：认不出按 Manual 处理，不抛。
    Source sourceFromString(std::string_view s);
}  // namespace core
