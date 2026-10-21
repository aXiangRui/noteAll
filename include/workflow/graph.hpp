#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "workflow/node.hpp"

// 图：节点集合 + 校验 + 拓扑排序。
//
// 边怎么表示：挂在节点上（Node::next + IfConfig 的 thenTo/elseTo），不单独维护
// edges 列表。好处是"从 A 能去哪"一眼可见，坏处是校验时必须把所有出口汇总 ——
// 见 outgoing()。
namespace workflow
{

struct Graph
{
    std::string       id;
    std::vector<Node> nodes;

    [[nodiscard]] Node const* find(std::string_view id) const noexcept;
};

enum class GraphErrorKind
{
    Parse,           // JSON 结构不对
    Empty,           // 没有节点
    DuplicateId,     // 节点 id 重复
    UnknownType,     // type 字段不认识
    ConfigMismatch,  // type 与 config 不匹配
    DanglingEdge,    // 边指向不存在的节点
    NoInput,         // 没有 Input 节点
    Cycle,           // 有环
};

char const* graphErrorName(GraphErrorKind kind) noexcept;

struct GraphError
{
    GraphErrorKind kind = GraphErrorKind::Parse;
    std::string    message;
};

/**
 * 从 JSON 加载。
 *
 * 节点对象的形状（配置字段平铺在节点上，读起来最省事）：
 * {
 *   "id": "draft",
 *   "nodes": [
 *     { "id": "in",    "type": "input",  "fields": ["raw_text"] },
 *     { "id": "llm",   "type": "llm",    "model": "deepseek-chat",
 *       "prompt": "整理成 JSON：{{ in.raw_text }}" },
 *     { "id": "parse", "type": "json",   "input": "{{ llm.text }}" },
 *     { "id": "out",   "type": "output", "value": "{{ parse.summary }}" }
 *   ]
 * }
 *
 * 只做结构解析，**不做语义校验**（唯一性/环/悬空边由 validate 负责），
 * 这样调用方可以先 load 再决定要不要 validate。
 */
std::expected<Graph, GraphError> loadGraph(nlohmann::json const& json);

/** 语义校验：id 唯一、type/config 匹配、边存在、至少一个 Input、无环。 */
std::expected<void, GraphError> validate(Graph const& graph);

/** 拓扑序（节点 id 列表）。有环返回 Cycle。 */
std::expected<std::vector<std::string>, GraphError> topoOrder(Graph const& graph);

}  // namespace workflow
