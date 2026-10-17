// note —— 命令行入口。
//
// 这个文件只做三件事：**解析参数 → 调 core → 调 view**。
// 不写任何业务逻辑（"这条记录该怎么存"是 core 的事，
// "该怎么显示"是 view 的事）。依赖方向见 docs/ROADMAP.md §0.7。
//
// 规格见 docs/ROADMAP.md §2.6。

#include "core/core.hpp"
#include "parse/timeParse.hpp"
#include "view/console_view.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
# include <windows.h>
#endif

namespace
{

#ifdef _WIN32
// 宽字符 -> UTF-8。
//
// ⚠️ 这个函数存在的原因（本机实测，见 docs/ROADMAP.md §2.6）：
// main(int, char**) 里的 argv 是 CRT 用**系统 ANSI 代码页**转出来的，
// 中文 Windows 上是 GBK。"中文测试" 拿到的字节是 D6 D0 CE C4 B2 E2 CA D4，
// 不是合法 UTF-8 —— 喂给 nlohmann 的 dump() 会抛 type_error（错误码 316），
// 结果是**连一条中文都存不进去**。
//
// SetConsoleOutputCP 管不了这个：它只影响输出，不影响 CRT 对 argv 的转换。
// 所以要换 wmain 拿宽字符，自己转成 UTF-8。
std::string toUtf8(const wchar_t* w)
{
    if (!w)
    {
        return {};
    }

    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
    {
        return {};
    }

    // ⚠️ 注意"先 resize(n) 再写"这个写法。
    // data() 在 C++17 后返回可写指针，但**缓冲区必须真的有 n 个字节**。
    // 常见的"开 n-1 个再写 n 个字节"会越界写。多开一个、写完再缩回去才是对的。
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.resize(static_cast<std::size_t>(n) - 1);   // 去掉末尾的 '\0'

    return s;
}
#endif

// 数据文件位置：环境变量 NOTE_FILE > 当前目录的 notes.jsonl（ROADMAP §0.6）。
std::filesystem::path dataPath()
{
    if (const char* env = std::getenv("NOTE_FILE"))
    {
        return env;
    }
    return "notes.jsonl";
}

// ---------------------------------------------------------------------------
// 本地墙上时间 <-> UTC
//
// parseTime 是时区无关的：它把 base 当"墙上时钟"做日历运算，结果的每个字段
// 也还是墙上时钟。可 Entry 里存的是 UTC（ROADMAP §0.5）。所以 CLI 这一层
// 必须做两次转换：
//
//     解析前：UTC(created_at)  -> 本地墙上时间          当 base
//     解析后：本地墙上时间      -> UTC                  落盘
//
// 不做的话，UTC+8 机器上「今天」会变成昨天，「下午3点」显示成晚上11点。
// ---------------------------------------------------------------------------

std::chrono::sys_seconds nowUtc()
{
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

// UTC -> 本地墙上时间。返回值类型仍是 sys_seconds，但它的"值"是墙上时钟
// （比如本地 15:00 就是 15:00），只用来喂 parseTime，不要当时间点存。
std::chrono::sys_seconds utcToLocalWall(std::chrono::sys_seconds utc)
{
    const std::chrono::zoned_time zt{std::chrono::current_zone(), utc};
    return std::chrono::sys_seconds{zt.get_local_time().time_since_epoch()};
}

// 本地墙上时间 -> UTC。
//
// 用 get_info 而不是 zoned_time 的构造函数：后者碰到夏令时"跳过"的时刻
// （gap）会抛 nonexistent_local_time，一条笔记不该因为这种边界存不进去。
//
// local_info 的三种结果：
//   unique      -> first.offset
//   nonexistent -> 取 second.offset（跳变**之后**的偏移，标准推荐的做法）
//   ambiguous   -> 取 first.offset（跳变**之前**的偏移，也就是较早的那个时刻）
std::chrono::sys_seconds localWallToUtc(std::chrono::sys_seconds wall)
{
    const std::chrono::local_time<std::chrono::seconds> local{wall.time_since_epoch()};
    const std::chrono::local_info info = std::chrono::current_zone()->get_info(local);

    const auto offset = info.result == std::chrono::local_info::nonexistent
                            ? info.second.offset
                            : info.first.offset;
    return wall - offset;
}

void printHelp(std::ostream& os)
{
    os << "用法:\n"
          "  note add <文本...>    追加一条记录\n"
          "  note list             列出全部（按时间倒序）\n"
          "  note show <id>        看一条的详情\n"
          "  note rm <id>          删一条\n"
          "  note help             显示这个帮助\n";
}

// 真正的逻辑。跟 wmain/main 分开，是为了让上面那两个只负责"把参数转成 UTF-8"。
int run(const std::vector<std::string>& args, view::View& v)
{
    // args[0] 是程序自己的名字
    if (args.size() < 2)
    {
        printHelp(std::cout);
        return 1;   // 用法错误
    }

    const std::string& cmd = args[1];

    // ---- 先做纯参数检查，别为了一个用法错误去读磁盘 ----

    if (cmd == "help")
    {
        printHelp(std::cout);
        return 0;
    }

    if (cmd != "add" && cmd != "list" && cmd != "show" && cmd != "rm")
    {
        // 命令不认识
        printHelp(std::cout);
        return 1;
    }

    const bool needsId = (cmd == "show" || cmd == "rm");
    if ((cmd == "add" || needsId) && args.size() < 3)
    {
        printHelp(std::cout);
        return 1;
    }

    // ---- 参数合法了，才开始读盘 ----

    core::Core note{dataPath()};

    if (auto loaded = note.load(); !loaded)
    {
        v.RenderError("读取数据文件失败: " + loaded.error());
        return 2;   // 运行时错误
    }

    // 坏行只警告、不拦人 —— 文件被手工编辑过是很正常的事（ROADMAP §2.6）。
    for (const auto& w : note.warnings())
    {
        v.RenderError(w);
    }

    // ---- 分发 ----

    if (cmd == "add")
    {
        // 把 args[2..] 用空格拼起来一条文本。
        // 这样 `note add 今天 天气 不错` 和 `note add "今天 天气 不错"` 等价，符合直觉。
        std::string text;
        for (std::size_t i = 2; i < args.size(); ++i)
        {
            if (i > 2)
            {
                text += ' ';
            }
            text += args[i];
        }

        // created_at 先定下来，再解析 —— 「上周三」相对的就是它。
        // 顺序反过来（先 add 再 parse），两次 now() 之间可能已经跨了一天。
        const auto createdAt = nowUtc();

        // parseTime 吃的是墙上时钟，所以 base 也要换成本地墙上时间。
        const auto parsed = parse::parseTime(text, utcToLocalWall(createdAt));

        // 解析结果是墙上时钟，落盘前换回 UTC。
        std::optional<core::TimeRange> occurred;
        if (parsed.occurred)
        {
            occurred = core::TimeRange{
                localWallToUtc(parsed.occurred->start),
                parsed.occurred->end
                    ? std::optional<std::chrono::sys_seconds>{localWallToUtc(*parsed.occurred->end)}
                    : std::nullopt};
        }

        auto added = note.add(text, createdAt, occurred);
        if (!added)
        {
            v.RenderError("保存失败: " + added.error());
            return 2;
        }

        v.RenderMessage("已记录 " + added->id);
        return 0;
    }

    if (cmd == "list")
    {
        v.RenderView(note.list());
        return 0;
    }

    if (cmd == "show")
    {
        if (auto found = note.find(args[2]))
        {
            v.RenderEntry(*found);
            return 0;
        }
        v.RenderError("没有这条记录: " + args[2]);
        return 2;
    }

    // cmd == "rm"
    if (auto removed = note.remove(args[2]); !removed)
    {
        v.RenderError(removed.error());
        return 2;
    }
    v.RenderMessage("已删除 " + args[2]);
    return 0;
}

}   // namespace

#ifdef _WIN32

int wmain(int argc, wchar_t** wargv)
{
    // 控制台默认不是 UTF-8，不设这行中文输出会变乱码（DESIGN.md §8.1）。
    ::SetConsoleOutputCP(CP_UTF8);

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i)
    {
        args.push_back(toUtf8(wargv[i]));
    }

    view::ConsoleView console;
    return run(args, console);
}

#else

int main(int argc, char** argv)
{
    // 非 Windows 的 argv 本来就是 UTF-8，直接用，不需要转换。
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i)
    {
        args.push_back(argv[i]);
    }

    view::ConsoleView console;
    return run(args, console);
}

#endif
