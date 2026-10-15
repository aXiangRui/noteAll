# 程序从哪开始跑 —— `main` / `wmain` / CRT 入门

> 这份文档解决一个困惑：
> **为什么 `main.cpp` 里写的不是 `main` 而是 `wmain`，还能编译？C++ 不是规定必须有 `main` 吗？**
>
> 答案会颠覆一个常见的直觉：**`main` 从来就不是程序的第一条指令。**
> 从零讲起，不需要任何前置知识。

---

## 0. 先看一个事实

在命令行敲下 `note add 你好` 然后回车。接下来这几毫秒里发生的事：

```
你按回车
   ↓
Windows 把 note.exe 从硬盘读进内存
   ↓
CPU 跳到一个地址，开始执行机器指令      ← 这里！会跳到哪一行？
   ↓
（一堆你看不见的准备工作）
   ↓
你写的 wmain() 被执行
   ↓
程序结束
```

**中间那个箭头，跳到的是哪一行？** 直觉上会答"`main` 的第一行"。不对。

---

## 1. exe 里存了什么

一个 `.exe` 文件本质上是两样东西：

1. **一堆机器指令**（你写的代码编译成的）
2. **一个"目录"**，记着这堆东西怎么摆放、从哪开始执行

这个目录叫 **PE 头**（Portable Executable header）。它是 Windows 定死的文件格式，链接器负责填。

PE 头里有个字段叫 **入口点（Entry Point）**，记着一个地址，意思是：

> "操作系统啊，把这个 exe 加载完之后，从**这个地址**开始执行。"

### 看实际的例子

用 `dumpbin` 工具能把 PE 头打印出来（VS Build Tools 自带）：

```bash
dumpbin /headers build/bin/Debug/note.exe
```

输出里有一行：

```
3FB7 entry point (0000000140003FB7) @ILT+12210(wmainCRTStartup)
```

翻译一下：**这个 exe 的入口点是一个叫 `wmainCRTStartup` 的函数。**

注意这个名字——**既不是 `main`，也不是 `wmain`**。它后面挂着 `CRTStartup` 的后缀。

（`@ILT+12210` 是"间接跳转表"里的位置，是实现细节，忽略即可。关键是括号里那个名字。）

---

## 2. 真正的入口：CRT 启动代码

**CRT = C Runtime（C 运行期库）**。它是编译器厂商提供的一个库，MSVC 自带，你每次编译都自动链进来。

CRT 里有一组函数，专门负责"程序启动前的准备工作"，名字都以 `CRTStartup` 结尾：

| 函数名 | 什么时候被选为入口 |
|---|---|
| `mainCRTStartup` | 你定义的是 `main` |
| `wmainCRTStartup` | 你定义的是 `wmain` |
| `WinMainCRTStartup` | 你定义的是 `WinMain`（图形界面程序） |
| `wWinMainCRTStartup` | 你定义的是 `wWinMain` |

**这些才是真正的入口。** 操作系统跳到它们，它们做完准备工作，**再调用你的函数**。

### 完整的链条

```
系统加载 note.exe
      ↓  读 PE 头，跳到入口点
  wmainCRTStartup          ← 真正的第一条指令，CRT 提供
      ↓  ① 建立堆（new / malloc 能用的前提）
      ↓  ② 初始化 C 运行期
      ↓  ③ 构造全局对象、跑全局变量的构造函数
      ↓  ④ 从系统拿到命令行的原始字节
      ↓  ⑤ 按当前代码页把它转成窄字符串 → argv
     wmain(argc, argv)      ← 你写的函数，在这里才被调用
      ↓  return 0
  CRT 收尾（刷新缓冲区、跑全局析构、返回退出码）
      ↓
  程序结束
```

**所以 `main` 是被"调用"的，不是"入口"。** 它只是 CRT 启动代码调用链里的一个环节。

### 一个证据

如果你在 `main` 的第一行设断点调试，看调用栈（Call Stack），会看到：

```
main.cpp
  ↑
wmainCRTStartup
  ↑
... 更底层的系统代码
```

`wmainCRTStartup` 就在你下面那一层。

---

## 3. 那链接器怎么知道该调用哪个？

你只写了 `wmain` 一个函数。链接器是怎么知道要拉进 `wmainCRTStartup`（而不是 `mainCRTStartup`）的？

编译分两步：

```
   .cpp 文件
      ↓  编译
   .obj 文件（机器指令 + 一张"我需要这些符号"的清单）
      ↓  链接
   .exe
```

链接时会干这件事：**在你的 `.obj` 和库里找符号**。

不指定 `/ENTRY` 选项时，链接器按这个规则挑：

| 子系统 | 它去找 | 找到就用 |
|---|---|---|
| `/SUBSYSTEM:CONSOLE`（命令行程序，默认） | `main` 或 `wmain` | `mainCRTStartup` 或 `wmainCRTStartup` |
| `/SUBSYSTEM:WINDOWS`（图形程序） | `WinMain` 或 `wWinMain` | `WinMainCRTStartup` 或 `wWinMainCRTStartup` |

我们这个项目是命令行程序（console 子系统），你定义了 `wmain`，链接器找到了它，就自动拉进 `wmainCRTStartup`，并且**把这个地址写进 PE 头的入口点字段**。

这就是为什么[ROADMAP §2.6](ROADMAP.md)记着这条实测结论：

> ✅ MSVC **会自动识别 `wmain`**，直接定义即可，**不需要**加 `/ENTRY:wmainCRTStartup`

不是它"知道你想干什么"，而是**默认规则正好把它选对了**。

（两个都定义会打架，别同时写 `main` 和 `wmain`。）

---

## 4. `main` 和 `wmain` 的区别：一个 w

只差一个字母，区别在**参数类型**：

```cpp
int  main(int argc, char**  argv);            // 窄字符
int wmain(int argc, wchar_t** wargv);         // 宽字符（wide）
```

| | `char` | `wchar_t` |
|---|---|---|
| 大小（Windows） | 1 字节 | **2 字节** |
| 能直接表示中文吗 | 看编码 | 可以 |

`w` 就是 **wide**（宽）的意思。

### 那为什么不一上来就用 `wmain`？

因为 `wmain` **不是 C++ 标准的一部分**——它只存在于 Windows/MSVC（见下面第 6 节）。标准里只写了 `main`。

非 Windows 平台（Linux、macOS）上根本没有 `wmain` 这个东西，所以那种地方只能写标准的 `main`。

---

## 5. 为什么这个项目非要用 `wmain` —— 中文的坑

这才是我们真正的动机。**不是"想用"，是"不得不用"。**

### 问题：`main` 拿到的中文是坏的

在中文 Windows 上，命令行里打的中文，交给 `main` 的 `argv` 时**不是 UTF-8**。

实测（[ROADMAP §2.6](ROADMAP.md) 有完整数据）：

```bash
$ note.exe "中文测试"        # 用 main(int, char**)
argv[1] 的字节: D6 D0 CE C4 B2 E2 CA D4      ← 这是什么？
```

`D6 D0 CE C4 B2 E2 CA D4` —— 这是 **GBK/CP936**，中文 Windows 的默认编码，**不是 UTF-8**。

### 为什么这是灾难

我们的数据要用 `nlohmann::json` 存成 JSONL 文件。JSON 标准规定**文本必须是 UTF-8**，nlohmann 严格照办——给它喂非法 UTF-8，它**直接抛异常**：

```
type_error 316
```

结果就是：**你一条中文都存不进去。** 程序看起来能跑，一打中文就崩。

### 为什么 `SetConsoleOutputCP(CP_UTF8)` 救不了

很多人第一反应是加这一行：

```cpp
::SetConsoleOutputCP(CP_UTF8);
```

**没用。** 这行只影响**输出**（你 `cout` 打出去的东西怎么显示），完全不影响**输入**——不影响 CRT 把命令行转成 `argv` 的过程。

「输出」和「输入」是两条独立的路，改一条不影响另一条。

### 解法：换 `wmain`，自己转

`wmain` 给的是 **UTF-16 宽字符**，是**确定的、没有歧义**的。然后我们用一个 Windows API 手工转成 UTF-8：

```cpp
std::string toUtf8(const wchar_t* w)
{
    // 第一次调用：问"转成 UTF-8 需要多少字节"
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);

    // 开足够的空间（注意不是 n-1）
    std::string s(static_cast<std::size_t>(n), '\0');

    // 第二次调用：真正转换，写进 s
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);

    s.resize(static_cast<std::size_t>(n) - 1);   // 去掉末尾的 '\0'
    return s;
}
```

转换之后：

```
E4 B8 AD E6 96 87 E6 B5 8B E8 AF 95        ← 标准 UTF-8 ✅
```

现在 nlohmann 满意了，中文能存了。

> 这个"先问大小、再开空间、写第二次"的两步套路，是 Windows 上所有 `...W` / `...A` API 的通用模式，值得记住。
>
> ⚠️ 里面那个 `resize(n)` 而不是 `resize(n-1)` 是个容易写错的细节，[ROADMAP §2.6](ROADMAP.md) 有说明。

---

## 6. 代价：`wmain` 不是标准 C++

| | 标准吗 | 谁认 |
|---|---|---|
| `main` | ✅ C++ 标准规定 | 所有编译器 |
| `wmain` | ❌ 微软扩展 | 只有 MSVC |

在 Linux 上用 GCC 编译一个只有 `wmain` 的程序，会报：

```
undefined reference to `main'
```

所以我们用条件编译**两条路都留着**：

```cpp
#ifdef _WIN32

int wmain(int argc, wchar_t** wargv)      // Windows 走这条
{
    ::SetConsoleOutputCP(CP_UTF8);
    // ...用 toUtf8 转换 argv...
}

#else

int main(int argc, char** argv)           // 其他平台走这条
{
    // 非 Windows 的 argv 本来就是 UTF-8，不用转换，直接收下
    // ...
}

#endif
```

`#ifdef _WIN32` 的意思是"只有 Windows 平台才编译这一段"。这样代码换个平台也能编过。

**为什么非 Windows 不需要转换**：Linux/macOS 的命令行本来就是 UTF-8 字节流，`main` 拿到的就是对的。这个坑是 Windows 独有的（因为 Windows 历史上用过 GBK 这类本地代码页）。

---

## 7. 对照本项目代码

打开 [main.cpp](../main.cpp)，从上往下：

| 位置 | 是什么 |
|---|---|
| `#ifdef _WIN32` / `#include <windows.h>` | 只为 Windows 编译的准备工作 |
| `toUtf8()` | UTF-16 → UTF-8 的转换 |
| `dataPath()` | 决定数据文件放哪（`NOTE_FILE` 环境变量 > `notes.jsonl`） |
| `printHelp()` | 打印用法 |
| `run()` | **真正的逻辑**：解析参数 → 调 core → 调 view |
| 文件末尾的 `wmain` / `main` | 入口。**只负责一件事**：把参数转成 UTF-8，然后交给 `run()` |

注意这个划分：

```cpp
int wmain(int argc, wchar_t** wargv)    // 入口：只管"把参数转成 UTF-8"
{
    ::SetConsoleOutputCP(CP_UTF8);

    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back(toUtf8(wargv[i]));

    view::ConsoleView console;
    return run(args, console);           // 逻辑全在这里面
}
```

`run()` 接收的是统一的 `std::vector<std::string>`，**它不知道外面是 `wmain` 还是 `main`**。这样两个平台的入口都能共享同一份逻辑，`#ifdef` 被限制在最外层的一小圈里。

---

## 8. 一页总结

1. **`main` 不是程序入口。** 真正的入口是 CRT 的 `mainCRTStartup` / `wmainCRTStartup` 之类。
2. **PE 头里记着入口点地址**，链接器决定填哪个。`dumpbin /headers xxx.exe` 能看出来。
3. **链接器按子系统自动挑**：console 下找 `main` / `wmain`，找到哪个用哪个对应的启动代码。
4. **`wmain` 是微软扩展**，标准里只有 `main`。所以要用 `#ifdef _WIN32` 分开写。
5. **这个项目必须用 `wmain`**，因为中文 Windows 上 `main` 的 `argv` 是 GBK 不是 UTF-8，喂给 nlohmann 会抛异常，一条中文都存不进去。
6. **`SetConsoleOutputCP` 只管输出不管输入**，救不了这个问题。

### 一图流

```
        ┌─────────────────────────────────────────┐
        │  PE 头：entry point = wmainCRTStartup   │
        └─────────────────────────────────────────┘
                          ↓ 操作系统跳到这
        ┌─────────────────────────────────────────┐
        │  wmainCRTStartup   （CRT 提供）         │
        │    建堆 / 初始化运行期 / 构造全局对象    │
        │    拿到命令行原始字节，按代码页转成 argv │
        └─────────────────────────────────────────┘
                          ↓ 调用
        ┌─────────────────────────────────────────┐
        │  wmain(argc, wargv)   （你写的）        │
        │    SetConsoleOutputCP(CP_UTF8)          │
        │    toUtf8(wargv[i])  →  std::vector     │
        │    return run(args, console)            │
        └─────────────────────────────────────────┘
                          ↓
        ┌─────────────────────────────────────────┐
        │  run()  —— 解析参数 → 调 core → 调 view │
        └─────────────────────────────────────────┘
```

---

## 附：想自己动手看

```bash
# 看自己程序的入口点
dumpbin /headers build/bin/Debug/note.exe

# 在输出里找这一行，括号里就是真正的入口函数名
#   3FB7 entry point (0000000140003FB7) @ILT+12210(wmainCRTStartup)
```

（在 Git Bash 里 `dumpbin /headers` 会被当成路径，要写成 `dumpbin //headers`。在 CMD 或 PowerShell 里直接写 `/headers` 即可。）
