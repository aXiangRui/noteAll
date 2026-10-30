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

    /**
     * 草稿（summary / tags / kind）处在什么状态。
     *
     * 为什么不是 bool：
     *   None    —— 从来没生成过（导入的历史数据多半是它）
     *   Pending —— 欠着，等以后补（add 之后就是它）
     *   Failed  —— 试过、失败了，看 attempts 决定还试不试
     *   Done    —— 草稿已生成，等用户确认（此时 confirmed=false）
     */
    enum class DraftStatus
    {
        None,
        Pending,
        Failed,
        Done,
    };

    /**
     * 记录里的 occurred 是谁给的。
     *
     * 为什么要记：规则解析和模型抽取是两条完全不同的路径 ——
     *   Rule   —— 规则命中（确定性、可测、免费）
     *   Ai     —— 模型建议、用户采纳（要花钱、有概率）
     *   Manual —— 用户在详情面板手改（最高优先级）
     * 没有这个字段，就永远只能凭感觉判断"规则够不够用"。
     */
    enum class TimeSource
    {
        None,     // 时间未知（或已清除）
        Rule,     // 规则解析器给的
        Ai,       // 模型建议、被采纳
        Manual,   // 手动改的
    };

    struct Entry
    {
        std::string              id;           // 短随机 id，见 ROADMAP.md §2.5
        std::string              raw_text;     // 原始输入 —— 不可变，永不丢
        std::chrono::sys_seconds created_at;   // 记录时刻（UTC）
        std::optional<TimeRange> occurred;     // 事情发生的时刻/时段（UTC）；空 = 时间未知
        // 短标题：只给列表/卡片用（时间轴上一个格子放不下整段话）。
        // 空表示"没生成过"，显示层按 title → summary → 原文截断 依次回退。
        std::string              title;
        std::string              summary;      // LLM 生成摘要，可为空
        std::vector<std::string> tags;         // 集合语义
        Kind                     kind = Kind::Unknown;
        bool                     confirmed = false;   // 用户是否确认过解析结果
        Source                   source = Source::Manual;

        // 墓碑：非空 = 已删除。
        // 记录系统不物理删除（误删没救），删除只是打标记；真要清掉用 purge()。
        std::optional<std::chrono::sys_seconds> deleted_at;

        // 草稿状态机（见上面 DraftStatus 的注释）。
        // attempts 到上限就别再试了 —— 一条永远失败的记录不该每次启动都烧一次 token。
        DraftStatus draft_status   = DraftStatus::None;
        int         draft_attempts = 0;
        std::string draft_error;               // 最后一次失败的原因（给人看的）

        // 这个时间是谁给的（见 TimeSource）。
        // 没有它就只能凭感觉判断"规则够不够用"；有了它可以直接统计
        // rule / ai / manual 的分布，把 ai 那些原文变成下一批该补的规则。
        TimeSource  occurred_source = TimeSource::None;
    };

    // 枚举 <-> 字符串。存字符串不存数字，见 docs/nlohmann-json.md §4.3。
    std::string toString(Kind k);

    // 认不出时返回 Kind::Unknown，**不抛异常** —— 读历史数据时宁可降级，
    // 也不要因为一个陌生值让整个文件读不出来。
    Kind kindFromString(std::string_view s);

    std::string toString(Source s);

    // 同上：认不出按 Manual 处理，不抛。
    Source sourceFromString(std::string_view s);

    std::string toString(DraftStatus s);

    // 同上：认不出按 None 处理（当作"还没生成过"，安全）。
    DraftStatus draftStatusFromString(std::string_view s);

    std::string toString(TimeSource s);

    // 认不出按 None（当作"时间未知"，安全）。
    TimeSource timeSourceFromString(std::string_view s);
}  // namespace core
