#include "workflow/lua_runtime.hpp"

#include "workflow/node_registry.hpp"

// Script 节点的处理器：把"调 Lua"这一步包成一个普通 handler，注册进 NodeRegistry。
//
// 为什么要单独一个文件而不是塞进 lua_runtime.cpp：
//   lua_runtime 只该认识 Lua（脚本路径、输入、输出）；
//   它"是什么节点"是注册表的事 —— 这样以后要做 Tool 节点 / Http 节点也是同样的形状。

namespace workflow
{

namespace
{

// 脚本路径的相对基准：流程文件所在目录（用户写 "scripts/classify.lua" 就好）。
// 这里暂时用进程工作目录 —— 等 FlowStore 落地后由它注入 base 目录。
std::filesystem::path resolveScriptPath(std::string const& script)
{
    return std::filesystem::path{ script };
}

}  // namespace

void addScriptNode(NodeRegistry& registry, ScriptRuntime& runtime)
{
    registry.add(NodeType::Script, [&runtime](Node const& node, RunContext const& context) {
        const auto* config = std::get_if<ScriptConfig>(&node.config);
        if (config == nullptr)
        {
            return std::expected<Value, NodeError>{
                std::unexpected(NodeError{ false, "script 节点配置不匹配" })
            };
        }

        // 传给脚本的 input：**和 {{ 节点id.字段 }} 同一套命名** ——
        //   input["in"].value        等价于 {{ in.value }}
        // 另外附两个元信息：__node（本节点 id）、__title（标题，可空）。
        // 脚本拿不到宿主任何东西，只看到这张表。
        Value input = Value::object();
        for (const auto& [key, value] : context.vars()) { input[key] = value; }
        input["__node"] = node.id;
        if (!node.title.empty()) { input["__title"] = node.title; }

        auto result = runtime.call(resolveScriptPath(config->script), config->entry, input);
        if (!result)
        {
            // Lua 的报错不 retryable：语法/逻辑错误再试一次也一样
            return std::expected<Value, NodeError>{
                std::unexpected(NodeError{ false, result.error() })
            };
        }

        return std::expected<Value, NodeError>{ std::move(*result) };
    });
}

}  // namespace workflow
