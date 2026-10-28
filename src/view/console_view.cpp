#include "view/console_view.hpp"

#include <chrono>
#include <cstddef>
#include <format>
#include <iostream>

// ConsoleView —— 把 Entry 画到终端。
//
// 它是"哑巴渲染器"：只负责把拿到的数据画出来，不知道数据从哪来，也不做业务判断。
// 判断"这条该不该显示""这个 id 对不对"是 core / main 的事。

namespace view
{
namespace
{
    // created_at 内部存的是 UTC，显示要转本地时区。
    // 见 docs/ROADMAP.md §2.7（本机实测可用）。
    std::string toLocalDisplay(std::chrono::sys_seconds t)
    {
        const std::chrono::zoned_time zt{std::chrono::current_zone(), t};
        return std::format("{:%Y-%m-%d %H:%M}", zt);
    }
}   // namespace

void ConsoleView::RenderView(const std::vector<core::Entry>& entries)
{
    if (entries.empty())
    {
        std::cout << "（还没有记录）\n";
        return;
    }

    // 列表就一行一条，紧凑好扫。字段展开是 RenderEntry 的事。
    for (const auto& e : entries)
    {
        // 用 '\n' 而不是 std::endl：std::endl 每次都强制 flush，
        // 逐条打屏时是白费的系统调用。
        //
        // 墓碑在这里只是一个标记：list 默认看不到它们，只有 list --deleted 会。
        std::cout << toLocalDisplay(e.created_at) << "  " << e.id << "  " << e.raw_text
                  << (e.deleted_at ? "   [已删除]" : "") << '\n';
    }
}

void ConsoleView::RenderEntry(const core::Entry& e)
{
    std::cout << "id:        " << e.id << '\n';
    std::cout << "记录时间:  " << toLocalDisplay(e.created_at) << '\n';
    std::cout << "kind:      " << core::toString(e.kind) << '\n';
    std::cout << "来源:      " << core::toString(e.source) << '\n';
    std::cout << "确认:      " << (e.confirmed ? "是" : "否") << '\n';

    // 草稿状态：用户要知道「这条还欠摘要」还是「生成失败了、试了几次」。
    std::cout << "草稿:      " << core::toString(e.draft_status);
    if (e.draft_status == core::DraftStatus::Failed)
    {
        std::cout << "（已试 " << e.draft_attempts << " 次：" << e.draft_error << "）";
    }
    std::cout << '\n';

    std::cout << "原文:      " << e.raw_text << '\n';
    std::cout << "标题:      " << (e.title.empty() ? "（无）" : e.title) << '\n';
    std::cout << "摘要:      " << (e.summary.empty() ? "（无）" : e.summary) << '\n';

    std::cout << "标签:      ";
    RenderTags(e.tags);
    std::cout << '\n';

    std::cout << "发生时间:  ";
    if (e.occurred)
    {
        std::cout << toLocalDisplay(e.occurred->start);
        if (e.occurred->end)
        {
            std::cout << " ~ " << toLocalDisplay(*e.occurred->end);
        }
    }
    else
    {
        // 空 = 时间未知。**不要猜** —— 见 DESIGN.md §2.3(c)。
        std::cout << "（未知）";
    }
    std::cout << '\n';

    if (e.deleted_at)
    {
        std::cout << "状态:      已删除（" << toLocalDisplay(*e.deleted_at)
                  << "），可 note undo " << e.id << " 恢复\n";
    }
}

void ConsoleView::RenderTags(const std::vector<std::string>& tags)
{
    if (tags.empty())
    {
        std::cout << "（无）";
        return;
    }

    // 不换行：换不换由调用方决定（详情里跟在 "标签:  " 后面）。
    for (std::size_t i = 0; i < tags.size(); ++i)
    {
        if (i > 0)
        {
            std::cout << ' ';
        }
        std::cout << tags[i];
    }
}

void ConsoleView::RenderMessage(std::string_view msg)
{
    std::cout << msg << '\n';
}

void ConsoleView::RenderError(std::string_view msg)
{
    // 走 stderr：`note list > out.txt` 的时候，错误仍然打在屏幕上而不是混进文件。
    std::cerr << "错误: " << msg << '\n';
}

}  // namespace view
