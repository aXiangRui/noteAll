#include "app_state.hpp"

#include "eui/app.h"

#include <cstdlib>
#include <filesystem>

#include "app/api_key.hpp"
#include "core/core.hpp"

namespace app
{

void repaint()
{
    requestUpdate();   // 框架的 ::app::requestUpdate —— 和我们的 namespace app 是同一个
}

std::chrono::year_month_day todayLocal()
{
    const auto now = std::chrono::system_clock::now();
    const std::chrono::zoned_time zt{ std::chrono::current_zone(), now };
    const auto localDay = std::chrono::floor<std::chrono::days>(zt.get_local_time());
    return std::chrono::year_month_day{ localDay };
}

void reloadData(AppState& state)
{
    state.error.clear();
    state.entries.clear();

    core::Core note{ app::dataPath() };
    if (auto loaded = note.load(); !loaded)
    {
        state.error = "读取数据失败: " + loaded.error();
        state.loaded = true;
        return;
    }

    state.entries = note.listAll();
    for (const auto& warning : note.warnings())
    {
        state.error += warning + "\n";
    }
    state.loaded = true;
    repaint();
}

void ensureLoaded(AppState& state)
{
    if (state.loaded)
    {
        return;
    }

    state.weekStart = mondayOf(todayLocal());
    reloadData(state);
}

bool llmKeyConfigured()
{
    // 规则和草稿线程、CLI 完全共用一份（app::findApiKey）：
    // 各自实现的话，最尴尬的情况是设置页说"已配置"、真生成时说"没找到"。
    return app::findApiKey(app::dataPath()).has_value();
}

std::size_t pendingDraftCount(AppState const& state)
{
    std::size_t count = 0;
    for (const auto& entry : state.entries)
    {
        if (!entry.deleted_at && entry.draft_status == core::DraftStatus::Pending)
        {
            ++count;
        }
    }
    return count;
}

core::Entry const* firstPendingDraft(AppState const& state)
{
    for (const auto& entry : state.entries)
    {
        if (!entry.deleted_at && entry.draft_status == core::DraftStatus::Pending)
        {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace app
