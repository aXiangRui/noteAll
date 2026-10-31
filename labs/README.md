# labs —— 备用实验代码，**不参与主线构建**

这里的代码不编译、不进 ctest、也不被 `note.exe` 依赖。它们是一次"提前抽象"的产物，
留着当备胎，但已经不假装是产品的一部分。

## 当前内容

| 目录 | 是什么 | 状态 |
|---|---|---|
| `flow/` | 草稿调度队列（取任务 / 重试 / 放弃 / 预算 / 陈旧丢弃） | 桩：`drain` 返回空、`runOne` 未实现 |
| `workflow/` | 编排引擎（Graph / Node / NodeRegistry / RunContext / Runner） | **执行器已实现，11 条用例全绿**（加载/校验/拓扑/插值/执行/分支/失败策略） |

两者各自带着原来的测试（`test_draft_queue.cpp` / `test_workflow.cpp`）。
默认不编译；要看它们是否还活着：

```powershell
cmake -S . -B build -DNOTE_BUILD_LABS=ON
cmake --build build --target workflow_tests --config Debug
build\bin\Debug\workflow_tests.exe
```

## 为什么移到这里（决策记录）

**问题**：`note` 的真实依赖是 `core + parse + view + llm`，草稿流程是一条约五步的**直线**，
已经直接写在 `main.cpp` 里。`flow` 和 `workflow` 编译进了仓库，却**一个用户都没有**。

**代价**：零用户的基础设施会让架构看起来"已经定了"，实际没有；每次打开仓库都要
重新判断一次它该不该存在。

**结论**：先移出主线，不做进一步投入。

**重新评估的触发条件**（出现任一条再回来）：

1. 出现**第二条**需要编排的流程（现在的草稿流程是第一条）；
2. 需要**用户自己改**流程（那才需要图 + 变量 + 节点注册）；
3. 需要统一的重试 / 追踪 / 并发策略，且已经有三处各自实现。

**如果真要重写**：带着上面那三种真实需求写，约 2 小时能重写一遍，而且会比现在正确。
现在这份的价值主要是"想过了"，不是"能用了"。

## 怎么重新捡起来

```cmake
# 在 CMakeLists.txt 里加回（示例，仅 flow）
add_library(flow_lab
    labs/flow/src/draft_queue.cpp)
target_include_directories(flow_lab PUBLIC labs/flow/include ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(flow_lab PUBLIC core)
```

测试同理：把 `labs/<模块>/test_*.cpp` 加进一个 `tests_labs` target，
自己带 `tests/main.cpp` 和 doctest。

## 另一个野心：mini-Dify

那个方向**不属于这个仓库**。它需要图编辑器、变量系统、运行追踪、节点生态 ——
是另一个产品、另一套技术栈。真要做，开一个新仓库，把这里当参考实现读。
