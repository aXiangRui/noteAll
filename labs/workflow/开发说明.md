# 工作流开发说明（mini-Dify 内核）

> 状态：数据模型与图算法已实现并有测试；**执行器待实现**（`src/workflow/engine.cpp`）。
> 本文记录"为什么这么分层"，代码里的注释记录"怎么用"。

---

## 1. 目标与非目标

**目标**：做 Dify 的内核，不是它的画布。

一张工作流 = 用 JSON 声明的节点图，运行时按依赖顺序执行，变量在节点之间流动，
每一步都有轨迹可查。

**非目标**（至少 P0 不做）：

| 不做 | 原因 |
|---|---|
| 画布 UI | 那是前端工程，跟 C++ 引擎的进度无关；先把"图能跑"做对 |
| 知识库 / RAG 节点 | 需要 embedding + 向量库 + 分块，一个节点能把工期拖三倍 |
| 通用 Tool 节点（带 schema 校验） | 先用 HTTP 节点顶；工具协议是另一个话题 |
| 并行分支 | 串行拓扑序先跑通；并行只影响调度，不影响数据模型 |

---

## 2. 命名空间分工

| 命名空间 | 目录 | 负责 | 是谁的 |
|---|---|---|---|
| `workflow` | `include/workflow/`、`src/workflow/` | **编排引擎**：节点、图、校验、拓扑、执行、变量 | 你的 |
| `flow` | `include/flow/`、`src/flow/` | **草稿调度**：哪些笔记欠草稿、重试、预算、放弃 | 我的 |
| `llm` | `include/llm/`、`src/llm/` | 聊天协议：OpenAI 兼容请求/响应、错误分类 | 我的 |
| `net` | `include/LLM-connect/`、`src/LLM-connect/` | 字节收发（WinHTTP），不认识 JSON | 我的 |

依赖方向（单向）：

```
workflow  ──→ third_party(nlohmann)          ← 引擎只认 JSON，不认网络
flow      ──→ core                            ← 调度只认条目与状态
llm       ──→ net + third_party
（上层 main/CLI 负责把 workflow 的处理器接到 llm 上）
```

**关键：`workflow` 不认识 `llm`。** 节点执行函数由上层注册（见 §5），
所以引擎可以用假处理器测试，不需要网络。

---

## 3. 数据模型

### 3.1 Node 是数据，不是代码

```cpp
struct Node {
    std::string              id;      // "llm_1"，可被 {{ llm_1.text }} 引用
    std::string              title;
    NodeType                 type;
    std::vector<std::string> inputs;  // 文档/校验用
    std::vector<std::string> next;    // 无条件出边
    NodeConfig               config;  // std::variant，各类型自己的参数
};
```

三条硬规矩：

1. **`Node` 里不许出现 `std::function`** —— 它不可序列化，图就没法存 JSON、没法进 git。
2. **参数不许平铺**：`prompt` 只在 `LlmConfig` 里，`when` 只在 `IfConfig` 里。
   平铺的下场是每个节点一半字段永远是空的。
3. **`id` 必须是字符串**：变量引用要写成 `{{ llm_1.text }}`；整数 id 一旦重排，所有引用一起错位。

### 3.2 节点类型（P0 五种）

| type | 配置 | 作用 |
|---|---|---|
| `input` | `fields[]` | 声明本次运行需要哪些外部输入 |
| `llm` | `model` / `prompt` / `maxTokens` / `temperature` | 调模型 |
| `json` | `input` | 把上游文本解析成 JSON（结构化输出） |
| `if` | `when` / `thenTo` / `elseTo` | 条件分支 |
| `output` | `value` | 最终结果 |

`config` 与 `type` 不匹配在**加载期**就报错（`configMatchesType`），
否则处理器里 `std::get<LlmConfig>` 会抛 `bad_variant_access`。

---

## 4. 变量与插值

语法：`{{ 节点id.字段 }}`，支持多层 `{{ parse.json.summary }}`，也支持整个节点值 `{{ in }}`。

- 插值**由引擎统一做**，处理器拿到的永远是替换好的字符串；
- 引用不存在（节点/字段）**报错**，绝不把 `{{ ... }}` 原样留在结果里 ——
  静默留下会让错误在很远的节点才爆；
- 引擎要**在节点副本上**插值：图会被复用，改原图会把上次的值带进下一次运行。

---

## 5. 处理器注册（"某个节点执行什么函数"）

```cpp
using NodeHandler = std::function<
    std::expected<Value, NodeError>(Node const&, RunContext const&)>;
```

`std::function` 在 **`NodeRegistry`** 里，不在 `Node` 里。注册时用工厂闭包注入依赖：

```cpp
workflow::NodeHandler makeLlmHandler(llm::IChatClient& client)
{
    return [&client](workflow::Node const& node, workflow::RunContext const& ctx)
               -> std::expected<workflow::Value, workflow::NodeError> {
        const auto& cfg = std::get<workflow::LlmConfig>(node.config);
        auto reply = client.complete({.messages = {{{llm::Message::Role::User}, cfg.prompt}},
                                      .model = cfg.model, .maxTokens = cfg.maxTokens});
        if (!reply) return std::unexpected(workflow::NodeError{reply.error().retryable(), reply.error().message});
        return workflow::Value{{"text", reply->text}};
    };
}

workflow::NodeRegistry registry;
registry.add(workflow::NodeType::Llm, makeLlmHandler(*chat));
registry.add(workflow::NodeType::Input, inputHandler);
// ...
```

好处：换 DeepSeek/ollama 只改工厂参数；测试时注册假处理器，引擎逻辑照样能测。

---

## 6. 执行器（待实现）

`workflow::Runner::run(graph, context, options)` 要做的五件事，按顺序：

1. 拓扑序遍历，串行跑通 `input → output`；
2. `stopOnError` 的失败策略；
3. 调用处理器前在**副本上**插值；
4. `if` 节点按分支走，未走到的节点标 `skipped`（它们仍在拓扑序里）；
5. 记录 `FlowRun::traces`，`output` 节点的结果进 `FlowRun::output`。

---

## 7. 测试现状

| 组 | 文件 | 状态 |
|---|---|---|
| 加载 / 校验 / 拓扑 / 插值 | `tests/test_workflow.cpp` | ✅ 绿（4 个用例） |
| 执行器 | `tests/test_workflow.cpp` 最后一条 | 🟥 红（未实现） |
| 草稿调度 | `tests/test_draft_queue.cpp` | 🟥 红（10 条，桩实现） |
| llm 协议 | `tests/test_chat.cpp` | ✅ 绿（8 条，回环服务器） |
| 字节收发 | `tests/test_connect.cpp` | ✅ 绿（5 条，回环服务器） |

跑：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --target tests --config Debug
build\bin\Debug\tests.exe
```

---

## 8. 待定项

1. **条件表达式怎么求值**（`IfConfig::when`）：建议先只支持 `A == B` / `A != B` 两种，
   引擎里 split 一下即可；真要 `&&`/`contains` 再上解析器。
2. **并行分支**：数据模型不用改，改的是调度器（拓扑序 → 就绪队列）。
3. **运行记录持久化**：把 `FlowRun` 落盘，支持"重放某次运行"和"从失败节点接着跑"。
4. **画布**：等命令行版本稳定后，再考虑导 HTML 只读图或做 Web 编辑器。
