#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "workflow/engine.hpp"
#include "workflow/graph.hpp"
#include "workflow/node.hpp"
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
// ⬇️ 这一条现在应该是红的：执行器未实现。
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
        return workflow::Value{config->value};
    });

    workflow::Runner      runner(registry);
    workflow::RunContext  context;
    workflow::RunOptions  options;

    auto run = runner.run(*graph, context, options);
    REQUIRE(run.has_value());
    CHECK(run->ok);
    CHECK(run->traces.size() == 2u);
    CHECK(run->output == "{{ in.value }}");
}
