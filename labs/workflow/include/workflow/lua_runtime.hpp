#pragma once

#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// 用 Lua 写节点逻辑：节点是"数据 + 一段脚本"，脚本就是一个函数。
//
// 为什么是 Lua（而不是自研语言 / C++ 插件）：
//   * 自研语法要自己维护解析器、错误行号、文档、编辑器支持 —— 我们已经在
//     timeParse 上付过一次这个账（957 行 + 1133 行测试）；
//   * C++ 插件（DLL）热重载在 Windows 上等于给自己埋崩溃：FreeLibrary 时
//     还有对象活着就直接炸；
//   * Lua 只有一个 C API、标准库可裁剪、脚本改坏了最多"这个节点失败"。
//
// 安全边界（用户脚本在进程内跑，必须有）：
//   1. 只开 base/string/table/math/utf8 —— **没有 io / os / package / debug**；
//   2. 每次调用有指令数上限（防 while true do end 把 UI 卡死）；
//   3. 脚本读不到宿主的任何东西，只能拿到传进去的 input 表。
namespace workflow
{

struct ScriptSettings
{
    /// 单次调用的指令数上限。默认 200 万 ≈ 几十毫秒。
    std::size_t instructionLimit = 2'000'000;
};

class ScriptRuntime
{
public:
    explicit ScriptRuntime(ScriptSettings settings = {});
    ~ScriptRuntime();

    ScriptRuntime(ScriptRuntime const&)            = delete;
    ScriptRuntime& operator=(ScriptRuntime const&) = delete;

    /**
     * 调用某个脚本文件里的入口函数。
     *
     * @param scriptPath .lua 文件的路径
     * @param entry      入口函数名（默认 run）
     * @param input      传给脚本的表（对应 Lua 里的 input 参数）
     */
    std::expected<nlohmann::json, std::string> call(const std::filesystem::path& scriptPath,
                                                   std::string const&            entry,
                                                   nlohmann::json const&         input);

    /** 强制重新加载某个脚本（不传则清空全部缓存）。用于热重载/手动刷新。 */
    void reload(std::optional<std::filesystem::path> const& scriptPath = std::nullopt);

    /** 已经缓存了哪些脚本（路径 → 修改时间），给界面显示用。 */
    std::map<std::string, std::string> cachedScripts() const;

private:
    struct Impl;
    Impl* impl_;
};

/** 把 Script 节点的处理器注册进注册表（依赖注入点：runtime 在这里被捕获）。 */
class NodeRegistry;
void addScriptNode(NodeRegistry& registry, ScriptRuntime& runtime);

}  // namespace workflow
