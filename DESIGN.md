# 个人时间轴记事本 — 设计方案

> 目标：用 C++ 开发一款个人可用的时间轴记事工具，同时作为 C++ 学习项目。
> 核心能力：自然语言 → 结构化时间轴条目（LLM 辅助）+ 集合/标签筛选。

---

## 0. 两个目标的冲突与解法

| 目标 | 时间尺度 | 要求 |
|---|---|---|
| 学会 C++ | 6–12 个月，伴随反复重写 | 深入底层，不怕推倒重来 |
| 做出可用软件 | 尽快闭环，否则会弃坑 | 快速跑通，要正反馈 |

**冲突点**：最容易的死法是"写到一半觉得架构不对 → 重写 → 再重写 → 永远没有可用版本"。

**解法：分层。**

- **学习区**——核心域逻辑，自己写，写它的过程就在学 C++。
- **糊弄区**——基础设施管道，直接用库，别造轮子。

判断标准一句话：**"自己造这个轮子，能学到 C++ 的核心概念吗？"** 能 → 自己写；只是在学 API 调用 → 用库。

---

## 1. 分层：学习区 vs 糊弄区

### 学习区（亲手实现）

| 模块 | 能学到的 C++ |
|---|---|
| Entry 数据结构 + 存储引擎 | RAII、所有权、`unique_ptr`、移动语义 |
| 查询 / 集合筛选 | STL 容器与算法、迭代器、泛型编程 |
| 时间表达式解析（"上周三"） | 字符串处理、`<chrono>`、时区、Unicode |
| 序列化 / 反序列化 | 文件 IO、错误处理、`std::expected` / `std::variant` |
| `View` 接口 + ConsoleView | 抽象基类、虚函数、多态、接口设计 |

### 糊弄区（直接用库）

| 需求 | 选择 | 理由 |
|---|---|---|
| HTTP 调 LLM | `cpr`（libcurl 封装） | 别手写 socket |
| JSON | `nlohmann/json` | 别手写 parser |
| 存储 | **JSONL 文件**（第一版） | 不上 SQLite |
| 构建 | CMake + vcpkg | 第一天配好 |
| 测试 | `doctest`（header-only） | 扔进来就能用 |
| GUI | **不碰** | 见 §6 |

---

## 2. 数据模型（本方案最关键的部分）

> Schema 设计错了，后面全部要重写；而且**它与选什么语言无关**。

### 2.1 核心不变式

**`raw_text` 和 `created_at` 是不可变的事实，其余一切都是可重新生成的派生物。**

这条决定了整个架构：

- LLM 解析错了 → 保留原文，重新解析即可修复，不会丢信息。
- 以后解析逻辑升级 → 可以回填重跑全部历史数据。
- LLM 挂了 / 换模型 → 系统降级，仍然可用。

**永远不要用 LLM 的输出覆盖原始文本。**

### 2.2 Entry 结构

```cpp
enum class Kind { Unknown, Event, Note, Todo, Thought };

struct TimeRange {
    std::chrono::sys_seconds start;
    std::optional<std::chrono::sys_seconds> end;   // 空 = 时间点，非空 = 时段
};

struct Entry {
    std::string              id;           // uuid，稳定标识
    std::string              raw_text;     // 原始输入 —— 不可变，永不丢
    std::chrono::sys_seconds created_at;   // 记录时刻（UTC）
    std::optional<TimeRange> occurred;     // 事情发生的时刻/时段（UTC）
                                           // 空 = 时间未知
    std::string              summary;      // LLM 生成摘要，可为空
    std::vector<std::string> tags;         // 集合语义
    Kind                     kind = Kind::Unknown;
    bool                     confirmed = false;   // 用户是否确认过 LLM 解析
};
```

### 2.3 三个必须分清的决策

**(a) `created_at` ≠ `occurred`。**
"我上周三去了杭州"——记录时间是今天，发生时间是上周三。**绝大多数笔记软件在这里混淆**，导致时间轴要么按错误的时间排序，要么干脆没法表达。必须分开存。

**(b) 时间点与时间段用同一个类型表达。**
`end` 为空即时间点。不要为一个概念建两种结构。

**(c) `occurred` 必须可为空。**
"最近有点迷茫"——这类记录没有时间，强行猜一个时间是**编造**。宁可空着，不要幻觉。

### 2.4 存储格式：JSONL

每行一条 JSON，追加写。可读、可 grep、可 git 管理、可手工修。

```json
{"id":"01J...","raw_text":"上周三下午和小李讨论了方案","created_at":"2026-10-08T15:30:00Z","occurred":{"start":"2026-10-01T14:00:00Z"},"summary":"与小李讨论方案","tags":["工作","项目A"],"kind":"event","confirmed":true}
```

**读写策略（第一版）**：启动时全量读入内存；修改时**原子重写**（写临时文件 → `rename`）。

- 为什么不上 SQLite：个人数据量（几千条）内存完全放得下，JSONL 的透明性对调试和手工修复价值极高。
- 什么时候换：文件 > 5MB，或需要并发写入，或查询开始变卡。

**多设备/单机权衡**：暂不考虑同步。若做，git 管 JSONL 文件是最省事的方案。

---

## 3. 架构与目录结构

```
cpp_workflow_demo/
├── CMakeLists.txt
├── DESIGN.md                    # 本文
├── docs/
│   ├── ROADMAP.md               # M0–M4 怎么做（含验收清单与现状）
│   ├── parse-time.md            # M1 时间解析：四步实现指南 + §3.5 现状速查
│   ├── timeParse-源码.md        # timeParse 两个文件的源码快照
│   ├── nlohmann-json.md         # json API 速查
│   ├── entry-point.md           # 教学：main / wmain / CRT 到底谁先跑
│   └── *.pdf / *.html           # 个人阅读用的导出副本（由 .md 转换），不纳入维护
├── third_party/                 # header-only 库，vendor 进来（见其 README）
│   ├── nlohmann/json.hpp        # 3.11.3
│   └── doctest/doctest.h        # 2.4.11
├── include/                     # 头文件 —— 与 src/ 镜像对应
│   ├── core/
│   │   ├── entry.hpp            # Entry / TimeRange / Kind          ✅
│   │   ├── store.hpp            # JSONL 持久化                      ✅
│   │   ├── core.hpp             # 门面：add / list / find / remove  ✅
│   │   ├── query.hpp            # 标签集合运算、时间筛选            ⬜ M3
│   │   └── view.hpp             # ⚠️ 旧版 View 残骸，无人 include，待删
│   ├── view/
│   │   ├── view.hpp             # View 接口（不是 IView，见 §4.2）
│   │   └── console_view.hpp
│   └── parse/
│       └── timeParse.hpp        # 中文时间解析（M1）                ✅
├── src/                         # 实现
│   ├── core/
│   │   ├── entry.cpp  store.cpp  core.cpp                          ✅
│   │   └── query.cpp            ⬜ M3
│   ├── view/
│   │   └── console_view.cpp
│   └── parse/
│       └── timeParse.cpp
├── main.cpp                     # wmain + CLI 分发
├── tools/
│   └── parse_demo.cpp           # 手动试 parseTime 的小工具（不是测试）
└── tests/
    ├── CMakeLists.txt
    ├── main.cpp                 # doctest 入口
    ├── test_parse.cpp           # ✅ 62 用例 / 438 断言
    └── test_store.cpp           # ❌ 只有注释，0 用例（最大的缺口）
```

**头文件约定**：`include/` 与 `src/` 结构镜像，包含时**带模块前缀**：

```cpp
#include "core/store.hpp"      // ✅ 一眼看出属于哪个模块
#include "store.hpp"           // ❌ 扁平写法，日后 core/view 出现同名文件就撞了
```

M1 增设 `{include,src}/parse/`，M2 增设 `{include,src}/llm/`。

**依赖方向**（必须单向）：

```
main.cpp → view / core / parse / llm
view     → core（只读数据模型）
core     → 无（纯领域逻辑）
parse    → core
llm      → core
```

**`core` 不得依赖任何 UI 概念。** 这条是硬约束。

---

## 4. UI 抽象：View 的形状

### 4.1 不采用：`onRender()` 无参接口

```cpp
// ❌ 不采用
struct View {
    virtual void onRender() = 0;
};
```

两个问题：

1. **抽象层级错了**。这是"绘制指令"级抽象，绑定在 **immediate-mode UI**（CLI / ncurses / ImGui）上。而 Qt/QML 和 Web 是 **retained mode**——你给出模型后是**框架反过来调你**，`onRender()` 无处安放。即：这个接口今天适配少数 UI，未来最可能用的那两种都不适配。
2. **C++ 生命周期坑**。无参意味着视图必须持有 `Core&` 才能取数据 → 视图与 core 相互知晓、引用永久绑死 → 悬垂引用 / 循环依赖风险。

### 4.2 采用：接收数据的"哑巴渲染器"

```cpp
// ✅ 采用（下面是实际代码的形状，见 include/view/view.hpp）
namespace view {

class View {
public:
    virtual void RenderView(const std::vector<core::Entry>& entries) = 0;
    virtual void RenderEntry(const core::Entry& entry)               = 0;
    virtual void RenderTags(const std::vector<std::string>& tags)    = 0;
    virtual void RenderMessage(std::string_view msg)                 = 0;
    virtual void RenderError  (std::string_view msg)                 = 0;
    virtual ~View() = default;      // ⚠️ 基类析构必须虚，否则派生类部分泄漏
};

}  // namespace view
```

> ⚠️ **本文档与实现的偏差（2026-10-14 追认现状）**：本节最初写的是
> `struct View` + `showEntries` / `showEntry` / `showMessage` / `showError`（camelCase，
> 与 [docs/ROADMAP.md §0.3](docs/ROADMAP.md) 的命名约定一致）。
> 实际实现叫 `View`、方法是 `Render*`、还多了个 `RenderTags`，
> 而且 **`ConsoleView` 没有做 `ostream&` 依赖注入**（它直接写 `std::cout` / `std::cerr`）。
> 命名之争无所谓，两套并存才是问题 —— 要么改代码，要么按现状改约定。
> 详见 [docs/ROADMAP.md §2.4](docs/ROADMAP.md)（已按现状改写）与 §2.11。

数据作为**参数**传入，不作为成员持有。视图不持有 core，无生命周期耦合，测试时塞个 vector 就能验。
此形状在 CLI / ImGui / Qt 下都能落地（Qt 里 `RenderView` 内部转成 model 塞给 `QListView` 即可）。

### 4.3 但要清楚它的天花板

**只有一个实现时抽出来的接口，几乎一定是错的。** 未来加 Web 前端时，中间大概率要插一层 HTTP/IPC，那时这个接口多半要重写。

- 现在写它，**价值在于练 C++ 接口设计与多态**，不在于"一劳永逸"。
- 守住一条：形状用"接收数据"，不要用"无参绘制"。
- 坦然接受未来重写。

---

## 5. 里程碑（每个都必须可用）

> 原则：**不要设计成"全部做完才能用"。** 每个里程碑结束都要有一个能天天用的东西。

### M0 — 记录与列出（1–2 周）✅ **功能已完成**（2026-10-14）

**目标**：跑通最小闭环，零 LLM、零时间解析、零筛选。

```
note add "今天下午和小李讨论了方案"   # → 追加 JSONL
note list                            # → 按 created_at 倒序列出
note show <id>
note rm <id>
```

**验收**：能连续记录一周不丢数据；JSONL 文件手工可读可修。
→ [ROADMAP.md §2.9](docs/ROADMAP.md) 那张清单 12 条，**实跑通过 11 条**；
唯一没做的是"连着用一周"（只有你能验）。另外**存储层单元测试至今没写** —— 这是当前最大的缺口。

**学到**：CMake 工程、RAII、文件 IO、`std::vector`、基础错误处理、`View` + `ConsoleView`
（依赖注入 `ostream&` 那条**没落地**，见 §4.2）。

### M1 — 时间解析 ✅ **能解析了，但还没接进 CLI**（2026-10-14）

**目标**：`note add "上周三 和小李讨论了方案"` 能识别出 `occurred`。
**范围**：相对日期（今天/昨天/上周三/下个月初）、绝对日期、时段（"下午 3 点到 5 点"）。
**验收**：20 个手写测试用例全过；解析不出的**留空而非猜测**。
→ 实际 **62 个用例 / 438 条断言全绿**；实际支持的范围比这里列的多
（大前天/大后天、下下周三、今年/明年、日期区间、跨零点区间、星期天…），
逐条清单见 [docs/parse-time.md §3.5](docs/parse-time.md)。
⚠️ `note add` 仍然不填 `occurred` —— `parseTime` 目前只有测试和 `tools/parse_demo.cpp` 在用。

**学到**：`<chrono>`、时区、字符串解析、异常安全。

### M2 — LLM 结构化
**目标**：自然语言 → `summary` / `tags` / `kind` / `occurred` 草稿。
**硬性要求**：
- **永不自动入库**，一律"生成草稿 → 用户确认 → 写入"，`confirmed` 字段标记。
- 先落 `raw_text`，**后台异步**调用，记录动作保持零延迟（写一条等 2 秒＝不会用）。
- 优先接**本地 ollama**，兼顾隐私与离线。
**学到**：HTTP 客户端、JSON、异步/线程、错误重试。

### M3 — 集合与查询
**目标**：标签作为集合，支持交/并/差筛选。

```
note list --tag 工作 --tag 项目A      # 交集（工作 ∩ 项目A）
note list --any-tag 工作,读书         # 并集
note list --since 2026-09-01
```

**注意**：值钱的是**集合这个数据模型**，不是韦恩图那张图。标签会动态生长（一年后可能几百个），集合可视化在规模上会失效。交互用**筛选器 + 时间轴**，韦恩图只作为两三个标签的对比小视图，不做主界面。
**学到**：STL 算法、`std::set_intersection`、泛型筛选器、lambda。

### M4 — 按需演进
- JSONL 撑不住了 → 换 SQLite（学习区，可以自己包一层）。
- 想要可视化 → 导出 Markdown（用现成编辑器看），或加本地 HTTP server 出 HTML。
- **仍然不碰 Qt**，除非前面全部跑顺了。

---

## 6. 明确不做（Non-Goals）

个人项目最大的杀手是范围蔓延。以下一律**第一版不做**：

- ❌ GUI（Qt / ImGui / Win32）——这是个人 C++ 项目最大的坟场
- ❌ 云同步、多用户、账号系统
- ❌ 富文本 / Markdown 所见即所得编辑器
- ❌ 插件系统、主题系统
- ❌ SQLite（M4 之前不做）
- ❌ 移动端
- ❌ "完美架构"——被真正咬到之前不重构

---

## 7. 技术栈与工具链

| 项 | 选择 | 说明 |
|---|---|---|
| 编译器 | **MSVC**（VS 2022 Build Tools，免费） | 别用 MinGW，中文与调试体验更差 |
| 标准 | **C++23** | 本机 MSVC 14.44 支持；用 23 是为了 `std::expected`（错误处理，已实测可用） |
| 构建 | CMake（**M0–M1 不用 vcpkg**） | 见下方「依赖策略」 |
| JSON | `nlohmann/json` 3.11.3 | header-only，已 vendor 到 `third_party/`；用法见 [docs/nlohmann-json.md](docs/nlohmann-json.md) |
| 测试 | `doctest` 2.4.11 | header-only，已 vendor |
| HTTP | `cpr`（经 vcpkg） | **M2** 才引入 |

### 依赖策略：先用 header-only，别急着上包管理器

`nlohmann/json` 与 `doctest` 都是**单头文件**库，丢进 `third_party/` 即可，不需要 vcpkg。
真正需要包管理器是 **M2** 接 `cpr` 时——它依赖 libcurl，那才是真依赖。

**先砍掉 vcpkg 这一环**，就少一个能卡住半天、且对学 C++ 毫无帮助的环节。
来源、版本、SHA256 与镜像下载方式见 [third_party/README.md](third_party/README.md)。

### 网络注意

本机实测 **github.com 与 raw.githubusercontent.com 不可达**（国内网络常见情况，非配置错误），
`gh-proxy.com` 可用。清华/中科大的 `github-release` 镜像未收录这两个库。
详见 [third_party/README.md](third_party/README.md)。

---

## 8. 已知的坑（提前知道能省半天到一周）

### 8.1 Windows 控制台编码 ✅ 必踩
控制台默认非 UTF-8，源码 / 字符串字面量 / 输出编码不一致会出乱码。对策：
- 源码文件全程 UTF-8（带 BOM 或不带，团队成员一致即可）
- MSVC 编译加 `/utf-8`
- 程序启动时 `SetConsoleOutputCP(CP_UTF8)`

> **本机实测**：三条都落实后中文输出正常（`main.cpp` 自检第 1 项）。这三条**不能省**。

### 8.2 MSVC 的 `<chrono>` 时区
C++20 时区支持在 MSVC 上可用，但需确认 `/std:c++20` 与最新工具集；早期版本可能需要 tzdata 配置。

> **本机实测通过**：MSVC 14.44，`zoned_time` + `std::format` 正常输出 `GMT+8`。M1 可以直接用，无需额外配置。

### 8.3 LLM 的失败模式是"自信地错"
它不报错，它**编造**。把"下周三"算成错的日期，你事后翻时间轴**根本无从发现**。
→ 所以 §5 M2 的"人工确认 + 保留原文"不是可选项，是硬要求。

### 8.4 相对时间归一化的基准
"上周三"是相对**记录时刻**还是**当前时刻**？两者在补记场景下不同。**规定：相对 `created_at`**，并在文档和代码注释里写明。

### 8.5 环境搭建：第一步是装编译器

实测本机**一个 C++ 编译器都没有**（无 MSVC、无 MinGW、无 clang，`C:\msys64` 是空壳）。
第一步：

```bash
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

约 4–7 GB。**注意 C 盘空间**——实测只剩 16 GB（已用 90%）。

装完用 CMake 的 Visual Studio 生成器即可，**不需要** `vcvarsall.bat`，也不需要把
`cl.exe` 加进 PATH：

```bash
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build
```

首次配置耗掉半天到一天是正常的，别拖到中途。

---

## 9. 现状与下一步

### 已完成（2026-10-14 实测）

| 项 | 状态 |
|---|---|
| 脚手架 | ✅ MSVC 14.44 + Windows SDK 26100、`CMakeLists.txt`、`include/`/`src/` 镜像结构、`third_party/` |
| 环境坑 | ✅ §8.1（中文乱码）/ §8.2（`<chrono>` 时区）**在本机都不存在**，实测通过 |
| 测试链路 | ✅ `ctest -C Debug` 绿：**62 用例 / 438 断言** |
| **M0 功能** | ✅ `add` / `list` / `show` / `rm` 全部可用，验收清单 12 条里 11 条实跑通过（§5 M0、[ROADMAP §2.9](docs/ROADMAP.md)） |
| **M0 存储层测试** | ❌ `tests/test_store.cpp` 只有注释，**0 个用例** —— 当前最大的缺口 |
| **M1 解析** | ✅ 62 个 parse 用例全绿，范围见 [docs/parse-time.md §3.5](docs/parse-time.md) |
| **M1 接线** | ❌ `parseTime` 还没接进 `note add`，`occurred` 一直是空的 |

构建命令：

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
./build/bin/Debug/note.exe       # 记事本体
./build/bin/Debug/parse_demo.exe # 手动试时间解析（--demo 看样例）
cd build && ctest -C Debug       # 跑测试
```

（Visual Studio 是多配置生成器：可执行文件带一层配置名目录；`ctest` 要用 `-C` 指定配置。）

### 下一步

**→ 完整清单（10 条，按建议顺序）见 [docs/ROADMAP.md §2.11](docs/ROADMAP.md)。**
最该先做的三件：

1. **建 git 基线**（仓库至今零提交，任何改动都没有退路）；
2. **补 `test_store.cpp`**：照 [ROADMAP §2.3](docs/ROADMAP.md) 那 8 条规格写，先红后绿；
3. **修两个静默坏数据的地方**：`from_json` 忽略 `fromIso` 返回值（坏时间变 1970）、
   `load()` 没检查 `exists` 的 `error_code`（IO 错误被当成"文件不存在"）。

其余文档：

| 文档 | 内容 |
|---|---|
| [docs/ROADMAP.md](docs/ROADMAP.md) | M0–M4 怎么做：接口规格、验收标准、**§2.11 现状与未完成清单** |
| [docs/parse-time.md](docs/parse-time.md) | M1 时间解析：四步实现指南 + **§3.5 现在能解析什么** |
| [docs/timeParse-源码.md](docs/timeParse-源码.md) | `timeParse.hpp` / `.cpp` 的源码快照（已按当天加固后的代码重导） |
| [docs/nlohmann-json.md](docs/nlohmann-json.md) | json 的 API 速查（写 `store.cpp` 时对照） |
| [docs/entry-point.md](docs/entry-point.md) | 教学：`main` / `wmain` / CRT —— 程序到底从哪开始跑 |
| [third_party/README.md](third_party/README.md) | 依赖来源、SHA256 与国内下载方式 |

> 📄 `docs/` 下的 `.pdf` / `.html` 都是**个人阅读用的导出副本**（由对应的 `.md` 转换而来），
> 不随 `.md` 一起维护：看不懂 `.md` 时自己转一份就行，**改了 `.md` 不必回头同步它们**。
> 真正的文档只有 `.md`。

一句话路线：`entry.hpp` 定数据结构 → `store` 读写 JSONL（**先写测试** ← 这条跳过了）→
`View` + `ConsoleView` → `core` 门面 → `main.cpp` 接 CLI → `parse` 解析时间（**还没接线**）。
实际规模 ≈ 900 行（M0）+ 1650 行（M1，含 767 行测试），不是当初估的 350 行 ——
多出来的部分主要是注释，**这不是坏事**：这份代码的可读性主要就靠它们。

---

## 附：核心设计原则速查

1. `raw_text` + `created_at` 不可变，其余皆可重新生成
2. `created_at` ≠ `occurred`，时间未知就留空，不猜
3. `core` 不依赖任何 UI 概念
4. UI 接口传数据，不持有 core
5. LLM 只建议，不自动入库
6. 每个里程碑都必须可用
7. 个人项目，YAGNI 到底
