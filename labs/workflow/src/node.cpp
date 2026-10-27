#include "workflow/node.hpp"

#include <utility>

namespace workflow
{

char const* nodeTypeName(NodeType type) noexcept
{
    switch (type)
    {
    case NodeType::Input:  return "input";
    case NodeType::Llm:    return "llm";
    case NodeType::Json:   return "json";
    case NodeType::If:     return "if";
    case NodeType::Output: return "output";
    }
    return "unknown";
}

std::optional<NodeType> nodeTypeFromName(std::string_view name)
{
    if (name == "input") { return NodeType::Input; }
    if (name == "llm") { return NodeType::Llm; }
    if (name == "json") { return NodeType::Json; }
    if (name == "if") { return NodeType::If; }
    if (name == "output") { return NodeType::Output; }
    return std::nullopt;
}

bool configMatchesType(Node const& node) noexcept
{
    switch (node.type)
    {
    case NodeType::Input:  return std::holds_alternative<InputConfig>(node.config);
    case NodeType::Llm:    return std::holds_alternative<LlmConfig>(node.config);
    case NodeType::Json:   return std::holds_alternative<JsonConfig>(node.config);
    case NodeType::If:     return std::holds_alternative<IfConfig>(node.config);
    case NodeType::Output: return std::holds_alternative<OutputConfig>(node.config);
    }
    return false;
}

std::vector<std::string> outgoing(Node const& node)
{
    std::vector<std::string> edges = node.next;

    // 条件节点的两条分支也算出边；空分支（elseTo 为空）表示"到此结束"。
    if (auto const* branch = std::get_if<IfConfig>(&node.config))
    {
        if (!branch->thenTo.empty()) { edges.push_back(branch->thenTo); }
        if (!branch->elseTo.empty()) { edges.push_back(branch->elseTo); }
    }

    return edges;
}

}  // namespace workflow
