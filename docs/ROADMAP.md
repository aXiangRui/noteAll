# M0–M4 实现路线图

> 配套 [DESIGN.md](../DESIGN.md)（**为什么这么设计**）——本文是**怎么做**。
> DESIGN.md 讲取舍与理由，本文给接口规格、行为规格、验收标准。
>
> 用法：M0 那一章照着写；写到哪卡住了再翻回对应的章节。
> M1–M4 只是路线，**别提前实现**。
>
> ---
>
> **⚠️ 现状（2026-10-14 实测）**：**M0、M1 都已实现**，本文已按实际代码校准过一次。
> 三处须知：
>
> 1. **§2.4 的接口与实现不一致**（`IView` → 实际是 `view::View` + `Render*`），下面那一节标了「现状」。
> 2. **§2.3 的 8 条存储层规格，目前 0 条有测试**（`tests/test_store.cpp` 只有注释）——
>    这是当前最大的缺口，见 §2.11。
> 3. 文档里凡是"实测"字样，都是真的在本机跑过的；数字（用例数、行数）也按实测更新过。

---

## 0. 全局约定

这些约定贯穿 M0–M4，先定下来，后面不反复。

### 0.1 环境与构建

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
cd build && ctest -C Debug
```

- 可执行文件在 **`build/bin/Debug/note.exe`**（VS 是多配置生成器，会多套一层 `Debug/`）
- 跑测试要 `ctest -C Debug`，**`-C` 不能省**，否则找不到配置
- 在 VSCode 里用 `${command:cmake.launchTargetPath}`，不用记路径

### 0.2 标准：C++23

`CMakeLists.txt` 已设 `CMAKE_CXX_STANDARD 23`。

用 23 不是赶时髦，是为了 `std::expected`——M0 的错误处理要用它（见 §0.4）。
本机 MSVC 14.44 实测支持，C++23 是 C++20 的超集，升上来没有代价。

### 0.3 命名约定

| 对象 | 风格 | 例子 |
|---|---|---|
| 类型 / 类 / 结构体 | `PascalCase` | `Entry`、`ConsoleView` |
| 函数 / 方法 | `camelCase` | `load`、`makeId`、`parseTime` |
| 成员变量 | `camelCase` + 尾部 `_` | `entries_`、`path_` |
| 常量 |  全大写 | `MAXLINE`、`BASE`（测试里） |
| 命名空间 / 文件名 | 全小写 | `core`、`store.hpp` |

**两处例外：**

1. **`to_json` / `from_json` 必须保持 snake_case。** 那是 nlohmann/json 通过 ADL
   规定的固定名字，改成别的就找不到（见 [docs/nlohmann-json.md §3](nlohmann-json.md)）。
2. ⚠️ **现状：`view` 层的方法用了 `PascalCase`** —— `RenderView` / `RenderEntry` /
   `RenderTags` / `RenderMessage` / `RenderError`。这与本节约定和 DESIGN.md §4.2
   原本写的 `showEntries` / `showEntry` / … 都不一致。

除此之外全项目一律 camelCase——**一致性比"和 STL 像"更值钱**。

> 例外 2 是**文档追认现状**，不是"这样写更好"。要么把 `view` 改成 camelCase，要么保留
> 现状但别再对外宣称"全项目 camelCase" —— 两套并存才是真正的问题。

### 0.4 错误处理策略 ⭐

**这是 M0 最值得想清楚的一件事。** 四种情况，四种手段，不要混：

| 情况 | 手段 | 例子 |
|---|---|---|
| **编程错误**（不变式被破坏，是 bug） | `assert` / 抛异常 | `save()` 传了空路径 |
| **可预期的失败**（是正常事，不是 bug） | `std::expected<T, std::string>` | 文件打不开、磁盘满、id 不存在 |
| **"可能没有"**（不是失败，是常态） | `std::optional<T>` | `find(id)` 没找到这条 |
| **坏数据但要继续** | 不失败，收集 warning | JSONL 里某一行坏了，跳过它继续读 |

```cpp
#include <expected>

std::expected<Entry, std::string>     e = ...;   // 要么有 Entry，要么有错误串
std::expected<void,  std::string>     v = ...;   // 成功时无返回值，失败时带错误串

if (!e) {
    std::string err = e.error();     // 取错误
    return;
}
Entry x = *e;                        // 取成功值（或 e.value()）
```

> `std::expected` 就是"带错误信息的 `optional`"。
> `expected<void, E>` 是它的特例：成功时没有值可返回，只表示"成了"。
> 已在**本机编译验证通过**（`expected<int,string>` 与 `expected<void,string>` 都可用）。

**不用异常做常规错误流**：文件不存在、用户输错 id，这些是每天都发生的正常事，
用异常会在代码里撒满 `try/catch` 且容易漏。异常留给真正的意外。

### 0.5 时间：内部一律 UTC

- `created_at` / `occurred` 存 **UTC**（`std::chrono::sys_seconds`）
- **只有显示的那一刻**才转本地时区（`std::chrono::zoned_time`，见 §2.7）
- 取当前时间：

```cpp
std::chrono::sys_seconds nowUtc() {
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}
```

> `floor` 是必须的：`system_clock` 的精度是 100 纳秒，不截断成秒，
> 序列化成 ISO 字符串时会把小数部分丢掉，读回来就不等于原来的值——**往返测试会挂**。

### 0.6 数据文件位置

优先级：环境变量 `NOTE_FILE` > 当前目录的 `notes.jsonl`。

```cpp
std::filesystem::path dataPath() {
    if (const char* env = std::getenv("NOTE_FILE")) return env;
    return "notes.jsonl";
}
```

M0 用当前目录，图的是**能直接打开文件看**。放到 `%APPDATA%` 是 M4 的事。

### 0.7 依赖方向（硬约束，写代码时手别抖）

```
main.cpp → view / core / parse
view     → core          只读数据模型，不碰存储
parse    → core          只依赖 TimeRange，不依赖 view
core     → 无            纯粹领域逻辑，不许出现任何"怎么显示"的东西
```

`core/` 里出现 `printf`、`cout`、`#include "view/..."` —— **都算写错了**。

> 现状：`CMakeLists.txt` 里 `note` 已经链接了 `parse`，但 `main.cpp` 还**没有调用**
> `parseTime`（见 §2.11 第 4 条）。`view` 也没有反向依赖 `core` 之外的东西，这条守住了。

---

## 1. 里程碑总览

| 里程碑 | 一句话目标 | 新增概念 | 规模（实测/原估） | 状态 |
|---|---|---|---|---|
| **M0** | 能记录、能列出、不丢数据 | CMake、RAII、文件 IO、`expected`、多态 | ≈ 900 行 / 估 350 | ✅ 功能完成，**存储层测试未写** |
| **M1** | "上周三"能变成 `occurred` | `<chrono>`、时区、字符串解析 | ≈ 1650 行 / 估 250 | ✅ 能解析，**未接进 CLI** |
| **M2** | LLM 生成 `summary`/`tags` 草稿 | HTTP、JSON、异步、重试 | ~300 行 | ⬜ 未开始 |
| **M3** | 按标签集合 / 时间筛选 | STL 算法、`set_intersection`、lambda | ~200 行 | ⬜ 未开始 |
| **M4** | 按真实需求演进 | 看情况 | 不定 | ⬜ 未开始 |

> M1 的 1650 行里，有 767 行是测试、164 行是 parse_demo，实现本身约 720 行
> （`timeParse.hpp` 103 + `timeParse.cpp` 617）。原估 250 行同样偏低。

**每个里程碑结束都必须有一个你愿意天天用的东西。** 做不到就是范围切错了，回头砍需求，别加需求。

---

## 2. M0 —— 记录与列出

**目标**：跑通最小闭环。**零 LLM、零时间解析、零筛选。**

### 2.0 开工前：先处理三个现状问题 ✅ **三处都已完成**（2026-10-14 实查）

代码写之前先把这三处收掉，否则第一步就编译不过。

> 下面三小节保留原文 —— 它们是"为什么必须包 `namespace core`"这类判断的记录，
> 值得留着。**现状**：`entry.hpp` 已有 `namespace core` 和完整 include；`include/core.hpp` 已删除。
> （另有一个同类残留：`include/core/view.hpp`，见 §2.11 第 1 条。）

**(a) `include/core/entry.hpp` 缺 include，且没进 `namespace core`。**

你写的那份结构体定义是对的，但有两处会导致编译失败：

```cpp
#pragma once

#include <chrono>      // ← 补
#include <optional>    // ← 补：用了 std::optional
#include <string>
#include <vector>      // ← 补：用了 std::vector

namespace core {       // ← 补：把整个文件包进来

enum class Kind { Unknown, Event, Note, Todo, Thought };

struct TimeRange {
    std::chrono::sys_seconds                start;
    std::optional<std::chrono::sys_seconds> end;   // 空 = 时间点
};

struct Entry {
    std::string              id;
    std::string              raw_text;
    std::chrono::sys_seconds created_at;
    std::optional<TimeRange> occurred;
    std::string              summary;
    std::vector<std::string> tags;
    Kind                     kind = Kind::Unknown;
    bool                     confirmed = false;
};

}  // namespace core
```

`std::optional` 和 `std::vector` 不写 include 就用了——它们的定义没进来，报的错会是
"`std::optional` 不是成员"这种看着莫名其妙的错。

**(b) 为什么要包 `namespace core`** —— 这不是洁癖，是具体风险：

`main.cpp` 里 `#include <windows.h>`。Windows 头文件往**全局命名空间**里塞了几千个宏和类型。
你再往全局塞一个 `Kind`（里面有 `Event`、`Note`）——**撞名是迟早的事**。
EUI-NEO（将来要接的 UI 库）同理。现在加一行 `namespace core`，比将来全局改名便宜一万倍。

**(c) 删掉 `include/core.hpp`。**

那是早期扁平布局的残留，和 `include/core/core.hpp` 内容重复。
留着的话 `#include "core.hpp"` 能编过，但它是死代码，日后必然踩坑。

```bash
rm include/core.hpp
```

---

### 2.1 交付物一览（行数已按实际代码更新）

| 文件 | 内容 | 行数（实测） | 状态 |
|---|---|---|---|
| `include/core/entry.hpp` | 数据结构 + `Kind` ↔ 字符串 | 39 | ✅ |
| `src/core/entry.cpp` | `toString` / `kindFromString` | 41 | ✅ |
| `include/core/store.hpp` | 接口声明 | 42 | ✅ |
| `src/core/store.cpp` | JSONL 读写 + JSON 转换 | 225 | ✅ |
| `include/view/view.hpp` | `View` 接口（原计划叫 `IView`） | 32 | ⚠️ 见 §2.4 |
| `include/view/console_view.hpp` | `ConsoleView` 声明 | 23 | ✅ |
| `src/view/console_view.cpp` | 终端渲染 | 91 | ✅ |
| `include/core/core.hpp` | `Core` 门面声明 | 43 | ✅ |
| `src/core/core.cpp` | 门面实现 | 163 | ✅ |
| `main.cpp` | `wmain` + 参数解析 | 197 | ✅ |
| `tests/test_store.cpp` | 存储层测试 | 7 | ❌ **只有注释，0 个用例** |

**M0 合计 ≈ 900 行**（含注释；最初估的是 350 行 —— 估计偏低，主要是因为注释写得很密）。

**先写 store，先写 test_store.cpp。** 理由：它最容易错、最好测、且不依赖别的东西。
先把它测绿，后面全是搭积木。

> ⚠️ **这条没有执行**：实际顺序是先写了 M1 的 parse（并且测试火力全压在 parse 上），
> `test_store.cpp` 至今是空的。详见 §2.11。

---

### 2.2 `entry.hpp` —— 数据结构

除结构体本身外，再加两个转换函数：

```cpp
// Kind <-> 字符串。存字符串不存数字（见 docs/nlohmann-json.md §4.3）
// 为什么：往枚举中间插一个值，数字存储会让全部历史数据静默错位
std::string toString(Kind k);
Kind        kindFromString(std::string_view s);   // 认不出 -> Kind::Unknown，不抛
```

**行为规格**：
- `toString` 返回 `"unknown"` / `"event"` / `"note"` / `"todo"` / `"thought"`，全小写
- `kindFromString` 认不出**不抛异常**，返回 `Kind::Unknown`
  —— 读历史数据时宁可降级，不要因为一个陌生值让整个文件读不出来

> camelCase 在这里正好避开一个坑：`toString` 与 `std::to_string` 名字不同，不会互相遮蔽。
> （`namespace core` 仍然必须有，理由见 (b)——真正会撞的是 `Kind`。）

---

### 2.3 `store` —— JSONL 持久化

`include/core/store.hpp`：

```cpp
#pragma once

#include "core/entry.hpp"
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace core {

struct LoadResult {
    std::vector<Entry>       entries;
    std::vector<std::string> warnings;   // 坏行说明，交给上层决定显不显示
};

// 文件不存在 -> 成功，返回空列表（不是错误！首次运行就是这情况）
// 打不开 / 读失败 -> 失败
std::expected<LoadResult, std::string> load(const std::filesystem::path& path);

// 原子写：先写 path.tmp，再 rename 覆盖 path
std::expected<void, std::string> save(const std::filesystem::path& path,
                                      const std::vector<Entry>& entries);

// ---- JSON 转换（ADL 找得到，必须放 namespace core）----
void to_json  (nlohmann::json& j, const Entry& e);
void from_json(const nlohmann::json& j, Entry& e);
void to_json  (nlohmann::json& j, const TimeRange& t);
void from_json(const nlohmann::json& j, TimeRange& t);

// ---- 时间 <-> ISO 8601 ----
std::string toIso(std::chrono::sys_seconds t);
bool fromIso(std::string_view s, std::chrono::sys_seconds& out);   // 失败返回 false

}  // namespace core
```

`toIso` / `fromIso` 的可用实现（**已在本机编译跑通**）见
[docs/nlohmann-json.md §4.2](nlohmann-json.md)。直接抄。

**行为规格**（每一条都要做成测试用例）：

| # | 场景 | 期望 |
|---|---|---|
| 1 | 文件不存在 | 成功，返回空 `entries`，**无 warning** |
| 2 | 空文件 | 成功，返回空 `entries` |
| 3 | 正常往返 | `save` 后再 `load`，所有字段逐字段相等 |
| 4 | `raw_text` 含 `"` `\` 换行 中文 | 转义正确，往返一致 |
| 5 | 某一行是坏 JSON | 成功，跳过该行，`warnings` 有 1 条，**其余行照常读出来** |
| 6 | `occurred` 为空 | 文件里**没有** `occurred` 键；读回来仍是空 |
| 7 | 给 `Entry` 加字段后读旧文件 | 不失败，新字段取默认值 |
| 8 | `save` 时目标已存在 | 被正常覆盖（原子替换） |

> ### ⚠️ 现状：这 8 条**一条测试都没有**
>
> `tests/test_store.cpp` 目前是 7 行注释、0 个用例。也就是说：
> 全项目 438 条断言**全部来自 parse**，而"唯一错了会静默丢数据"的这一层是裸奔的。
> 这是当前最该补的一件事（§2.11 第 3 条）。
>
> **另外，实现里有两个静默问题（2026-10-14 实测）：**
>
> 1. **`from_json(TimeRange)` 忽略了 `fromIso` 的返回值** ——
>    `{"occurred":{"start":"garbage"}}` 这种坏数据会**静默变成 1970-01-01**，
>    不出 warning、不报错（`created_at` 和 `end` 都检查了，只有 `start` 漏了）。
>    实测：`note show <id>` 打印 `发生时间: 1970-01-01 08:00`。
> 2. **`load()` 里 `std::filesystem::exists(path, ec)` 的 `ec` 从未检查** ——
>    权限/IO 错误会被当成"文件不存在"，于是返回**成功 + 空列表**，
>    用户以为数据没了（与规格第 1 条"文件不存在 → 成功"混在一起，无法区分）。
>
> 还欠一个 `save()` 的契约说明：它声明返回 `expected`，但
> `nlohmann::json(e).dump()` 遇到**非法 UTF-8 会抛异常**（见 [nlohmann-json.md §6](nlohmann-json.md)）。
> 现在 `raw_text` 一律经过 `main.cpp` 的 `toUtf8`，所以这条路走不到；
> 但 M2 从网络/文件读文本时就会通 —— 到时要么 `try/catch` 转成 `expected`，要么在注释里写明前提。

**三个必须注意的实现细节**：

1. **`dump()` 不要传参数**。传了 `indent` 就变成多行，JSONL 直接坏掉。
   默认的 `ensure_ascii = false` 正好保留中文原样。
2. **读的时候跳过空行**。`parse("")` 会抛异常。文件末尾常有空行。
3. **文件流用 `std::ios::binary`**。Windows 文本模式会把 `\n` 悄悄改成 `\r\n`，
   你写 1 行，文件里字节数对不上。用 binary，内容完全可控。

**`std::filesystem::rename` 在 Windows 上的坑**：MSVC 的实现底层是
`MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`，**会覆盖已存在的目标文件**，行为正确。
但如果目标文件正被别的程序打开（编辑器、杀软扫描、OneDrive 同步），
rename 会**间歇性失败**并抛 `filesystem_error`。

→ 对策：用 `std::error_code` 重载版（`rename(from, to, ec)`）拿到错误码而不是异常，
失败重试 3 次、每次歇 50ms，仍失败才真正报错。**这个坑会随机出现，很难查**，值得现在处理。

---

### 2.4 `view` —— `View` 接口（⚠️ 本节已按现状改写）

`include/view/view.hpp`（形状来自 [DESIGN.md §4.2](../DESIGN.md)）。

> ⚠️ **现状与原本的规格有三处出入**：接口叫 `View` 而不是 `IView`；方法名是 `Render*`
> 而不是 `show*`；另外多了个 `RenderTags`。下面是**实际的**头文件。

```cpp
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/entry.hpp"

namespace view {

class View
{
public:
    virtual void RenderView(const std::vector<core::Entry>& entries) = 0;
    virtual void RenderEntry(const core::Entry& entry) = 0;
    virtual void RenderTags(const std::vector<std::string>& tags) = 0;
    virtual void RenderMessage(std::string_view msg) = 0;
    virtual void RenderError(std::string_view msg) = 0;

    virtual ~View() = default;      // ← 必须有
};

}  // namespace view
```

**虚析构这条最重要**（原规格特意点出，实现也照做了）：

基类析构函数**必须**是虚的。否则 `delete` 一个 `View*`（实际指向 `ConsoleView`）时，
**只有基类部分被析构，派生类部分泄漏**——而且编译器不报错。
这是 C++ 多态最容易踩的坑之一。`= default` 让它保持朴素；代价是有了虚表指针，可忽略。

**⚠️ 两处没落地的设计**（文档追认现状，不是"这样更好"）：

1. 方法名用了 `PascalCase`，与 §0.3 的 camelCase 约定、DESIGN.md §4.2 的 `showEntries` 都不一致。
2. **`ConsoleView` 没有做 `std::ostream&` 依赖注入** —— 它直接写 `std::cout` / `std::cerr`，
   构造函数是默认的（`main.cpp` 里就是 `view::ConsoleView console;`）。
   DESIGN.md §4.2 的卖点之一是"测试时塞个 vector 就能验"，现状是**没法在测试里重定向输出**；
   §2.10 那张"学到的 C++"表里列的"依赖注入（传 `ostream&` 进去）"，**实际没有落地**。

`ConsoleView`（实际声明）：

```cpp
#pragma once

#include "view/view.hpp"

namespace view {

class ConsoleView final : public View
{
public:
    void RenderView(const std::vector<core::Entry>& entries) override;
    void RenderEntry(const core::Entry& entry) override;
    void RenderTags(const std::vector<std::string>& tags) override;
    void RenderMessage(std::string_view msg) override;
    void RenderError(std::string_view msg) override;
};

}  // namespace view
```

> `final` 表示"不许再继承我"。**不需要被继承的类就标 `final`**，
> 编译器能多做优化，也能拦住无意的继承。

**输出格式**（下面是从 `note.exe` 实跑抓的原文，2026-10-14）：

```
$ note list
2026-10-14 22:59  d33fee858007  第 1 条
2026-10-14 22:59  d32fae4f90f1  第 9 条

$ note list                    # 一个记录都没有时
（还没有记录）

$ note show craft0000001
id:        craft0000001
记录时间:  2026-10-08 08:00
kind:      unknown
原文:      第一行
第二行	制表                  # ← 原文里的换行和制表符原样打出来
摘要:      （无）
标签:      （无）
发生时间:  （未知）
```

- 时间用**本地时间**（`zoned_time` 转换，见 §2.7）；文件里存 UTC
- id 定长 12 字符，天然对齐，不用额外补空格
- 空列表文案是 `（还没有记录）` —— **不是**原规格写的 `（还没有任何记录）`
- `发生时间`：`occurred` 有值就打 `start`（有 `end` 再跟 ` ~ end`），否则 `（未知）`
- `摘要` / `标签` 为空时打 `（无）`

**`RenderError` 输出到哪**？走 **`std::cerr`**，前缀是 `错误: `（不是 `[错误]`，也没走 `out_`）。
这是实现**比原计划更好**的一处：`note list > out.txt` 时错误仍留在屏幕上。

> ⚠️ **副作用**：`RenderError` 被两种东西共用 —— 真错误，以及**坏行的 warning**
> （`main.cpp` 里 `for (const auto& w : note.warnings()) v.RenderError(w);`）。
> 于是"某一行坏了、已跳过"会顶着 `错误: ` 打出来，但退出码仍是 0。
> 想分清就得给接口加一个 `RenderWarning`（现状没有）。

---

### 2.5 `core` —— 门面

`include/core/core.hpp`：

```cpp
#pragma once

#include "core/entry.hpp"
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core {

class Core {
public:
    explicit Core(std::filesystem::path dataFile);   // 实现里就叫 dataFile

    // 启动时调一次。main 决定失败时要不要退。
    std::expected<void, std::string> load();

    // 上面 load 跳过的坏行说明
    const std::vector<std::string>& warnings() const;

    // 追加一条：生成 id，然后落盘。返回写好的 Entry。
    // created_at 由调用方传入（parse 的 base 必须和它同一个时刻）；
    // occurred 是解析结果，core 不认识 parse。
    std::expected<Entry, std::string> add(std::string_view        raw_text,
                                          std::chrono::sys_seconds created_at,
                                          std::optional<TimeRange> occurred = std::nullopt);

    // 按 created_at 倒序（新的在前）
    const std::vector<Entry>& list() const;

    std::optional<Entry> find(std::string_view id) const;

    std::expected<void, std::string> remove(std::string_view id);

private:
    std::filesystem::path    path_;
    std::vector<Entry>       entries_;
    std::vector<std::string> warnings_;
};

// 生成一个短 id（见下方说明）
std::string makeId(const std::vector<Entry>& existing);

}  // namespace core
```

**行为规格**：

- `add` —— `created_at = nowUtc()`，`raw_text` 原样存入（**不 trim、不改一个字**），
  `occurred` 留空、`tags` 空、`kind = Unknown`、`confirmed = false`。
  **先改内存再落盘**：`save` 失败就回滚内存（把刚加的那条弹出去），
  否则内存和文件不一致，下次启动数据就"丢"了。
- `list` —— `created_at` 倒序。同一秒内创建的多条，**用 id 做次级排序**保证顺序稳定。
  （`std::sort` 不稳定，不加次级键的话每次运行顺序可能不同。）
- `find` —— 精确匹配，没找到返回 `std::nullopt`
- `remove` —— 找到就删并落盘；没找到返回错误串 `"没有这条记录: <id>"`
  （实现里回滚还多保了一步：**记住被删元素原来的下标**，落盘失败时插回原位 ——
  直接 `push_back` 会把它挪到末尾，破坏"倒序"不变量。）

**`makeId` 怎么做** —— 这个决定有实际后果：

`note show <id>` 要**手输 id**。如果 id 是 36 字符的 UUID，你每次都得复制粘贴，
在终端里是灾难。所以要短。

```cpp
// 12 位十六进制 = 48 bit 随机
std::string makeId(const std::vector<Entry>& existing) {
    static std::mt19937_64 rng{std::random_device{}()};
    static std::uniform_int_distribution<std::uint64_t> dist;
    for (;;) {
        std::string id = std::format("{:012x}", dist(rng) & 0xFFFFFFFFFFFFull);
        const bool dup = std::any_of(existing.begin(), existing.end(),
                                     [&](const Entry& e) { return e.id == id; });
        if (!dup) return id;      // 撞了就重摇
    }
}
```

**为什么不用递增计数器**：计数器在"两台机器各记一段、以后合并文件"时会撞车，
而且删除中间某条后序号会断。随机 48 bit 在这两种情况下都不会出问题。

**为什么是 12 位而不是 8 位**：8 位 = 32 bit，记到 1 万条时撞车概率约 1.2%（生日问题），
偏高。12 位 = 48 bit，撞车概率可以忽略。12 个字符手输仍然可接受。

> `<random>`、`std::any_of`、lambda —— 这三个都是 M0 顺手能学到的。
> 那个 `for(;;)` 循环就是"撞了就重摇"，最简单的处理方式。

---

### 2.6 `main.cpp` —— CLI ⭐

#### ⚠️ 先说一个本机实测的坑：`main` 拿不到中文参数

**实测数据**（本机 Windows 11 中文版，控制台代码页 936）：

```
$ note.exe "中文测试"        # main(int, char** argv)
argv[1] 字节: D6 D0 CE C4 B2 E2 CA D4      ← 这是 GBK，不是 UTF-8！
```

`main` 的 `argv` 是 CRT 用**系统 ANSI 代码页**（中文 Windows 上是 GBK/936）
从命令行转出来的。GBK 字节喂给 `nlohmann::json`，因为不是合法 UTF-8，
`dump()` 会抛 `type_error`（错误码 316）——**你的程序连一条中文都存不进去**。

`SetConsoleOutputCP(CP_UTF8)` 管不了这个：它只影响**输出**，
不影响 CRT 对 `argv` 的转换。

**解法：用 `wmain` 拿宽字符参数，自己转成 UTF-8。**

```cpp
#include <windows.h>
#include <string>

// 宽字符 -> UTF-8
std::string toUtf8(const wchar_t* w) {
    if (!w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.resize(static_cast<std::size_t>(n) - 1);      // 去掉末尾的 '\0'
    return s;
}

int wmain(int argc, wchar_t** argv) { ... }
```

**实测结果**（同一台机器）：

```
$ note.exe "中文测试"        # wmain + toUtf8
UTF-8 字节: E4 B8 AD E6 96 87 E6 B5 8B E8 AF 95
rendered: [中文测试]        ← 正确
```

**两个实测结论**：
1. ✅ MSVC **会自动识别 `wmain`**，直接定义即可，**不需要**加 `/ENTRY:wmainCRTStartup`
   （实测：不写任何链接选项也能编过、能跑）
2. ✅ 转换后是标准 UTF-8，与 `nlohmann::json` 的严格模式兼容

`toUtf8` 里那个"先 `resize(n)` 再写"是有意的写法：`std::string::data()` 在 C++17
后返回可写指针，但**必须保证缓冲区真的有 `n` 个字节**。常见的"开 `n-1` 再写 `n` 个字节"
写法会越界。这个细节值得记住。

> 顺带：`wmain` 是 Windows 专有。将来真跨平台时，用
> `#ifdef _WIN32` 包住 `wmain` 和 `toUtf8`，非 Windows 走 `main` + 直传即可
> （Linux/macOS 的 `argv` 本来就是 UTF-8）。

#### CLI 规格

```
note add <文本...>      追加一条记录，参数用空格拼起来
note list               列出全部，按时间倒序
note show <id>          看一条的详情
note rm <id>            删一条
note help               帮助（无参数时也打这个）
```

示例：

```
$ note add 今天下午和小李讨论了方案
已记录 0193a4b2c7d1

$ note list
2026-10-08 23:30  0193a4b2c7d1  今天下午和小李讨论了方案

$ note show 0193a4b2c7d1
...

$ note rm 0193a4b2c7d1
已删除 0193a4b2c7d1
```

**退出码**（约定俗成，值得从第一天就对）：

| 码 | 含义 |
|---|---|
| `0` | 成功 |
| `1` | 用法错误（命令不认、参数少了） |
| `2` | 运行时错误（文件打不开、id 不存在） |

**实现要点**：

- `argv[1]` 是命令；`add` 时把 `argv[2..]` **用空格拼接**成一条文本
  （这样 `note add 今天 天气 不错` 和 `note add "今天 天气 不错"` 等价，符合直觉）
- 命令不认识 → 打帮助 + `return 1`
- `main` 只管**解析参数 → 调 core → 调 view**，**不写任何业务逻辑**
- `load()` 失败 → `v.RenderError(...)` + `return 2`（实际实现里变量就叫 `v`）
- `load()` 成功后如果有 warnings，**打出来但继续跑**（文件有坏行不该拦住你记事）

**✅ 两个现状提醒都已解决（2026-10-18）**：

1. ~~`parseTime` 还没接进来~~：`main.cpp` 的 `add` 分支先定 `created_at`，
   再 `parseTime(text, 本地墙上时间)`，解析结果换回 UTC 后交给 `core::add`。
   `occurred` 现在会落盘，`note show` 能看到。
2. ~~"用空格拼接"会咬人~~：`parseTime` 现在允许主体与后缀之间、以及「到」两边
   有空格 —— `note add 明天 下午3点` 和 `明天下午3点` 等价。
   跳过的空格算进被吃掉的区间，不进 `remainder`。

> ⚠️ 接线时发现的新坑（已处理）：`parseTime` 是时区无关的墙上时钟运算，
> 而 `occurred` 要存 UTC。直接拿 UTC 的 `created_at` 当 base，在 UTC+8 机器上
> 「今天」会变成昨天、「下午3点」会显示成晚上11点。CLI 层在解析前后各做一次
> `zoned_time` 转换（见 `main.cpp` 的 `utcToLocalWall` / `localWallToUtc`）。

---

### 2.7 显示本地时间

`created_at` 存 UTC，显示要转本地。这段 **已在本机编译跑通**（MSVC 14.44）：

```cpp
#include <chrono>
#include <format>

std::string toLocalDisplay(std::chrono::sys_seconds t) {
    const std::chrono::zoned_time zt{std::chrono::current_zone(), t};
    return std::format("{:%Y-%m-%d %H:%M}", zt);
}
```

实测输出 `2026-10-08 23:30`，时区 `GMT+8` 正确识别。
[DESIGN.md §8.2](../DESIGN.md) 标记的这个坑在本机**不存在**，M1 也可以直接用。

---

### 2.8 实现顺序（每步都能编译、能验证）

**别一次性写完再编。** 每一步结束都跑一次，错在哪立刻就知道了。

| 步 | 做什么 | 怎么验证 |
|---|---|---|
| **0** | 收掉 §2.0 那三处（补 include、加 namespace、删 `include/core.hpp`） | `cmake --build build --config Debug` 能过 |
| **1** | 写 `store` 的 JSON 转换 + `toIso`/`fromIso` | 写 `test_store.cpp` 的往返用例，`ctest -C Debug` 绿 |
| **2** | 写 `store::load` / `save`（含原子替换） | 把 §2.3 那 8 条规格全写成用例 |
| **3** | 写 `view.hpp` + `console_view` | 先写个临时的 `main` 塞几条假数据进去，肉眼看输出 |
| **4** | 写 `core.hpp` / `core.cpp` | 临时 `main` 里跑 `add` → `list` → 写进真文件 |
| **5** | 写真正的 `main.cpp`（`wmain` + 解析） | `note add 中文测试` 能存、`note list` 能看 |
| **6** | 删掉临时的假数据代码，全量走一遍验收清单 | §2.9 |

**第 1、2 步是重点。** 存储层是整个 M0 唯一"错了会静默丢数据"的地方，
其余层错了你一眼就看得出来。所以测试火力集中在它上面。

---

### 2.9 验收清单（2026-10-14 逐条实跑，结果如下）

M0 完成的定义——**每条都要真的做过**，不是"看起来对"：

- [x] `note add "中文测试"` 能存进去，`note list` 显示不乱码
- [x] 关掉程序再开，数据还在
- [x] 连续记 10 条，id 无重复（实测 10/10 唯一），倒序正确
      —— ⚠️ 注意：10 条落在**同一秒**内时，二级排序键是 id，所以列表**看起来是乱序**的
      （实测打印顺序：第1、第9、第3、第7…）。这是 §2.5 定的规则，不是 bug；
      真要"按输入顺序"就得再加个递增序号。
- [x] `note show <id>` 能看详情；`note show 不存在的id` → `错误: 没有这条记录: <id>`，退出码 **2**
- [x] `note rm <id>` 之后 list 里没有它了，文件里也没有
- [x] `note add` 不带文本 → 帮助 + 退出码 **1**（且不会创建数据文件）
- [x] `note 乱敲` → 帮助 + 退出码 **1**
- [x] 手工往 `notes.jsonl` 里插一行乱码 → 程序**照常启动**、跳过坏行、其余数据都在
      （⚠️ 但那条警告顶着 `错误: ` 打印，退出码仍是 0，见 §2.4 末尾）
- [x] `raw_text` 里含 `"`、`\`、换行、制表符 → 存了能读回来，一字不差
      （实测：JSON 里转义成 `\"`、`\\`、`\n`、`\t`，`note show` 原样打印）
- [x] 文件不存在时首次运行不报错（打印 `（还没有记录）`，退出码 0）
- [x] `ctest -C Debug` 全绿（1/1 passed）
- [ ] **连着用一周没丢过数据** ← 这一条最重要，也最花时间，**只有你能验**

最后一条是 DESIGN.md §5 定的验收标准。**别跳过它**——
一周的使用会暴露一堆测试里想不到的问题（手输 id 嫌烦、输出格式不顺手、想加个字段……），
那些才是 M1 真正该做的事。

---

### 2.10 这一程学到的 C++

| 概念 | 出现在哪 |
|---|---|
| 头文件 / 编译单元 / `#pragma once` | 所有 `.hpp` |
| `namespace` 与名字冲突 | §2.0 (b) |
| RAII（`ofstream` 析构自动关文件） | `store.cpp` |
| 值语义 vs 引用，`const&` 传参 | 所有接口签名 |
| `std::optional` —— "可能没有" | `find` / `occurred` |
| `std::expected` —— "可能失败，带原因" | 所有可能失败的接口 |
| 虚函数 / 多态 / 虚析构 | `View` |
| 纯虚接口 + 实现分离 | `View` / `ConsoleView` |
| lambda 与捕获 | `makeId` |
| STL 算法 `any_of` / `sort` | `makeId` / `list` |
| `<random>` 随机数引擎 | `makeId` |
| `<chrono>` 时区 | 显示层 |
| `std::filesystem` | 路径与文件操作 |
| 序列化 / ADL | `to_json` / `from_json` |
| ~~依赖注入（传 `ostream&` 进去）~~ | ❌ **没落地**：`ConsoleView` 直接写 `std::cout`，见 §2.4 |
| Windows 宽窄字符与 UTF-8 | `wmain` / `toUtf8` |

**15 个概念（原本 16 个，依赖注入那条没落地），M0 实际 ≈ 900 行**（含注释）。
这就是选 M0 做起点而不是先写 LLM 部分的原因。

---

### 2.11 现状与未完成清单（2026-10-14）

M0/M1 的功能都在，但下面这些"洞"还在。**按建议处理顺序排列**，每条都有实测依据：

| # | 事项 | 证据 / 影响 | 建议 |
|---|---|---|---|
| 1 | **`tests/test_store.cpp` 是空的** | 7 行注释、0 个用例；全项目 438 条断言全来自 parse；而存储层是唯一"错了会静默丢数据"的地方 | **最优先**。照 §2.3 那 8 条规格逐条写用例，先红后绿 |
| 2 | **`from_json(TimeRange)` 忽略 `fromIso` 返回值** | 坏数据静默变 1970-01-01，无 warning。`created_at` / `end` 都检查了，只有 `start` 漏了 | 一行的事：检查返回值并让 `load()` 收一条 warning |
| 3 | **`load()` 里 `exists(path, ec)` 的 `ec` 从未检查** | 权限/IO 错误被当成"文件不存在"→ 返回成功空列表，用户以为数据没了 | 判 `ec`，真错误走 `unexpected` |
| 4 | ~~**`parseTime` 没接进 `note add`**~~ | ✅ 2026-10-18 已接线：`note add` 现在会落 `occurred`，空格和时区转换一并处理 | — |
| 5 | **`include/core/view.hpp` 是死文件** | 旧版 `View` 残骸，全仓库无人 include，**而且没有虚析构**（正是 DESIGN.md §4.2 警告的那个坑） | 删掉。与 §2.0 (c) 的 `include/core.hpp` 同类 |
| 6 | **`git` 仓库零提交** | `git log` 报 "does not have any commits yet"，所有文件都是 untracked | **动手改代码前先 `git add -A && git commit`**，否则没有 diff、没法回滚 |
| 7 | **`view` 接口/命名与文档不一致** | `View` vs `IView`、`Render*` vs `show*`、没做 `ostream&` 注入 | 二选一：改代码 或 保留现状（本文已按现状改写，见 §0.3 / §2.4） |
| 8 | **`RenderError` 兼任 warning** | 坏行跳过会顶着 `错误: ` 打印，退出码却是 0 | 给接口加 `RenderWarning`，或明确接受 |
| 9 | ~~**`unitWords` / `timeWords` 两张死词表**~~ | ✅ 2026-10-17 已删除，全仓库 0 引用 | — |
| 10 | 仓库卫生 | `reesources/`（拼写错）、`save/`（空目录）、根目录 `vc.log`（构建日志，含 vswhere 报错） | 删 |

> 顺序的理由：**1–3 是"会静默坏数据"的**，4 是功能缺口，5–10 是卫生问题。
> 6 之所以排这么靠前，是因为没有 git 基线时，其余任何改动都没有退路。

---

## 3. M1 —— 时间解析 ✅ **已完成**（2026-10-18 接进 CLI）

**目标**：`note add "上周三 和小李讨论了方案"` → `occurred` 自动填上，且 `raw_text` 一个字不变。

**新增**：`include/parse/` + `src/parse/`（依赖方向见 [DESIGN.md §3](../DESIGN.md)）。

```cpp
namespace parse {

struct ParseResult {
    std::optional<core::TimeRange> occurred;
    std::string                    remainder;   // 抠掉时间词后剩下的文本
};

ParseResult parseTime(std::string_view text, std::chrono::sys_seconds base);

}  // namespace parse
```

**关键设计**：`base` 是**基准时刻**，必须显式传进来，**不要在函数内部取 `now()`**。

[DESIGN.md §8.4](../DESIGN.md) 定了"相对 `created_at`"。做成参数的好处：
测试时传一个固定的 `base`，结果完全可预测；否则测试依赖"现在几点"，半夜会随机挂。

**范围（✅ = 已实现，⬜ = 还没）**：
- 相对：今天 ✅ / 昨天 ✅ / 前天 ✅ / 明天 ✅ / 上周三 ✅ / 下周一 ✅ / 下个月初 ✅ / 上周末 ✅
  - ⬜ **`三天前` 这种"N 天前/后"没实现**（中文数字和阿拉伯数字都没做）
  - ➕ 实际还多做了：**大前天 / 大后天 / 下下周三 / 上上周三 / 今年 / 明年 / 去年**
- 绝对：`2026-10-01` ✅、`10月1日` ✅、`2026年10月1日` ✅
  - ➕ 多做了：**`今年3月5日`**（年前缀 + 月日）、**日期区间 `3月5日到3月7日` / `周一到周五`**
- 时段：`下午3点到5点` ✅、`上午` ✅、`昨晚` ✅
  - ➕ 多做了：**跨零点区间**（`晚上11点到凌晨1点` → 次日 01:00）
- 时刻：`下午3点` ✅、`15:30` ✅、`上午9点半` ✅
  - ➕ 修正了：**冒号形式也吃时段词修正**（`下午3:30` = `下午3点半`），以及 **12 点 / 0 点的边界**

**硬要求**：
- **解析不出就留空，绝不猜。** 宁可 `occurred` 为空，不要编一个时间。
  （[DESIGN.md §2.3 (c)](../DESIGN.md) + §8.3）
- ✅ 20 个手写用例全过 —— **实际 66 个 / 512 条断言**（2026-10-18 实跑），其中"应该解析失败"的用例有 7 组
- ⬜ 中文数字（"三点"、"十点半"）先不做，M1.5 再说 —— **仍未做**（与计划一致）

**验收**：✅ 66 个用例绿（远超 20 的要求）。
✅ `note add "上周三 和小李讨论了方案"` → `occurred` 自动填上，`raw_text` 一个字不变（2026-10-18 端到端实跑）。
⬜ 连续用一周，检查有没有"时间对了但文本被吃掉"（从 2026-10-18 接线后开始算）。
`remainder` 必须能还原出原文——如果时间词抠错了，宁可不动文本。

> 📖 **能解析什么、不能解析什么，看 [parse-time.md §3.5](parse-time.md)** ——
> 本文只列范围，那份是逐条核对过的速查表（含 12 点边界、区间沿用/回落、日期区间等规则）。

**学到**：`<chrono>` 日历运算（`year_month_day`、`weekday`）、字符串扫描、Unicode 边界。

---

## 4. M2 —— LLM 结构化

**目标**：自然语言 → `summary` / `tags` / `kind` 草稿，用户确认后写入。

**这一步才引入真正的依赖管理**：`cpr`（HTTP）经 vcpkg。
`DESIGN.md` §7 说了 M0–M1 不上 vcpkg，就是不想让它在早期卡住你。

**三条硬要求**（来自 [DESIGN.md §5 M2](../DESIGN.md) 与 §8.3）：

1. **永不自动入库。** 一律"生成草稿 → 用户确认 → 写入"，`confirmed` 标记是否确认过。
   LLM 的失败模式是**自信地错**，不报错，它编。事后你在时间轴里根本发现不了。
2. **先落 `raw_text`，后台异步调用。** 记录动作必须零延迟——
   写一条等 2 秒，你不会用第二次。这是产品存活问题，不是优化问题。
3. **优先本地 ollama。** 隐私 + 离线 + 免费。

**建议先做一件不涉及 LLM 的事**：把 M1 的 `parseTime` 结果做成"草稿"，
走一遍"生成草稿 → 确认 → 写入"的完整流程（`confirmed` 先只被时间解析用上）。
流程跑通了再换 LLM 填同样的位置。**这样 LLM 接不进来时系统仍然是可用的。**

**验收**：拔网线 / 关掉 ollama，`note add` 仍然零延迟、仍然能用，只是没有草稿。

**学到**：HTTP 客户端、异步 / 线程、超时与重试、JSON 请求体构造。

---

## 5. M3 —— 集合与查询

**目标**：标签作为集合，支持交 / 并 / 差筛选。

```
note list --tag 工作 --tag 项目A      # 交集 工作 ∩ 项目A
note list --any-tag 工作,读书         # 并集
note list --not-tag 琐事              # 差集
note list --since 2026-09-01
note list --until 2026-10-01
note list --kind event
note list --no-time                   # 只看 occurred 为空的
```

**新增**：`include/core/query.hpp` + `src/core/query.cpp`。

```cpp
namespace core {

struct Query {
    std::vector<std::string>   allTags;     // 交集
    std::vector<std::string>   anyTags;     // 并集
    std::vector<std::string>   noneTags;    // 差集
    std::optional<sys_seconds> since;
    std::optional<sys_seconds> until;
    std::optional<Kind>        kind;
};

std::vector<const Entry*> runQuery(const std::vector<Entry>& all, const Query& q);

}  // namespace core
```

**为什么返回 `const Entry*` 而不是 `Entry`**：不拷贝，只指向原容器里的元素。
M3 时正好讲清"指针 vs 值"——注意此时**调用方必须保证 `all` 还活着**。

**注意 [DESIGN.md §5 M3](../DESIGN.md) 的提醒**：值钱的是**集合这个数据模型**，
不是韦恩图那张图。标签会动态生长（一年后可能几百个），集合可视化在规模上必然失效。
主界面是**筛选器 + 时间轴**；韦恩图只做两三个标签的对比小视图。

**验收**：能回答"上个月跟项目A有关、但没提过工作的事有哪些"。
这个查询你自己每天都会想用，用不上说明做错了。

**学到**：`std::set_intersection` / `set_union` / `set_difference`、泛型谓词、lambda 捕获、
迭代器与生命周期。

---

## 6. M4 —— 按需演进

**不预先设计。** 等 M0–M3 真的用出问题了再动，且一次只动一个。

| 触发条件 | 动作 |
|---|---|
| 文件 > 5MB，或查询开始卡 | JSONL → SQLite（学习区，自己包一层） |
| 想要可视化 | 导出 Markdown（用现成编辑器看）或本地 HTTP 出 HTML |
| 想要第二个 UI | 这时才接 EUI-NEO——`View` 抽象第一次真正被用到 |
| 需要跨机器 | git 管 JSONL 文件，最省事 |

**仍然不做**：云同步、账号、插件、主题、移动端。
[DESIGN.md §6](../DESIGN.md) 列的那些不做的，除非被真实需求咬到，否则一律不做。

**个人项目最大的杀手是范围蔓延，不是技术难度。**

---

## 附：开工三句话

1. **先写 `test_store.cpp`，把它测绿。** M0 只有存储层错了会静默丢数据。
   → ⚠️ **这条实际没执行**（测试火力全压到了 parse 上，存储层至今 0 用例）。
   现在回头补还来得及，见 §2.11 第 1 条。
2. **每一步都编译一次。** 一周改一千行再编译，你会在几百个错误里淹死。
3. **M0 的目标是能用，不是完美。** 觉得该重构了——忍住，记下来，等一周后回头看还觉得该改再改。
