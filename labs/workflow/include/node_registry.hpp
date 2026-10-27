#pragma once

#include <cstddef>
#include <functional>
#include <map>

#include "workflow/node.hpp"
#include "workflow/run_context.hpp"

// 节点处理器注册表 —— "某个 type 执行什么函数"就定义在这里。
//
// 为什么 std::function 在这里、而不在 Node 里：
//   * Node 要能从 JSON 加载（std::function 不可序列化）；
//   * 同一种 type 在所有 flow 里行为一致，行为集中在一处才好审查；
//   * 依赖（IChatClient 等）由工厂闭包在注册时捕获，天然做得了依赖注入。
namespace workflow
{

/**
 * 处理器契约：
 *   * 拿到的 Node 已经过 loadGraph/validate，type 与 config 一定匹配；
 *   * 拿到的 RunContext 只读（写回由引擎做，避免处理器互相踩）；
 *   * 插值已经由引擎做完，处理器里不该再出现 "{{ ... }}"。
 */
using NodeHandler = std::function<std::expected<Value, NodeError>(Node const& node,
                                                                 RunContext const& context)>;

class NodeRegistry
{
public:
    void add(NodeType type, NodeHandler handler);

    /** 返回 nullptr 表示没注册；调用方负责报"不支持的节点类型"。 */
    [[nodiscard]] NodeHandler const* find(NodeType type) const noexcept;

    [[nodiscard]] bool has(NodeType type) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return handlers_.size(); }

private:
    std::map<NodeType, NodeHandler> handlers_;
};

}  // namespace workflow
