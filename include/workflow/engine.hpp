#pragma once

#include <expected>
#include <string>
#include <vector>

#include "workflow/graph.hpp"
#include "workflow/node_registry.hpp"
#include "workflow/run_context.hpp"

// 执行器 —— 按拓扑序跑一张图。
//
// 这是**留给你实现**的部分（现在 src/flow/engine.cpp 是桩）。
// 需要处理的事，按实现顺序：
//   1. 拓扑序遍历；每个节点：查处理器 -> 调用 -> 结果写入 RunContext -> 记 trace
//   2. 条件节点（If）：处理器返回走了哪条分支，引擎按分支决定下一个节点；
//      没走到的分支上的节点标 skipped，不执行
//   3. 失败：stopOnError 时中止并把错误带出去；否则记录并继续
//   4. 插值：调用处理器之前，把 Node 里的 {{ ... }} 替换好（见 run_context.hpp）
//      —— 注意别改坏原 Node：图是可复用的，插值结果放临时副本里
namespace workflow
{

struct NodeTrace
{
    std::string id;
    NodeType    type = NodeType::Input;
    NodeResult  result;
};

struct FlowRun
{
    bool               ok = false;
    std::string        error;
    std::vector<NodeTrace> traces;
    Value              output;   // Output 节点的结果
};

struct RunOptions
{
    bool stopOnError = true;
};

class Runner
{
public:
    explicit Runner(NodeRegistry const& registry);

    std::expected<FlowRun, std::string> run(Graph const&         graph,
                                            RunContext&          context,
                                            RunOptions const&    options = {}) const;

private:
    NodeRegistry const& registry_;
};

}  // namespace workflow
