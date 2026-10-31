#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "workflow/engine.hpp"
#include "workflow/graph.hpp"
#include "workflow/node.hpp"
#include "workflow/lua_runtime.hpp"
#include "workflow/lua_runtime.hpp"
#include "workflow/node_registry.hpp"
#include "workflow/run_context.hpp"

// doctest 打印枚举要用（且不能出现叫 toString 的函数 —— ADL 会劫持 doctest 的查找）。
namespace workflow
{
inline std::ostream& operator<<(std::ostream& out, NodeType type)
{
    return out << nodeTypeName(type);
}

inline std::ostream& operator<<(std::ostream& out, GraphErrorKind kind)
{
    return out << graphErrorName(kind);
}
}  // namespace workflow

// flow —— 编排引擎的数据模型与图算法。
//
// 前三组（加载 / 校验 / 拓扑 / 插值）是**已实现**的，应该绿。
// 最后一组（执行器）是留给你写的，现在红 —— 红了就说明该填 src/flow/engine.cpp。
//
// 注意：这些用例都不联网、不碰磁盘，全是内存里的 JSON。

namespace
{

using workflow::Graph;
using workflow::GraphErrorKind;
using workflow::NodeType;

nlohmann::json sampleFlow()
{
    return nlohmann::json::parse(R"({
        "id": "draft",
        "nodes": [
            { "id": "in",    "type": "input",  "fields": ["raw_text"] },
            { "id": "llm",   "type": "llm",    "model": "deepseek-chat",
              "prompt": "整理成 JSON：{{ in.raw_text }}" },
            { "id": "parse", "type": "json",   "input": "{{ llm.text }}" },
            { "id": "out",   "type": "output", "value": "{{ parse.summary }}" }
        ]
    })");
}

}  // namespace

TEST_CASE("flow：从 JSON 加载图，节点字段与配置都落到正确的位置")
{
    auto graph = workflow::loadGraph(sampleFlow());
    REQUIRE(graph.has_value());
    CHECK(graph->id == "draft");
    REQUIRE(graph->nodes.size() == 4u);

    const workflow::Node* llm = graph->find("llm");
    REQUIRE(llm != nullptr);
    CHECK(llm->type == NodeType::Llm);

    const auto* llmConfig = std::get_if<workflow::LlmConfig>(&llm->config);
    REQUIRE(llmConfig != nullptr);
    CHECK(llmConfig->model == "deepseek-chat");
    CHECK(llmConfig->prompt.find("{{ in.raw_text }}") != std::string::npos);
    CHECK(llmConfig->maxTokens == 512);          // 缺省值

    const workflow::Node* in = graph->find("in");
    REQUIRE(in != nullptr);
    const auto* inputConfig = std::get_if<workflow::InputConfig>(&in->config);
    REQUIRE(inputConfig != nullptr);
    REQUIRE(inputConfig->fields.size() == 1u);
    CHECK(inputConfig->fields[0] == "raw_text");

    CHECK(graph->find("不存在") == nullptr);
}

TEST_CASE("flow：校验能抓住重复 id、悬空边、缺 input、未知类型")
{
    SUBCASE("正常图通过")
    {
        auto graph = workflow::loadGraph(sampleFlow());
        REQUIRE(graph.has_value());
        CHECK(workflow::validate(*graph).has_value());
    }

    SUBCASE("重复 id")
    {
        auto json = sampleFlow();
        json["nodes"].push_back({{"id", "out"}, {"type", "output"}, {"value", "x"}});
        auto graph = workflow::loadGraph(json);
        REQUIRE(graph.has_value());
        auto result = workflow::validate(*graph);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == GraphErrorKind::DuplicateId);
    }

    SUBCASE("悬空边")
    {
        auto json = sampleFlow();
        json["nodes"][0]["next"] = {"不存在的节点"};
        auto graph = workflow::loadGraph(json);
        REQUIRE(graph.has_value());
        auto result = workflow::validate(*graph);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == GraphErrorKind::DanglingEdge);
    }

    SUBCASE("没有 input 节点")
    {
        auto json = sampleFlow();
        json["nodes"].erase(0);
        auto graph = workflow::loadGraph(json);
        REQUIRE(graph.has_value());
        auto result = workflow::validate(*graph);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == GraphErrorKind::NoInput);
    }

    SUBCASE("未知类型")
    {
        auto json = sampleFlow();
        json["nodes"][1]["type"] = "python";
        auto result = workflow::loadGraph(json);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == GraphErrorKind::UnknownType);
    }
}

TEST_CASE("flow：拓扑排序给出确定的顺序，并能发现环")
{
    auto graph = workflow::loadGraph(sampleFlow());
    REQUIRE(graph.has_value());

    auto order = workflow::topoOrder(*graph);
    REQUIRE(order.has_value());

    // 同一张图每次顺序要一致（可复现），且依赖关系满足。
    const auto position = [&order](std::string const& id) {
        for (std::size_t i = 0; i < order->size(); ++i)
        {
            if ((*order)[i] == id) { return i; }
        }
        return order->size();
    };
    CHECK(position("in") < position("llm"));
    CHECK(position("llm") < position("parse"));
    CHECK(position("parse") < position("out"));

    SUBCASE("有环时报 cycle")
    {
        auto json = sampleFlow();
        json["nodes"][0]["next"] = {"out"};
        json["nodes"][3]["next"] = {"in"};
        auto cyclic = workflow::loadGraph(json);
        REQUIRE(cyclic.has_value());

        auto result = workflow::validate(*cyclic);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == GraphErrorKind::Cycle);
    }
}

TEST_CASE("flow：插值替换引用，缺失时报错而不是留下原文")
{
    workflow::RunContext context;
    context.set("llm", nlohmann::json{{"text", "模型输出"}});
    context.set("parse", nlohmann::json{{"json", nlohmann::json{{"summary", "摘要内容"}}}});
    context.set("in", "原始输入");

    SUBCASE("单层字段")
    {
        auto result = context.interpolate("前缀 {{ llm.text }} 后缀");
        REQUIRE(result.has_value());
        CHECK(*result == "前缀 模型输出 后缀");
    }

    SUBCASE("多段路径")
    {
        auto result = context.interpolate("{{ parse.json.summary }}");
        REQUIRE(result.has_value());
        CHECK(*result == "摘要内容");
    }

    SUBCASE("整个节点值（字符串）")
    {
        auto result = context.interpolate("{{ in }}");
        REQUIRE(result.has_value());
        CHECK(*result == "原始输入");
    }

    SUBCASE("没有引用的文本原样返回")
    {
        auto result = context.interpolate("就是一段普通文字");
        REQUIRE(result.has_value());
        CHECK(*result == "就是一段普通文字");
    }

    SUBCASE("引用不存在的节点要报错")
    {
        auto result = context.interpolate("{{ nope.text }}");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("不存在的节点") != std::string::npos);
    }

    SUBCASE("引用不存在的字段要报错")
    {
        auto result = context.interpolate("{{ llm.missing }}");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("没有字段") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// 执行器
// ---------------------------------------------------------------------------

TEST_CASE("flow：执行器按拓扑序跑完一张图，产出 output")
{
    auto graph = workflow::loadGraph(nlohmann::json::parse(R"({
        "id": "mini",
        "nodes": [
            { "id": "in",  "type": "input",  "fields": ["raw_text"] },
            { "id": "out", "type": "output", "value": "{{ in.value }}" }
        ]
    })"));
    REQUIRE(graph.has_value());
    REQUIRE(workflow::validate(*graph).has_value());

    // 处理器：input 直接把外部输入透传，output 取值。
    workflow::NodeRegistry registry;
    registry.add(NodeType::Input, [](workflow::Node const&, workflow::RunContext const&) {
        return std::expected<workflow::Value, workflow::NodeError>{workflow::Value{{"value", "来自输入"}}};
    });
    registry.add(NodeType::Output, [](workflow::Node const& node, workflow::RunContext const& context)
                 -> std::expected<workflow::Value, workflow::NodeError> {
        const auto* config = std::get_if<workflow::OutputConfig>(&node.config);
        if (config == nullptr) { return std::unexpected(workflow::NodeError{false, "配置不匹配"}); }
        (void)context;
        return workflow::Value(config->value);
    });

    workflow::Runner      runner(registry);
    workflow::RunContext  context;
    workflow::RunOptions  options;

    auto run = runner.run(*graph, context, options);
    REQUIRE(run.has_value());
    CHECK(run->ok);
    CHECK(run->traces.size() == 2u);
    // 引擎在调用处理器前做插值（改的是副本），所以 Output 拿到的是替换后的值
    CHECK(run->output == "来自输入");

    // 而原图必须保持原样（图会被复用，第二次运行不能带着上次的值）
    const auto* outConfig = std::get_if<workflow::OutputConfig>(&graph->find("out")->config);
    REQUIRE(outConfig != nullptr);
    CHECK(outConfig->value == "{{ in.value }}");
}

// ---------------------------------------------------------------------------
// 执行器：失败 / 缺处理器 / 分支 / 跳过
// ---------------------------------------------------------------------------

namespace
{

workflow::NodeRegistry makeRegistry(int& outputCalls)
{
    workflow::NodeRegistry registry;

    registry.add(workflow::NodeType::Input,
                 [](workflow::Node const&, workflow::RunContext const&) {
                     return std::expected<workflow::Value, workflow::NodeError>{
                         workflow::Value{ { "value", "来自输入" } }
                     };
                 });

    registry.add(workflow::NodeType::Output,
                 [&outputCalls](workflow::Node const& node, workflow::RunContext const&) {
                     ++outputCalls;
                     const auto* config = std::get_if<workflow::OutputConfig>(&node.config);
                     if (config == nullptr)
                     {
                         return std::expected<workflow::Value, workflow::NodeError>{
                             std::unexpected(workflow::NodeError{ false, "配置不匹配" })
                         };
                     }
                     // 注意用圆括号：nlohmann::json 的花括号会被解释成 initializer_list，
                     // Value{"x"} 是 ["x"]（数组）而不是 "x"。这个坑值得写在测试里。
                     return std::expected<workflow::Value, workflow::NodeError>{
                         workflow::Value(config->value)
                     };
                 });

    return registry;
}

}  // namespace

TEST_CASE("flow：处理器失败时中止，错误里要有节点 id")
{
    auto graph = workflow::loadGraph(nlohmann::json::parse(R"({
        "id": "fail",
        "nodes": [
            { "id": "in",  "type": "input",  "fields": [] },
            { "id": "llm", "type": "llm",    "prompt": "{{ in.value }}" },
            { "id": "out", "type": "output", "value": "{{ llm.text }}" }
        ]
    })"));
    REQUIRE(graph.has_value());

    int                   outputCalls = 0;
    workflow::NodeRegistry registry   = makeRegistry(outputCalls);

    registry.add(workflow::NodeType::Llm,
                 [](workflow::Node const&, workflow::RunContext const&) {
                     return std::expected<workflow::Value, workflow::NodeError>{
                         std::unexpected(workflow::NodeError{ true, "模型超时" })
                     };
                 });

    workflow::Runner     runner(registry);
    workflow::RunContext context;
    workflow::FlowRun    run = runner.run(*graph, context, {}).value();

    CHECK_FALSE(run.ok);
    CHECK(run.error.find("llm") != std::string::npos);        // 指明哪个节点
    CHECK(run.error.find("模型超时") != std::string::npos);
    REQUIRE(run.traces.size() == 2u);                          // in 记一条，llm 记失败
    CHECK(run.traces[1].result.retryable);                     // retryable 透传
    CHECK(outputCalls == 0);                                   // 中止了，out 没跑
}

TEST_CASE("flow：stopOnError=false 时记录错误并继续")
{
    auto graph = workflow::loadGraph(nlohmann::json::parse(R"({
        "id": "tolerant",
        "nodes": [
            { "id": "in",  "type": "input",  "fields": [] },
            { "id": "out", "type": "output", "value": "固定值" }
        ]
    })"));
    REQUIRE(graph.has_value());

    int                   outputCalls = 0;
    workflow::NodeRegistry registry   = makeRegistry(outputCalls);

    // 让 input 失败
    workflow::NodeRegistry failing;
    failing.add(workflow::NodeType::Input,
                [](workflow::Node const&, workflow::RunContext const&) {
                    return std::expected<workflow::Value, workflow::NodeError>{
                        std::unexpected(workflow::NodeError{ false, "输入缺失" })
                    };
                });
    failing.add(workflow::NodeType::Output, [&outputCalls](workflow::Node const&,
                                                           workflow::RunContext const&) {
        ++outputCalls;
        return std::expected<workflow::Value, workflow::NodeError>{
            workflow::Value("固定值") };
    });

    workflow::Runner     runner(failing);
    workflow::RunContext context;

    workflow::RunOptions options;
    options.stopOnError = false;

    workflow::FlowRun run = runner.run(*graph, context, options).value();

    CHECK(run.ok);               // 没有致命错误
    CHECK(outputCalls == 1);     // 后面的节点照跑
    REQUIRE(run.traces.size() == 2u);
    CHECK_FALSE(run.traces[0].result.ok);
    CHECK(run.traces[1].result.ok);
    (void)registry;
}

TEST_CASE("flow：没注册处理器要明确报错，而不是跳过")
{
    auto graph = workflow::loadGraph(nlohmann::json::parse(R"({
        "id": "missing",
        "nodes": [ { "id": "in", "type": "input", "fields": [] } ]
    })"));
    REQUIRE(graph.has_value());

    workflow::NodeRegistry empty;   // 一个都没注册
    workflow::Runner       runner(empty);
    workflow::RunContext   context;

    workflow::FlowRun run = runner.run(*graph, context, {}).value();

    CHECK_FALSE(run.ok);
    CHECK(run.error.find("没有注册处理器") != std::string::npos);
}

TEST_CASE("flow：If 走 then 分支，else 分支被标 skipped 且不执行")
{
    auto graph = workflow::loadGraph(nlohmann::json::parse(R"({
        "id": "branch",
        "nodes": [
            { "id": "in",   "type": "input",  "fields": [] },
            { "id": "gate", "type": "if",     "when": "{{ in.value }}", "thenTo": "yes", "elseTo": "no" },
            { "id": "yes",  "type": "output", "value": "走了 then" },
            { "id": "no",   "type": "output", "value": "走了 else" }
        ]
    })"));
    REQUIRE(graph.has_value());

    int                   outputCalls = 0;
    workflow::NodeRegistry registry   = makeRegistry(outputCalls);

    registry.add(workflow::NodeType::If,
                 [](workflow::Node const& node, workflow::RunContext const&) {
                     (void)node;
                     return std::expected<workflow::Value, workflow::NodeError>{
                         workflow::Value{ { "branch", "then" } }
                     };
                 });

    workflow::Runner     runner(registry);
    workflow::RunContext context;

    workflow::FlowRun run = runner.run(*graph, context, {}).value();

    CHECK(run.ok);
    CHECK(run.output == "走了 then");
    CHECK(outputCalls == 1);   // 只执行了一个 Output

    bool elseSkipped = false;
    for (const auto& trace : run.traces)
    {
        if (trace.id == "no") { elseSkipped = trace.result.skipped; }
    }
    CHECK(elseSkipped);
}

TEST_CASE("flow：If 返回 else 时反过来")
{
    auto graph = workflow::loadGraph(nlohmann::json::parse(R"({
        "id": "branch2",
        "nodes": [
            { "id": "in",   "type": "input",  "fields": [] },
            { "id": "gate", "type": "if",     "when": "x", "thenTo": "yes", "elseTo": "no" },
            { "id": "yes",  "type": "output", "value": "走了 then" },
            { "id": "no",   "type": "output", "value": "走了 else" }
        ]
    })"));
    REQUIRE(graph.has_value());

    int                   outputCalls = 0;
    workflow::NodeRegistry registry   = makeRegistry(outputCalls);

    registry.add(workflow::NodeType::If,
                 [](workflow::Node const&, workflow::RunContext const&) {
                     return std::expected<workflow::Value, workflow::NodeError>{
                         workflow::Value{ { "branch", "else" } }
                     };
                 });

    workflow::Runner     runner(registry);
    workflow::RunContext context;

    workflow::FlowRun run = runner.run(*graph, context, {}).value();

    CHECK(run.ok);
    CHECK(run.output == "走了 else");

    bool yesSkipped = false;
    for (const auto& trace : run.traces)
    {
        if (trace.id == "yes") { yesSkipped = trace.result.skipped; }
    }
    CHECK(yesSkipped);
}

TEST_CASE("flow：同一个图跑两次结果一样（插值不污染原图）")
{
    auto graph = workflow::loadGraph(nlohmann::json::parse(R"({
        "id": "reuse",
        "nodes": [
            { "id": "in",  "type": "input",  "fields": [] },
            { "id": "out", "type": "output", "value": "{{ in.value }}" }
        ]
    })"));
    REQUIRE(graph.has_value());

    int                   outputCalls = 0;
    workflow::NodeRegistry registry   = makeRegistry(outputCalls);
    workflow::Runner       runner(registry);

    for (int i = 0; i < 2; ++i)
    {
        workflow::RunContext context;
        workflow::FlowRun    run = runner.run(*graph, context, {}).value();
        CHECK(run.ok);
        CHECK(run.output == "来自输入");
    }
}

// ---------------------------------------------------------------------------
// Script 节点（Lua）
// ---------------------------------------------------------------------------

namespace
{

std::filesystem::path writeScript(char const* name, std::string const& body)
{
    const auto dir = std::filesystem::temp_directory_path() / "noteall-lua-test";
    std::filesystem::create_directories(dir);
    const auto file = dir / name;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << body;
    out.close();
    return file;
}

// 让文件"确实变新"：NTFS 时间戳够细，但连续两次写太快可能落同一 tick
void bumpMtime(std::filesystem::path const& file)
{
    std::error_code ec;
    const auto      now = std::filesystem::last_write_time(file, ec);
    std::filesystem::last_write_time(file, now + std::chrono::seconds(2), ec);
}

}  // namespace

TEST_CASE("script：Lua 脚本能读写 input，返回表当输出")
{
    const auto file = writeScript("basic.lua", R"(
function run(input)
    return { category = "餐饮", amount = input.amount * 2 }
end
)");

    workflow::ScriptRuntime runtime;
    auto                    result = runtime.call(file, "run", nlohmann::json{ { "amount", 21 } });

    REQUIRE(result.has_value());
    CHECK((*result)["category"] == "餐饮");
    CHECK((*result)["amount"] == 42);
}

TEST_CASE("script：热重载 —— 文件改了立刻生效，不用重启")
{
    const auto file = writeScript("reload.lua", R"(
function run(input) return { tag = "v1" } end
)");

    workflow::ScriptRuntime runtime;

    const auto first = runtime.call(file, "run", nlohmann::json::object());
    REQUIRE(first.has_value());
    CHECK((*first)["tag"] == "v1");

    // 改脚本（并确保 mtime 变新）
    writeScript("reload.lua", R"(
function run(input) return { tag = "v2" } end
)");
    bumpMtime(file);

    const auto second = runtime.call(file, "run", nlohmann::json::object());
    REQUIRE(second.has_value());
    CHECK((*second)["tag"] == "v2");   // ★ 没有重启、没有 reload() 调用
}

TEST_CASE("script：改坏了（语法错）保留旧版本，不让节点失效")
{
    const auto file = writeScript("broken.lua", R"(
function run(input) return { ok = true } end
)");

    workflow::ScriptRuntime runtime;
    REQUIRE(runtime.call(file, "run", nlohmann::json::object()).has_value());

    // 用户手滑：少了 end
    writeScript("broken.lua", "function run(input) return { ok = false }");
    bumpMtime(file);

    const auto after = runtime.call(file, "run", nlohmann::json::object());
    REQUIRE(after.has_value());
    CHECK((*after)["ok"] == true);   // 还是旧行为

    // 错误信息要能看见（界面用它提示"你刚改的脚本有语法错误"）
    const auto cached = runtime.cachedScripts();
    REQUIRE(cached.count(file.string()) == 1);
    CHECK(cached.at(file.string()).find("错误") != std::string::npos);
}

TEST_CASE("script：死循环被指令预算掐断")
{
    const auto file = writeScript("loop.lua", R"(
function run(input)
    while true do end
end
)");

    workflow::ScriptRuntime runtime{ workflow::ScriptSettings{ 200'000 } };

    const auto result = runtime.call(file, "run", nlohmann::json::object());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("指令上限") != std::string::npos);
}

TEST_CASE("script：沙箱里没有 io / os / require")
{
    const auto file = writeScript("sandbox.lua", R"(
function run(input)
    return { io = io == nil, os = os == nil, require_is_nil = require == nil,
             dofile_is_nil = dofile == nil }
end
)");

    workflow::ScriptRuntime runtime;
    auto                    result = runtime.call(file, "run", nlohmann::json::object());

    REQUIRE(result.has_value());
    CHECK((*result)["io"] == true);
    CHECK((*result)["os"] == true);
    CHECK((*result)["require_is_nil"] == true);
    CHECK((*result)["dofile_is_nil"] == true);
}

TEST_CASE("script：作为节点接进图里跑（脚本 → output）")
{
    const auto file = writeScript("node.lua", R"(
function run(input)
    -- input 的形状和 {{ in.value }} 一致：按上游节点 id 取
    return { summary = "分类=" .. input["in"].value }
end
)");

    // 用占位符拼 JSON：路径里有反斜杠，直接塞进原始字符串会把引号搞乱
    std::string path    = file.generic_string();
    std::string jsonText = R"({
        "id": "script-flow",
        "nodes": [
            { "id": "in",     "type": "input",  "fields": [] },
            { "id": "script", "type": "script", "script": "__SCRIPT_PATH__" },
            { "id": "out",    "type": "output", "value": "{{ script.summary }}" }
        ]
    })";

    jsonText.replace(jsonText.find("__SCRIPT_PATH__"), std::string{ "__SCRIPT_PATH__" }.size(),
                     path);

    auto graph = workflow::loadGraph(nlohmann::json::parse(jsonText));
    REQUIRE(graph.has_value());
    REQUIRE(workflow::validate(*graph).has_value());

    int                         outputCalls = 0;
    workflow::NodeRegistry      registry    = makeRegistry(outputCalls);
    workflow::ScriptRuntime     runtime;
    workflow::addScriptNode(registry, runtime);

    workflow::Runner     runner(registry);
    workflow::RunContext context;

    workflow::FlowRun run = runner.run(*graph, context, {}).value();

    CHECK(run.ok);
    CHECK(run.output == "分类=来自输入");
    CHECK(context.has("script"));   // 脚本的输出也进变量表，下游能用 {{ script.summary }}
}

// ---------------------------------------------------------------------------
// 示例流程（examples/ 下的文件必须真的能跑，否则文档就是假的）
// ---------------------------------------------------------------------------

TEST_CASE("examples：bill-to-ledger 示例能加载、校验、用脚本跑出分类")
{
    const auto flowPath =
        std::filesystem::path{ NOTE_EXAMPLES_DIR } / "examples" / "bill-to-ledger.json";

    std::ifstream in(flowPath, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), flowPath.string());

    const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };

    auto graph = workflow::loadGraph(nlohmann::json::parse(text));
    REQUIRE(graph.has_value());
    REQUIRE(workflow::validate(*graph).has_value());

    int                    outputCalls = 0;
    workflow::NodeRegistry registry    = makeRegistry(outputCalls);

    // 覆盖 Input：模拟"一笔账单进来"（makeRegistry 的 Input 只返回占位值）
    registry.add(workflow::NodeType::Input,
                 [](workflow::Node const&, workflow::RunContext const&) {
                     return std::expected<workflow::Value, workflow::NodeError>{
                         workflow::Value{ { "note", "麦当劳午餐" }, { "amount_cents", 3250 } }
                     };
                 });

    workflow::ScriptRuntime runtime;
    workflow::addScriptNode(registry, runtime);

    // 示例里的 script 路径是相对 examples/ 的，测试时切到那个目录
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(std::filesystem::path{ NOTE_EXAMPLES_DIR });

    workflow::Runner     runner(registry);
    workflow::RunContext context;

    workflow::FlowRun run = runner.run(*graph, context, {}).value();

    std::filesystem::current_path(previous);

    REQUIRE(run.ok);
    CHECK(run.output == "餐饮（关键词:麦当劳）");   // 脚本返回英文键，输出模板照常引用
}
