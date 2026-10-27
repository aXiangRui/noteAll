#include "workflow/engine.hpp"

#include <string>
#include <utility>

// ===========================================================================
// 执行器 —— 留给你实现。
//
// 现在 run() 直接返回"未实现"，tests/test_flow.cpp 里那条用例是红的。
//
// 实现顺序建议（每一步都能单独跑测试）：
//   1. 拓扑序遍历，串行跑通 input -> llm -> output（处理器用假的即可）
//   2. 失败分支：stopOnError 时中止并返回错误；否则记录错误继续
//   3. 插值：调用处理器前，把 Node 里的 {{ ... }} 替换好（改副本，别改原图）
//   4. 条件节点：按处理器返回的分支走，没走到的节点标 skipped
//   5. RunContext 写入 + FlowRun::traces 记录（每个节点一条）
//
// 三个坑：
//   * 不要把插值结果写回 Graph —— 图会被复用，第二次运行就带着旧值了；
//   * 条件分支下，"没走到的节点"仍然在拓扑序里，要显式标 skipped，
//     否则轨迹里会看起来像"执行过"；
//   * Output 节点的结果是 FlowRun::output，别只放进 RunContext 就完事。
// ===========================================================================

namespace workflow
{

Runner::Runner(NodeRegistry const& registry) : registry_(registry)
{
}

std::expected<FlowRun, std::string> Runner::run(Graph const&    graph,
                                                RunContext&     context,
                                                RunOptions const& options) const
{
    (void)graph;
    (void)context;
    (void)options;

    return std::unexpected(std::string{"未实现：Runner::run（步骤见 src/flow/engine.cpp 顶部）"});
}

}  // namespace workflow
