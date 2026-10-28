#include "app_state.hpp"

#include <cstdlib>
#include <filesystem>

#include "core/core.hpp"

namespace app
{

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
    if (const char* env = std::getenv("NOTE_LLM_KEY"); env != nullptr && *env != '\0')
    {
        return true;
    }

    // 查找顺序必须和 main.cpp 里 resolveApiKey 一致，否则设置页会说"已配置"、
    // 真正生成草稿时却说"没找到 key"。
    std::error_code ec;

    std::filesystem::path dataDir = dataPath().parent_path();
    if (dataDir.empty())
    {
        dataDir = ".";
    }
    if (std::filesystem::is_regular_file(dataDir / "note.key", ec))
    {
        return true;
    }

#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA"); appData != nullptr && *appData != '\0')
    {
        if (std::filesystem::is_regular_file(std::filesystem::path{ appData } / "noteAll" / "note.key", ec))
        {
            return true;
        }
    }
#endif

    return false;
}

}  // namespace app
