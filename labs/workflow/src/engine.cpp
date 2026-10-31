#include "workflow/engine.hpp"

#include <algorithm>
#include <chrono>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// 执行器：按拓扑序跑一张图。
//
// 三个纪律（写在这里，因为它们是这个函数全部的价值）：
//   1. **不修改 Graph**：图是可复用的，插值结果必须写进临时副本；
//   2. **每个节点都留 trace**：包括没走到的分支（skipped=true），
//      否则轨迹看起来像"执行过"，排查时会误导人；
//   3. **错误要说清是哪个节点**：只报"执行失败"，用的人得自己二分查找。

namespace workflow
{

namespace
{

using Clock = std::chrono::steady_clock;

// 插值只作用于**值**字段。节点 id（thenTo/elseTo）和输入名（fields）不是模板，
// 插了反而会把连线改坏。
std::expected<NodeConfig, std::string> interpolateConfig(NodeConfig const& config,
                                                         RunContext const& context)
{
    if (const auto* llm = std::get_if<LlmConfig>(&config))
    {
        auto copy = *llm;
        if (auto model = context.interpolate(copy.model); model) { copy.model = *model; }
        else { return std::unexpected("model: " + model.error()); }

        if (auto prompt = context.interpolate(copy.prompt); prompt) { copy.prompt = *prompt; }
        else { return std::unexpected("prompt: " + prompt.error()); }

        return NodeConfig{ std::move(copy) };
    }

    if (const auto* json = std::get_if<JsonConfig>(&config))
    {
        auto copy = *json;
        if (auto input = context.interpolate(copy.input); input) { copy.input = *input; }
        else { return std::unexpected("input: " + input.error()); }

        return NodeConfig{ std::move(copy) };
    }

    if (const auto* branch = std::get_if<IfConfig>(&config))
    {
        auto copy = *branch;
        if (auto when = context.interpolate(copy.when); when) { copy.when = *when; }
        else { return std::unexpected("when: " + when.error()); }

        return NodeConfig{ std::move(copy) };
    }

    if (const auto* output = std::get_if<OutputConfig>(&config))
    {
        auto copy = *output;
        if (auto value = context.interpolate(copy.value); value) { copy.value = *value; }
        else { return std::unexpected("value: " + value.error()); }

        return NodeConfig{ std::move(copy) };
    }

    return config;   // Input：fields 是名字，不插值
}

// If 节点"走了哪条分支"的判定。
//
// 约定（宽容一点，方便用户自定义处理器）：
//   {"branch":"then"} / {"branch":"else"}  → 按字面
//   true / false                          → then / else
// 其它一律报错 —— 猜错分支比报错更难查。
std::expected<bool, std::string> takesThenBranch(Value const& value, std::string const& nodeId)
{
    if (value.is_boolean()) { return value.get<bool>(); }

    if (value.is_object() && value.contains("branch"))
    {
        const auto& branch = value["branch"];
        if (branch.is_string())
        {
            const std::string text = branch.get<std::string>();
            if (text == "then") { return true; }
            if (text == "else") { return false; }
        }
    }

    return std::unexpected("If 节点 " + nodeId +
                           " 的返回值无法判断分支（要 {\"branch\":\"then\"|\"else\"} 或 true/false）");
}

// 从某节点出发能到达的所有节点（含自身）。用来算"分支没走到的下游"。
std::set<std::string> reachableFrom(Graph const& graph, std::string_view start)
{
    std::set<std::string>  seen;
    std::queue<std::string> pending;

    if (start.empty()) { return seen; }
    pending.emplace(start);

    while (!pending.empty())
    {
        const std::string current = pending.front();
        pending.pop();

        if (!seen.insert(current).second) { continue; }

        const Node* node = graph.find(current);
        if (node == nullptr) { continue; }

        for (const auto& child : outgoing(*node)) { pending.emplace(child); }
    }

    return seen;
}

}  // namespace

Runner::Runner(NodeRegistry const& registry) : registry_(registry)
{
}

std::expected<FlowRun, std::string> Runner::run(Graph const&      graph,
                                                RunContext&       context,
                                                RunOptions const& options) const
{
    FlowRun    result;
    const auto order = topoOrder(graph);
    if (!order)
    {
        result.error = std::string{ graphErrorName(order.error().kind) } + ": " +
                       order.error().message;
        return result;
    }

    // 分支没走到的节点：跑到时直接标 skipped，不执行。
    std::set<std::string> skipped;

    for (const auto& nodeId : *order)
    {
        const Node* node = graph.find(nodeId);
        if (node == nullptr) { continue; }

        NodeResult nodeResult;

        if (skipped.contains(nodeId))
        {
            nodeResult.skipped = true;
            result.traces.push_back(NodeTrace{ nodeId, node->type, nodeResult });
            continue;
        }

        const NodeHandler* handler = registry_.find(node->type);
        if (handler == nullptr)
        {
            nodeResult.error = std::string{ "没有注册处理器: " } + nodeTypeName(node->type);
            result.traces.push_back(NodeTrace{ nodeId, node->type, nodeResult });
            result.error = "节点 " + nodeId + " 失败: " + nodeResult.error;
            result.ok    = false;
            return result;   // 缺处理器是配置错误，不该被 stopOnError 放过
        }

        // 插值：改副本，不改原图
        Node working  = *node;
        auto inlining = interpolateConfig(node->config, context);
        if (!inlining)
        {
            nodeResult.error = "插值失败: " + inlining.error();
            result.traces.push_back(NodeTrace{ nodeId, node->type, nodeResult });
            result.error = "节点 " + nodeId + " 失败: " + nodeResult.error;
            result.ok    = false;
            return result;
        }
        working.config = std::move(*inlining);

        const auto started = Clock::now();
        auto       value   = (*handler)(working, context);
        nodeResult.elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();

        if (!value)
        {
            nodeResult.error     = value.error().message;
            nodeResult.retryable = value.error().retryable;
            result.traces.push_back(NodeTrace{ nodeId, node->type, nodeResult });

            if (options.stopOnError)
            {
                result.error = "节点 " + nodeId + " 失败: " + nodeResult.error;
                result.ok    = false;
                return result;
            }

            continue;   // 容错模式：记下错误继续（下游缺变量时会自己报错）
        }

        context.set(nodeId, *value);

        nodeResult.ok     = true;
        nodeResult.output = *value;
        result.traces.push_back(NodeTrace{ nodeId, node->type, nodeResult });

        // Output：最后一个 Output 节点的结果就是整次运行的产出
        if (node->type == NodeType::Output) { result.output = *value; }

        // If：按返回值决定分支，另一条分支独有的下游全部标 skipped
        if (const auto* branch = std::get_if<IfConfig>(&working.config))
        {
            const auto thenTaken = takesThenBranch(*value, nodeId);
            if (!thenTaken)
            {
                result.error = "节点 " + nodeId + " 失败: " + thenTaken.error();
                result.ok    = false;
                return result;
            }

            const std::string& taken    = *thenTaken ? branch->thenTo : branch->elseTo;
            const std::string& notTaken = *thenTaken ? branch->elseTo : branch->thenTo;

            if (!notTaken.empty())
            {
                // 只标"走不到的分支独有"的节点：两条分支在下游汇合的那个节点仍要执行。
                const auto notTakenNodes = reachableFrom(graph, notTaken);
                const auto takenNodes    = reachableFrom(graph, taken);

                for (const auto& id : notTakenNodes)
                {
                    if (!takenNodes.contains(id)) { skipped.insert(id); }
                }
            }
        }
    }

    result.ok = true;
    return result;
}

}  // namespace workflow
