#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// flow —— 编排引擎（mini-Dify）的数据模型。
//
// 核心原则：**节点是数据，不是代码**。
//   * 图要能从 JSON 加载（所以这里不能出现 std::function —— 它不可序列化）；
//   * "这个节点怎么执行"由引擎按 type 查注册表决定，见 node_registry.hpp；
//   * 每种节点类型的参数收在自己的 config 里（variant），不平铺在 Node 上 ——
//     否则 tool 节点会有一半字段永远是空的。
//
// 命名遵循 ROADMAP §0.3：类型 PascalCase，成员 camelCase，枚举值 PascalCase。
namespace workflow
{

enum class NodeType
{
    Input,    // 流程入口：声明这次运行需要哪些外部输入
    Llm,      // 调用大模型
    Json,     // 把上游的文本解析成 JSON（结构化输出）
    If,       // 条件分支：选一条边继续
    Output,   // 产出最终结果
};

char const* nodeTypeName(NodeType type) noexcept;

/** 从配置文件里的字符串解析类型；未知返回 nullopt（由 validate 报错）。 */
std::optional<NodeType> nodeTypeFromName(std::string_view name);

// ---------------------------------------------------------------------------
// 每类节点自己的参数
//
// 所有字符串里都可以写 {{ 节点id.字段 }} 引用上游输出 —— 插值由引擎统一做，
// 节点处理器拿到的永远是替换好的字符串（见 run_context.hpp）。
// ---------------------------------------------------------------------------

struct InputConfig
{
    std::vector<std::string> fields;   // 本次运行必须提供的输入名
};

struct LlmConfig
{
    std::string model;
    std::string prompt;
    int         maxTokens   = 512;
    double      temperature = 0.2;
};

struct JsonConfig
{
    std::string input;   // 例如 "{{ llm.text }}"
};

struct IfConfig
{
    std::string when;     // 例如 "{{ parse.kind }} == Event"
    std::string thenTo;   // 条件成立走哪个节点 id
    std::string elseTo;   // 不成立走哪个（可空 = 直接结束这条分支）
};

struct OutputConfig
{
    std::string value;    // 例如 "{{ parse.summary }}"
};

using NodeConfig = std::variant<InputConfig, LlmConfig, JsonConfig, IfConfig, OutputConfig>;

struct Node
{
    std::string              id;      // "llm_1"：可读，且能被 {{ llm_1.text }} 引用
    std::string              title;   // 轨迹里显示（可空）
    NodeType                 type = NodeType::Input;
    std::vector<std::string> inputs;  // 本节点要用的上游变量（文档/校验用）
    std::vector<std::string> next;    // 无条件出边；条件节点用 IfConfig 里的分支
    NodeConfig               config;
};

/**
 * config 与 type 是否匹配。
 *
 * 必查：处理器里用 std::get<LlmConfig>(node.config)，类型不对会抛
 * std::bad_variant_access。校验放在加载期，比运行期崩掉好。
 */
bool configMatchesType(Node const& node) noexcept;

/** 节点所有出边（含条件分支），校验与拓扑排序共用。 */
std::vector<std::string> outgoing(Node const& node);

}  // namespace workflow
