#pragma once

#include <expected>
#include <map>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// 运行期上下文：变量表 + 插值。
//
// 设计要点：**插值由引擎统一做**。节点处理器拿到的永远是已经替换好的字符串，
// 所以处理器里不需要出现任何 "{{ ... }}" 的解析代码 —— 那是最容易写错、
// 也最需要统一报错的地方。
namespace workflow
{

using Value = nlohmann::json;

/** 处理器失败时的信息。retryable 决定上层（草稿队列 / 重试策略）要不要再试。 */
struct NodeError
{
    bool        retryable = false;
    std::string message;
};

struct NodeResult
{
    bool        ok      = false;
    bool        skipped = false;   // 条件分支没走到这条
    Value       output;
    std::string error;
    bool        retryable  = false;
    long long   elapsedMs  = 0;
};

class RunContext
{
public:
    /** 写入某节点的输出（引擎在每个节点成功后调用）。 */
    void set(std::string id, Value value);

    /** 读某节点的输出；没有返回 nullptr。 */
    [[nodiscard]] Value const* get(std::string_view id) const;

    [[nodiscard]] bool has(std::string_view id) const;

    [[nodiscard]] std::map<std::string, Value> const& vars() const noexcept { return vars_; }

    /**
     * 把 "{{ llm.text }}" 这样的引用替换成具体值。
     *
     * 支持：
     *   * 多段路径：{{ parse.json.summary }}（逐层取 json 字段）
     *   * 整个节点值：{{ llm }}（当上游输出是字符串时常用）
     * 失败（引用不存在的节点/字段）返回 error，而不是把原文留在结果里 ——
     * 静默留下 "{{ ... }}" 会让错误在很远的节点才爆，极难排查。
     */
    [[nodiscard]] std::expected<std::string, std::string> interpolate(std::string_view text) const;

private:
    std::map<std::string, Value> vars_;
};

}  // namespace workflow
