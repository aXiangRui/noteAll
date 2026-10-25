// note —— 命令行入口。
//
// 这个文件只做三件事：**解析参数 → 调 core → 调 view**。
// 不写任何业务逻辑（"这条记录该怎么存"是 core 的事，
// "该怎么显示"是 view 的事）。依赖方向见 docs/ROADMAP.md §0.7。
//
// 两种用法：
//   note add ...        一次性命令（脚本 / 终端里用）
//   note                不带参数 = 双击启动，进交互模式（否则窗口一闪就关，像闪退）
//
// 规格见 docs/ROADMAP.md §2.6。

#include "core/core.hpp"
#include "core/note_log.hpp"
#include "parse/timeParse.hpp"
#include "view/console_view.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
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

/**
 * 从标准输入读一行，转成 UTF-8。
 *
 * 为什么不用 std::getline：
 *   控制台的输入代码页默认也不是 UTF-8（中文 Windows 是 GBK），
 *   直接读字节会把中文输入弄成乱码 —— 而"记中文"是这个程序的全部意义。
 *   控制台用 ReadConsoleW 拿宽字符再转；管道/重定向（脚本里 echo 进来）本来就是
 *   UTF-8 字节，走 getline。
 *
 * @returns false = 输入结束（Ctrl+Z / 管道关闭）
 */
bool readLineUtf8(std::string& out)
{
#ifdef _WIN32
    const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD        mode  = 0;
    if (input != INVALID_HANDLE_VALUE && input != nullptr && ::GetConsoleMode(input, &mode) != 0)
    {
        wchar_t buffer[4096];
        DWORD   read = 0;
        if (::ReadConsoleW(input, buffer, static_cast<DWORD>(std::size(buffer) - 1), &read, nullptr) == 0)
        {
            return false;
        }
        if (read == 0)
        {
            return false;   // Ctrl+Z
        }

        std::wstring line(buffer, read);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n'))
        {
            line.pop_back();
        }

        out = toUtf8(line.c_str());
        return true;
    }
#endif

    return static_cast<bool>(std::getline(std::cin, out));
}

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
          "  note add <文本...>      追加一条记录（会尝试解析「上周三」「下午3点」这类时间）\n"
          "  note list               列出全部（按时间倒序，不含已删除）\n"
          "  note list --deleted     只列出已删除的（准备恢复 / 彻底删）\n"
          "  note show <id>          看一条的详情\n"
          "  note confirm <id>       标记「已确认」（草稿转正）\n"
          "  note rm <id>            删除（软删除，可 note undo 恢复）\n"
          "  note undo <id>          撤销删除\n"
          "  note purge <id>         彻底删除（不可恢复）\n"
          "  note parse <文本...>    只试一下时间解析，不写盘（排查「为什么不识别」用）\n"
          "  note help               显示这个帮助\n"
          "\n"
          "  直接运行 note（不带参数）进入交互模式，命令里不用再写开头的 note：\n"
          "      > add 今天下午3点跟老张聊排期\n"
          "      > list\n"
          "      > quit\n"
          "\n"
          "数据文件：环境变量 NOTE_FILE，默认当前目录的 notes.jsonl\n";
}

// ---------------------------------------------------------------------------
// 执行一条命令
//
// args[0] 是程序名，args[1] 是命令 —— 交互模式也按同样的形状拼，避免两套解析。
// 返回：0 成功 / 1 用法错误 / 2 运行时错误
// ---------------------------------------------------------------------------

/** 把 args[from..] 用空格拼回一句话（add / parse 共用）。 */
std::string joinArgs(const std::vector<std::string>& args, std::size_t from)
{
    std::string text;
    for (std::size_t i = from; i < args.size(); ++i)
    {
        if (i > from)
        {
            text += ' ';
        }
        text += args[i];
    }
    return text;
}

/** UTC 时刻 -> 本地可读时间（解析结果给人看用）。 */
std::string localText(std::chrono::sys_seconds utc)
{
    const std::chrono::zoned_time zt{std::chrono::current_zone(), utc};
    return std::format("{:%Y-%m-%d %H:%M}", zt);
}

int execute(const std::vector<std::string>& args,
            core::Core&                     note,
            view::View&                     v,
            const std::filesystem::path&    dataFile)
{
    if (args.size() < 2)
    {
        printHelp(std::cout);
        return 1;
    }

    const std::string& cmd = args[1];

    if (cmd == "help")
    {
        printHelp(std::cout);
        return 0;
    }

    // ---- parse：干跑，只解析不写盘 ----
    //
    // 存在的意义：用户说"这条没识别出时间"时，不用先存进去再看 show，
    // 直接拿原句跑一遍，立刻看到「解析成什么 + 剩下什么没被吃掉」。
    if (cmd == "parse")
    {
        if (args.size() < 3)
        {
            v.RenderError("用法：note parse <文本...>");
            return 1;
        }

        const std::string parsedText = joinArgs(args, 2);
        const auto        base       = nowUtc();
        const auto        parsed     = parse::parseTime(parsedText, utcToLocalWall(base));

        std::cout << "输入　" << parsedText << '\n';

        if (parsed.occurred)
        {
            const auto start = localWallToUtc(parsed.occurred->start);
            if (parsed.occurred->end)
            {
                std::cout << "时段　" << localText(start) << "  →  "
                          << localText(localWallToUtc(*parsed.occurred->end)) << "（本地时间）\n";
            }
            else
            {
                std::cout << "时间　" << localText(start) << "（本地时间）\n";
            }
        }
        else
        {
            std::cout << "时间　（没解析出时间 —— 原文仍会原样保存进 raw_text）\n";
        }

        std::cout << "剩余　[" << parsed.remainder << "]\n";
        return 0;
    }

    const bool needsId = (cmd == "show" || cmd == "confirm" || cmd == "rm" || cmd == "undo" ||
                          cmd == "purge");

    if (cmd != "add" && cmd != "list" && !needsId)
    {
        v.RenderError("不认识的命令: " + cmd + "（输入 help 看用法）");
        return 1;
    }

    if ((cmd == "add" || needsId) && args.size() < 3)
    {
        v.RenderError("这个命令需要一个参数（输入 help 看用法）");
        return 1;
    }

    // 统一的失败出口：终端给用户看，日志给"事后排查"看。
    // 只对**异常**记日志：用户敲错 id 不算异常，不记。
    const auto fail = [&](const std::string& message) {
        core::appendLog(dataFile, core::LogLevel::Error, message);
        v.RenderError(message);
        return 2;
    };

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
            return fail("记录失败: " + added.error());
        }

        v.RenderMessage("已记录 " + added->id);
        return 0;
    }

    if (cmd == "list")
    {
        // 默认只看活的；--deleted 只看墓碑（恢复 / 彻底删之前要能看到 id）。
        if (args.size() >= 3 && args[2] == "--deleted")
        {
            std::vector<core::Entry> deleted;
            for (const auto& e : note.listAll())
            {
                if (e.deleted_at)
                {
                    deleted.push_back(e);
                }
            }

            if (deleted.empty())
            {
                v.RenderMessage("（没有已删除的记录）");
            }
            else
            {
                v.RenderView(deleted);
            }
            return 0;
        }

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

        // 已删除的记录 find 不到，但用户可能只是记错了状态 —— 提示一句。
        if (note.findAny(args[2]))
        {
            v.RenderError("这条记录已删除，可 undo " + args[2] + " 恢复");
        }
        else
        {
            v.RenderError("没有这条记录: " + args[2]);
        }
        return 2;
    }

    if (cmd == "confirm")
    {
        auto found = note.find(args[2]);
        if (!found)
        {
            v.RenderError("没有这条记录: " + args[2]);
            return 2;
        }

        found->confirmed = true;

        if (auto updated = note.update(*found); !updated)
        {
            return fail("确认失败: " + updated.error());
        }

        v.RenderMessage("已确认 " + args[2]);
        return 0;
    }

    if (cmd == "rm")
    {
        if (auto removed = note.remove(args[2]); !removed)
        {
            return fail(removed.error());
        }

        v.RenderMessage("已删除 " + args[2] + "（可 undo " + args[2] + " 恢复）");
        return 0;
    }

    if (cmd == "undo")
    {
        if (auto restored = note.restore(args[2]); !restored)
        {
            return fail(restored.error());
        }

        v.RenderMessage("已恢复 " + args[2]);
        return 0;
    }

    // cmd == "purge"
    if (auto purged = note.purge(args[2]); !purged)
    {
        return fail(purged.error());
    }

    v.RenderMessage("已彻底删除 " + args[2] + "（不可恢复）");
    return 0;
}

/** 读盘 + 打警告。失败返回 2。 */
int loadOrReport(core::Core& note, view::View& v, const std::filesystem::path& dataFile)
{
    if (auto loaded = note.load(); !loaded)
    {
        const std::string message = "读取数据文件失败: " + loaded.error();
        core::appendLog(dataFile, core::LogLevel::Error, message);
        v.RenderError(message);
        return 2;
    }

    // 坏行只警告、不拦人 —— 文件被手工编辑过是很正常的事（ROADMAP §2.6）。
    // store 已经把它们写进日志了，这里只负责显示。
    for (const auto& w : note.warnings())
    {
        v.RenderError(w);
    }
    return 0;
}

/** 交互模式：双击启动就是这个（否则窗口一闪就关，用户以为闪退）。 */
int interactive(view::View& v)
{
    const std::filesystem::path dataFile = dataPath();
    core::Core                  note{ dataFile };

    if (const int code = loadOrReport(note, v, dataFile); code != 0)
    {
        // 读盘失败也别立刻把窗口关掉：让用户看见原因。
        std::cout << "\n按回车退出…" << std::flush;
        std::string ignored;
        readLineUtf8(ignored);
        return code;
    }

    std::cout << "note 交互模式（输入 help 看命令，quit 退出）\n"
              << "数据文件: " << dataFile.string() << "\n\n";

    for (;;)
    {
        std::cout << "> " << std::flush;

        std::string line;
        if (!readLineUtf8(line))
        {
            std::cout << '\n';   // Ctrl+Z / 管道结束
            break;
        }

        // 切成 token：第一个是命令，其余是参数（add 会把它们再拼回一句话）。
        std::vector<std::string> tokens;
        std::size_t              begin = 0;
        while (begin < line.size())
        {
            while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t'))
            {
                ++begin;
            }
            const std::size_t end = line.find_first_of(" \t", begin);
            if (end == std::string::npos)
            {
                tokens.push_back(line.substr(begin));
                break;
            }
            tokens.push_back(line.substr(begin, end - begin));
            begin = end;
        }

        if (tokens.empty())
        {
            continue;
        }

        if (tokens[0] == "quit" || tokens[0] == "exit")
        {
            break;
        }

        // execute 期望 args[0] 是程序名，这里补一个占位，两套入口共用同一份解析。
        std::vector<std::string> args;
        args.reserve(tokens.size() + 1);
        args.emplace_back("note");
        args.insert(args.end(), tokens.begin(), tokens.end());

        execute(args, note, v, dataFile);
        std::cout << '\n';
    }

    return 0;
}

/** 一次性命令入口。 */
int run(const std::vector<std::string>& args, view::View& v)
{
    // 不带参数 = 双击启动：进交互模式，别打印一堆帮助然后关窗口。
    if (args.size() < 2)
    {
        return interactive(v);
    }

    const std::string& cmd = args[1];

    // 先做纯参数检查，别为了一个用法错误去读磁盘。
    if (cmd == "help")
    {
        printHelp(std::cout);
        return 0;
    }

    const bool needsId = (cmd == "show" || cmd == "confirm" || cmd == "rm" || cmd == "undo" ||
                          cmd == "purge");

    if (cmd != "add" && cmd != "list" && cmd != "parse" && !needsId)
    {
        v.RenderError("不认识的命令: " + cmd + "（输入 note help 看用法）");
        return 1;
    }

    if ((cmd == "add" || cmd == "parse" || needsId) && args.size() < 3)
    {
        v.RenderError("这个命令需要一个参数（输入 note help 看用法）");
        return 1;
    }

    const std::filesystem::path dataFile = dataPath();
    core::Core                  note{ dataFile };

    if (const int code = loadOrReport(note, v, dataFile); code != 0)
    {
        return code;
    }

    return execute(args, note, v, dataFile);
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
