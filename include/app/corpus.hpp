#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// 语料回流：把"AI 给的时间被采纳"这类真实样本存下来，变成规则的回归用例。
//
// 为什么要有它：规则解析的"完备"不是靠拍脑袋加分支，而是靠真实语料暴露缺口。
// 每次你采纳一次 AI 时间，就多一条 (原文, 基准, 期望) —— 哪天同一类写法
// 在语料里出现三次，就该把它实现成规则（三次法则）。
namespace app
{

struct ParseCase
{
    std::string                             text;    // 原文
    std::chrono::sys_seconds                base;    // 解析基准（当时那条记录的 created_at）
    std::optional<std::chrono::sys_seconds> start;   // 期望的开始（UTC）
    std::optional<std::chrono::sys_seconds> end;     // 期望的结束（UTC，可空）
};

struct CaseOutcome
{
    ParseCase   parseCase;
    bool        startOk = false;
    bool        endOk   = true;
    std::string gotStart;   // 实际解析结果（本地文本，给人看）
    std::string gotEnd;
};

/** 追加一条用例（文件不存在就建，带表头注释）。 */
bool appendParseCase(std::filesystem::path const& file, ParseCase const& parseCase,
                     const std::chrono::time_zone& tz);

/** 读回全部用例（`#` 开头是注释，空行跳过）。 */
std::expected<std::vector<ParseCase>, std::string> loadParseCases(
    std::filesystem::path const& file, const std::chrono::time_zone& tz);

/** 用**规则解析器**跑一遍全部用例（不调模型）。 */
std::vector<CaseOutcome> runParseCases(const std::vector<ParseCase>&  cases,
                                       const std::chrono::time_zone& tz);

}  // namespace app
