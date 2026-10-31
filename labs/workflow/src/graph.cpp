#include "workflow/graph.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace workflow
{

namespace
{

GraphError makeError(GraphErrorKind kind, std::string message)
{
    return GraphError{kind, std::move(message)};
}

std::string readString(nlohmann::json const& object, char const* key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) { return {}; }
    return it->get<std::string>();
}

std::vector<std::string> readStringArray(nlohmann::json const& object, char const* key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array()) { return {}; }
    std::vector<std::string> out;
    for (auto const& item : *it)
    {
        if (item.is_string()) { out.push_back(item.get<std::string>()); }
    }
    return out;
}

/** 解析单个节点：基础字段 + 按 type 取对应 config（配置字段平铺在节点对象上）。 */
std::expected<Node, GraphError> parseNode(nlohmann::json const& object)
{
    if (!object.is_object()) { return std::unexpected(makeError(GraphErrorKind::Parse, "节点必须是对象")); }

    Node node;
    node.id    = readString(object, "id");
    node.title = readString(object, "title");
    if (node.id.empty()) { return std::unexpected(makeError(GraphErrorKind::Parse, "节点缺少 id")); }

    const std::string typeName = readString(object, "type");
    if (typeName.empty())
    {
        return std::unexpected(makeError(GraphErrorKind::Parse, "节点 " + node.id + " 缺少 type"));
    }
    const auto type = nodeTypeFromName(typeName);
    if (!type)
    {
        return std::unexpected(makeError(GraphErrorKind::UnknownType,
                                         "节点 " + node.id + " 的类型无法识别：" + typeName));
    }
    node.type   = *type;
    node.inputs = readStringArray(object, "inputs");
    node.next   = readStringArray(object, "next");

    switch (node.type)
    {
    case NodeType::Input:
    {
        InputConfig config;
        config.fields = readStringArray(object, "fields");
        node.config   = std::move(config);
        break;
    }
    case NodeType::Llm:
    {
        LlmConfig config;
        config.model       = readString(object, "model");
        config.prompt      = readString(object, "prompt");
        config.maxTokens   = object.value("maxTokens", 512);
        config.temperature = object.value("temperature", 0.2);
        node.config        = std::move(config);
        break;
    }
    case NodeType::Json:
    {
        JsonConfig config;
        config.input = readString(object, "input");
        node.config  = std::move(config);
        break;
    }
    case NodeType::If:
    {
        IfConfig config;
        config.when   = readString(object, "when");
        config.thenTo = readString(object, "thenTo");
        config.elseTo = readString(object, "elseTo");
        node.config   = std::move(config);
        break;
    }
    case NodeType::Output:
    {
        OutputConfig config;
        config.value = readString(object, "value");
        node.config  = std::move(config);
        break;
    }
    case NodeType::Script:
    {
        ScriptConfig config;
        config.script = readString(object, "script");
        const auto entry = readString(object, "entry");
        if (!entry.empty()) { config.entry = entry; }
        node.config      = std::move(config);
        break;
    }
    }

    return node;
}

}  // namespace

char const* graphErrorName(GraphErrorKind kind) noexcept
{
    switch (kind)
    {
    case GraphErrorKind::Parse:          return "parse";
    case GraphErrorKind::Empty:          return "empty";
    case GraphErrorKind::DuplicateId:    return "duplicate-id";
    case GraphErrorKind::UnknownType:    return "unknown-type";
    case GraphErrorKind::ConfigMismatch: return "config-mismatch";
    case GraphErrorKind::DanglingEdge:   return "dangling-edge";
    case GraphErrorKind::NoInput:        return "no-input";
    case GraphErrorKind::Cycle:          return "cycle";
    }
    return "unknown";
}

Node const* Graph::find(std::string_view nodeId) const noexcept
{
    for (Node const& node : nodes)
    {
        if (node.id == nodeId) { return &node; }
    }
    return nullptr;
}

std::expected<Graph, GraphError> loadGraph(nlohmann::json const& json)
{
    if (!json.is_object()) { return std::unexpected(makeError(GraphErrorKind::Parse, "根节点必须是对象")); }

    Graph graph;
    graph.id = readString(json, "id");

    const auto nodes = json.find("nodes");
    if (nodes == json.end() || !nodes->is_array())
    {
        return std::unexpected(makeError(GraphErrorKind::Parse, "缺少 nodes 数组"));
    }

    for (auto const& item : *nodes)
    {
        auto node = parseNode(item);
        if (!node) { return std::unexpected(std::move(node.error())); }
        graph.nodes.push_back(std::move(*node));
    }

    return graph;
}

std::expected<void, GraphError> validate(Graph const& graph)
{
    if (graph.nodes.empty()) { return std::unexpected(makeError(GraphErrorKind::Empty, "图为空")); }

    std::set<std::string> ids;
    bool                  hasInput = false;

    for (Node const& node : graph.nodes)
    {
        if (!ids.insert(node.id).second)
        {
            return std::unexpected(makeError(GraphErrorKind::DuplicateId, "重复的节点 id：" + node.id));
        }
        if (!configMatchesType(node))
        {
            return std::unexpected(makeError(GraphErrorKind::ConfigMismatch,
                                             "节点 " + node.id + " 的配置与类型不匹配"));
        }
        if (node.type == NodeType::Input) { hasInput = true; }
    }

    if (!hasInput) { return std::unexpected(makeError(GraphErrorKind::NoInput, "缺少 input 节点")); }

    for (Node const& node : graph.nodes)
    {
        for (std::string const& to : outgoing(node))
        {
            if (!ids.contains(to))
            {
                return std::unexpected(makeError(GraphErrorKind::DanglingEdge,
                                                 "节点 " + node.id + " 指向不存在的节点：" + to));
            }
        }
    }

    // 环检测顺手复用拓扑排序：排不出全部节点 = 有环。
    auto order = topoOrder(graph);
    if (!order) { return std::unexpected(std::move(order.error())); }

    return {};
}

std::expected<std::vector<std::string>, GraphError> topoOrder(Graph const& graph)
{
    std::map<std::string, int>         inDegree;
    std::map<std::string, std::vector<std::string>> edges;

    for (Node const& node : graph.nodes) { inDegree[node.id] = 0; }

    for (Node const& node : graph.nodes)
    {
        for (std::string const& to : outgoing(node))
        {
            edges[node.id].push_back(to);
            ++inDegree[to];
        }
    }

    // 用节点原始顺序做初始队列，保证同一张图每次跑出来的顺序一致（可复现）。
    std::vector<std::string> queue;
    for (Node const& node : graph.nodes)
    {
        if (inDegree[node.id] == 0) { queue.push_back(node.id); }
    }

    std::vector<std::string> order;
    for (std::size_t index = 0; index < queue.size(); ++index)
    {
        const std::string current = queue[index];
        order.push_back(current);

        for (std::string const& to : edges[current])
        {
            if (--inDegree[to] == 0) { queue.push_back(to); }
        }
    }

    if (order.size() != graph.nodes.size())
    {
        return std::unexpected(makeError(GraphErrorKind::Cycle, "图里有环，无法确定执行顺序"));
    }

    return order;
}

}  // namespace workflow
