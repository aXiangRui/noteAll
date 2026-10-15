// parse_demo —— 手动试 parse::parseTime 的小工具。
//
// 用法：
//     parse_demo                      交互模式，输一行看一行
//     parse_demo 2026-03-01           换掉基准日，再进交互模式
//     parse_demo --demo               跑一组写死的样例，一条条看
//
// 在 build/bin/Debug/ 下。注意这是个工具，不是测试 ——
// 测试在 tests/，这里的输出要你自己用眼睛看。

#include "parse/timeParse.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std::chrono;

namespace
{
    // 默认基准：2026-10-08（周四）14:00
    const sys_seconds DEFAULT_BASE = sys_days{ 2026y / 10 / 8d } + 14h;

    std::string formatTime(sys_seconds t)
    {
        const sys_days       dp = floor<days>(t);
        const year_month_day ymd{ dp };
        const hh_mm_ss<std::chrono::seconds> hms{ t - dp };

        char buf[64];
        std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02d:%02d:%02d",
                      (int)ymd.year(), (unsigned)ymd.month(), (unsigned)ymd.day(),
                      (int)hms.hours().count(), (int)hms.minutes().count(),
                      (int)hms.seconds().count());
        return buf;
    }

    void show(std::string_view text, sys_seconds base)
    {
        const parse::ParseResult r = parse::parseTime(text, base);

        std::printf("  输入　%s\n", std::string{ text }.c_str());
        if (r.occurred)
        {
            if (r.occurred->end)
            {
                // 时段：「3点到5点」这种，end 才有值
                std::printf("  时段　%s  →  %s\n",
                            formatTime(r.occurred->start).c_str(),
                            formatTime(*r.occurred->end).c_str());
            }
            else
            {
                std::printf("  时间　%s\n", formatTime(r.occurred->start).c_str());
            }
        }
        else
        {
            std::printf("  时间　（解析不出）\n");
        }
        std::printf("  剩余　[%s]\n\n", r.remainder.c_str());
    }

    void runSamples(sys_seconds base)
    {
        const char* samples[] =
        {
            // ── 能解析的 ──
            "今天和小李吃饭",
            "昨天没睡好",
            "明天开会",
            "上周三下午和小李讨论了方案",
            "3月5日交作业",
            "2026-10-01的会议",
            "明天早上8点开会",
            "9:30 站会",
            "下午3点到5点",
            "上周三下午3点到5点和小李讨论了方案",
            "晚上11点到凌晨1点",          // 第二个钟点自带时段词 + 跨零点
            "下下周三下午3点",             // 前缀叠两次
            "周一到周五",                  // 日期区间
            "星期天去爬山",                // 口语的周日写法
            "大后天",
            "今年3月5日",                  // 明确年份，不再按"还没到的那个"猜
            "今年要努力",                  // 「今年」= 当年 1 月 1 日

            // ── 解析不出的 ──
            "下月一号交房租",
            "下周天气不错",                // 「天」是周日，但「天气」不是星期词
            "25点开会",
            "最近有点迷茫",
            "abcdefg",
        };

        for (const char* s : samples)
        {
            show(s, base);
        }
    }

    // 命令行给了 YYYY-MM-DD 就用它当基准日；认不出来就返回默认值
    sys_seconds baseFromArg(const char* arg)
    {
        std::istringstream in{ arg };
        sys_seconds        t{};
        from_stream(in, "%Y-%m-%d", t);

        if (in.fail())
        {
            std::printf("看不懂的日期参数：%s（格式应为 YYYY-MM-DD），用默认值\n\n", arg);
            return DEFAULT_BASE;
        }
        return t;
    }
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    // 不加这两句，中文在控制台里是乱码
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    bool        samplesOnly = false;
    sys_seconds base        = DEFAULT_BASE;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a{ argv[i] };
        if (a == "--demo")
        {
            samplesOnly = true;
        }
        else
        {
            base = baseFromArg(argv[i]);
        }
    }

    std::printf("基准时刻 base = %s\n", formatTime(base).c_str());
    std::printf("========================================\n\n");

    if (samplesOnly)
    {
        runSamples(base);
        return 0;
    }

    std::printf("输一行中文，回车看结果。输入 q 退出。\n\n");

    std::string line;
    while (true)
    {
        std::printf("> ");
        std::fflush(stdout);

        if (!std::getline(std::cin, line))
        {
            break;   // Ctrl+Z / Ctrl+D / 输入被管道关掉
        }
        if (line == "q")
        {
            break;
        }

        std::printf("\n");
        show(line, base);
    }
    return 0;
}
