# nlohmann/json 接口速查

> 只覆盖**写 `store.cpp` 用得到的部分**，不是完整 API 参考。
> 完整文档：<https://json.nlohmann.me/api/basic_json/>
>
> 下面每条都对着本地这份 `third_party/nlohmann/json.hpp` 核实过，
> 带 ✅ 标注的代码片段在本机编译并运行过。

## 0. 前置信息

| 项 | 值 |
|---|---|
| 版本 | **3.11.3**（`third_party/nlohmann/json.hpp`，单头文件，24765 行） |
| 引入 | `#include <nlohmann/json.hpp>` |
| 命名空间 | `nlohmann::` |

---

## 1. 心智模型

`nlohmann::json` 是一个**能装任意 JSON 值的容器**，同时像 map 又像 vector：

```cpp
nlohmann::json j;
j["name"] = "张三";          // 像 map
j["tags"] = {"工作", "急"};   // 也能塞数组
j["nested"]["k"] = 1;        // 自动创建中间层
```

JSON 六种类型它都能表示：null / 布尔 / 数字 / 字符串 / 数组 / 对象。
判断当前类型用 `is_null()`、`is_string()`、`is_array()`、`is_object()`。

---

## 2. 三个核心动作

### 2.1 序列化 —— `dump()`

```cpp
std::string s = j.dump();
```

完整签名（已核实）：

```cpp
string_t dump(const int       indent        = -1,
              const char      indent_char   = ' ',
              const bool      ensure_ascii  = false,
              error_handler_t error_handler = error_handler_t::strict) const;
```

三个默认值正好都是你要的：

- `indent = -1` → 不换行不缩进 ← **JSONL 必需，别传 indent**
- `ensure_ascii = false` → 中文**原样输出**：写 `"你好"` 存进去就是 `"你好"` 两个汉字，
  **不是** `"\u4f60\u597d"` 那串转义（本项目的 JSONL 靠这条保持可读、可手工改）
- `error_handler = strict` → ⚠️ 遇到非法 UTF-8 **直接抛异常**，见 §6

### 2.2 反序列化 —— `parse()`

```cpp
nlohmann::json j = nlohmann::json::parse(line);
```

```cpp
static basic_json parse(InputType&& i,
                        const parser_callback_t cb = nullptr,
                        const bool allow_exceptions = true,
                        const bool ignore_comments = false);
```

`allow_exceptions = true`（默认）→ 解析失败抛 `parse_error`。**这是你要的行为**，不要关掉。

### 2.3 取值 —— 三种写法差别很大

| 写法 | 键不存在时 | 类型不对时 |
|---|---|---|
| `j["k"]` | ⚠️ **静默插入 null**（把对象改脏） | 返回引用 |
| `j.at("k")` | 抛 `out_of_range` | 抛 `type_error` |
| `j.value("k", def)` | 返回 `def` | 抛 `type_error` |

**读可能有缺字段的历史数据时，一律用 `value()`。** 详见 §8。

```cpp
int         n = j.value("count", 0);
std::string s = j.value("raw_text", std::string{});
```

其他常用：
- `j.contains("k")` —— 判断键是否存在
- `j.at("k").get<std::string>()` —— 显式转换

---

## 3. 自定义类型：`to_json` / `from_json`

**这是把 `Entry` 和 json 连起来的机制。** 不用继承、不用注册，靠 **ADL** 自动找到。

规则：在**类型所在的命名空间**里定义两个自由函数。

```cpp
struct Point { int x = 0; int y = 0; };

void to_json(nlohmann::json& j, const Point& p) {
    j = nlohmann::json{{"x", p.x}, {"y", p.y}};
}

void from_json(const nlohmann::json& j, Point& p) {
    p.x = j.at("x").get<int>();
    p.y = j.at("y").get<int>();
}
```

定义完后，这个类型就"变成"了 json：

```cpp
Point p{3, 4};

nlohmann::json j = p;          // 自动调 to_json
std::string   s = j.dump();    // {"x":3,"y":4}

Point q = j.get<Point>();      // 自动调 from_json
```

> ⚠️ **最常见的写错方式**：`to_json` 是**整体赋值** `j = ...`，
> 不是往传入的 `j` 里塞（`j["x"] = ...`）。后者会让旧键残留。

---

## 4. 本项目要处理的三种特殊情况

### 4.1 `std::optional<T>` —— 没有内置支持

**已核实：整个头文件里 `std::optional` 出现 0 次。** 得手写。

约定：**有值才写这个键，没值就不写。**

```cpp
// 写：有值才加进去
if (e.occurred) {
    j["occurred"] = *e.occurred;          // 注意解引用
}

// 读：缺键 和 显式 null 都当作"没有"
if (j.contains("occurred") && !j["occurred"].is_null()) {
    e.occurred = j["occurred"].get<TimeRange>();
}
```

### 4.2 时间类型 —— 也没有内置支持

**已核实：整个头文件里 `chrono` 出现 0 次。** 这个版本完全没有时间类型支持。

方案：用 **ISO 8601 字符串**存，**存 UTC**（[DESIGN.md](../DESIGN.md) §2.2）。

下面这段 ✅ **已在本机编译并跑通**（MSVC 14.44 / C++17 起可用 `std::format`，
本项目按 C++23 编译）。**`fromIso` 用的是项目里的实际版本** ——
先解析到局部变量，成功才写回 `out`，失败绝不改动调用方的值
（`store.hpp` 的注释承诺"失败返回 false，不改 out"，直接往 `out` 里 `from_stream` 做不到这一点）：

```cpp
#include <chrono>
#include <format>
#include <sstream>
#include <string_view>

// 序列化：sys_seconds -> "2025-10-08T14:00:00Z"
std::string toIso(std::chrono::sys_seconds t) {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", t);
}

// 反序列化：失败时返回 false，且不动 out
bool fromIso(std::string_view s, std::chrono::sys_seconds& out) {
    std::istringstream       in{std::string{s}};
    std::chrono::sys_seconds parsed{};

    std::chrono::from_stream(in, "%Y-%m-%dT%H:%M:%SZ", parsed);
    if (in.fail()) {
        return false;
    }
    out = parsed;
    return true;
}
```

实测输出：

```
format    -> [2025-10-08T14:00:00Z]
parse     -> ok
roundtrip -> IDENTICAL            （往返一致）
bad input -> correctly rejected   （非法输入会被识别出来）
```

> ⚠️ **调用方必须看返回值**。`src/core/store.cpp` 的 `from_json(TimeRange)` 里
> `fromIso(j.value("start", ...), t.start);` **把返回值丢了** —— 于是
> `{"occurred":{"start":"garbage"}}` 会静默变成 1970-01-01（同一个函数里 `end` 和
> `created_at` 都检查了，只有 `start` 漏了）。见 [ROADMAP.md §2.3](ROADMAP.md) 的现状框。

### 4.3 枚举 —— 存字符串，不存数字

```cpp
{"kind", "event"}     // ✅
{"kind", 1}           // ❌
```

**为什么**：存数字的话，哪天你往 `Kind` 中间插一个枚举值，
**所有历史数据的 kind 全部错位，而且不会报错**。存字符串就没这个问题。

代价是稍微占空间，对个人笔记可以忽略。

---

## 5. 键的顺序：默认会被重排 ⚠️

**已核实**：默认的 `nlohmann::json` 底层是 `std::map`（`basic_json` 模板的 `ObjectType` 默认值），
所以**键按字母序输出，不是插入顺序**。

```cpp
nlohmann::json j;
j["zzz"] = 1;
j["aaa"] = 2;
j.dump();          // {"aaa":2,"zzz":1}   ← 顺序被换成了字母序
```

要保持插入顺序，用 **`nlohmann::ordered_json`**（已核实存在）：

```cpp
nlohmann::ordered_json j;
```

**本项目建议**：先**不引入**。JSONL 是给你自己看和手工改的，固定字段顺序确实更舒服，
但不值得现在多一个概念——等真觉得别扭时再换，**只改类型名**即可（[DESIGN.md](../DESIGN.md) §6 的 YAGNI）。

---

## 6. UTF-8 陷阱 ⚠️

`dump()` 的 `error_handler` 默认是 **strict**：**输入不是合法 UTF-8 就抛 `type_error`（错误码 316）**。

对你意味着：

- **中文本身没问题** —— `"你好"` 是合法 UTF-8，原样存进去、原样读出来
- **危险的是从控制台读进来的输入**。如果控制台给的是 GBK 字节，`dump()` 会炸

> ⚠️ **本机实测的一条更正（2026-10-14）**：这里原本写着
> "`SetConsoleOutputCP(CP_UTF8)` 保证了输入也是合法 UTF-8" —— **那是错的**。
>
> `SetConsoleOutputCP` 只影响**输出**，**管不了 CRT 对 `argv` 的转换**。
> 实测：`main(int, char** argv)` 拿到的中文参数是 GBK 字节（`中文测试` 为
> `D6 D0 CE C4 B2 E2 CA D4`），喂给 `dump()` 直接抛 316。
>
> 项目里的对策是**改用 `wmain` 拿宽字符再自己转 UTF-8**，
> 见 [main.cpp](../main.cpp) 的 `toUtf8` 与 [ROADMAP.md §2.6](ROADMAP.md)。
> `SetConsoleOutputCP(CP_UTF8)` 仍然要有 —— 但它的职责只是**输出不乱码**。

真遇到 316 时，`error_handler_t::replace` 可以容错（非法字节换成 `U+FFFD`），
但那是掩盖问题，**先查编码来源**。

---

## 7. 错误处理

所有异常都继承自 `nlohmann::json::exception`：

```cpp
try {
    nlohmann::json j = nlohmann::json::parse(line);
    Entry e = j.get<Entry>();
} catch (const nlohmann::json::parse_error& e) {
    // JSON 语法错误 —— 这一行文件损坏；e.what() 含字符偏移量，方便定位
} catch (const nlohmann::json::type_error& e) {
    // 类型不对，或 UTF-8 非法（错误码 316）
} catch (const nlohmann::json::out_of_range& e) {
    // at() 找不到键
}
```

**读 JSONL 的建议策略**：一行坏了不该让整个文件读不出来。
记录错误、跳过这一行、继续读——个人数据文件被手工编辑过是很正常的事。

---

## 8. JSONL 的具体写法

```cpp
// ---- 写：一行一条 ----
std::ofstream out(path, std::ios::binary);
out << j.dump() << '\n';        // 不传 indent！JSONL 必须一行一条

// ---- 读：逐行解析 ----
std::ifstream in(path, std::ios::binary);
std::string line;
while (std::getline(in, line)) {
    if (line.empty()) continue; // 跳过空行（文件末尾常有）
    // ... parse(line)
}
```

> ⚠️ **本项目的实际写入方式不是"追加"**：`store::save()` 走的是
> **原子重写** —— 先整个写进 `path.tmp`，再 `rename` 覆盖正式文件
> （理由与 Windows 上的坑见 [ROADMAP.md §2.3](ROADMAP.md)）。
> 上面 `std::ios::app` 那种追加写法**本版没有采用**，
> 因为"删一条 / 改一条"都要求重写整个文件，而写一半崩掉会毁掉全部数据。

**两个细节**：

- **`std::ios::binary`** —— Windows 文本模式会把 `\n` 悄悄写成 `\r\n`。
  用 binary 避免这种隐式转换，文件内容完全可控。
- **空行必须跳过**，否则 `parse("")` 抛异常。
  （`store::load()` 还额外剥了一次行尾的 `'\r'`，这样 CRLF 文件也能读。）

**向前兼容的读法** —— 以后给 `Entry` 加字段时用带默认值的 `value()`，旧数据不会读失败：

```cpp
e.summary   = j.value("summary", std::string{});   // 旧数据没这个键 → 空串
e.confirmed = j.value("confirmed", false);         // 旧数据没这个键 → false
```

**实际落盘的键序**（`nlohmann::json` 底层是 `std::map`，所以是**字母序**，不是代码里的书写顺序）：

```json
{"confirmed":false,"created_at":"2026-10-14T14:58:24Z","id":"2110abc89bb6","kind":"unknown","raw_text":"上周三下午和小李讨论了方案","summary":"","tags":[]}
```

（`occurred` 键只在有值时才出现 —— 这就是 §4.1 说的 optional 约定。）

---

## 9. 坑清单

| # | 坑 | 后果 | 对策 |
|---|---|---|---|
| 1 | 用 `j["k"]` 读不存在的键 | **静默插入 null**，对象被改脏 | 读用 `at()` 或 `value()` |
| 2 | `to_json` 写成 `j["x"] = p.x` | 键残留旧值 | 改成整体赋值 `j = json{...}` |
| 3 | `dump()` 传了 indent | 输出带换行，JSONL 直接坏掉 | `dump()` 不传参数 |
| 4 | 非法 UTF-8 | 抛 `type_error` 316 | 保证输入是 UTF-8（见 §6） |
| 5 | 以为键保持插入序 | 文件顺序和你写的不一样 | 用 `ordered_json`（暂不需要） |
| 6 | 枚举存数字 | 以后加枚举值 → 历史数据错位且不报错 | **存字符串** |
| 7 | 读文件没跳空行 | `parse("")` 抛异常 | `if (line.empty()) continue;` |
| 8 | 在 **`const` json** 上用 `j["k"]` 且键不存在 | **未定义行为**（只有非 const 版才会静默插入 `null`） | 先 `contains()` 再取，或改用 `at()` / `value()` |

> 第 8 条是本项目的实际写法要注意的：`from_json` 收到的是 `const nlohmann::json&`，
> 所以那两处 `j["occurred"]` / `j["end"]` **都先用 `contains()` 挡住了**才取。

---

## 10. 完整最小示例

（toy 类型，只演示机制；你的 `Entry` 照这个模式写）

```cpp
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

struct Tag {
    std::string          name;
    std::optional<int>   weight;
};

void to_json(nlohmann::json& j, const Tag& t) {
    j = nlohmann::json{{"name", t.name}};
    if (t.weight) j["weight"] = *t.weight;      // optional：有值才写
}

void from_json(const nlohmann::json& j, Tag& t) {
    t.name = j.value("name", std::string{});    // 缺键用默认值
    if (j.contains("weight") && !j["weight"].is_null())
        t.weight = j["weight"].get<int>();
}

// ---- 使用 ----
Tag t{"工作", 3};
nlohmann::json j = t;
std::string line = j.dump();                       // {"name":"工作","weight":3}

Tag back = nlohmann::json::parse(line).get<Tag>(); // 往返回来
```

---

**参考**：完整 API 文档 <https://json.nlohmann.me/api/basic_json/>
