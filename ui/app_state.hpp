#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "app/calendar.hpp"
#include "core/entry.hpp"
#include "eui_neo.h"

// GUI 的共享状态与配色。
//
// 为什么单独拿出来：每加一个模块（周视图 / 设置 / 以后的月视图、搜索、
// 复习计划…）都要读同一份数据、用同一套颜色。放在 app.cpp 里会变成
// 一个越写越长的巨型文件。
namespace app
{

// 侧边栏里的模块。加一个模块 = 这里加一项 + 侧边栏加一行 + 写一个 compose 函数。
enum class Page
{
    Week,        // 周视图（当前默认）
    Settings,    // 设置
};

struct AppState
{
    Page                      page = Page::Week;
    std::chrono::year_month_day weekStart{};      // 当前周的周一
    std::vector<core::Entry>  entries;            // 内存快照（compose 里不读盘）
    std::string               error;
    bool                      loaded = false;
};

/** 本地"今天"（按本地时区，不是 UTC）。 */
std::chrono::year_month_day todayLocal();

/** 只在第一次调用时读盘；之后靠 reloadData() 手动刷新。 */
void ensureLoaded(AppState& state);

/** 重新读盘（点「刷新」时用）。compose 里绝不能调它。 */
void reloadData(AppState& state);

/** 有没有可用的模型 key：环境变量 > 数据目录/note.key > %APPDATA%/noteAll/note.key */
bool llmKeyConfigured();

// ---------------------------------------------------------------------------
// 配色：银白底 + 淡蓝强调
//
// 统一放在这里，模块之间才不会各自发明颜色。改名时也只改一处。
// ---------------------------------------------------------------------------
namespace colors
{

inline constexpr eui::Color kPageBg{ 0.957f, 0.969f, 0.980f, 1.0f };      // 页面底：银白
inline constexpr eui::Color kSidebarBg{ 0.925f, 0.941f, 0.961f, 1.0f };   // 侧栏：略深一档
inline constexpr eui::Color kColumnBg{ 0.976f, 0.984f, 0.992f, 1.0f };    // 日期列
inline constexpr eui::Color kTodayBg{ 0.902f, 0.945f, 0.996f, 1.0f };     // 今天那一列
inline constexpr eui::Color kCardBg{ 0.855f, 0.914f, 0.996f, 1.0f };      // 任务卡：淡蓝
inline constexpr eui::Color kCardBorder{ 0.729f, 0.827f, 0.957f, 1.0f };  // 淡蓝描边
inline constexpr eui::Color kAccent{ 0.780f, 0.878f, 0.996f, 1.0f };      // 选中态
inline constexpr eui::Color kText{ 0.145f, 0.180f, 0.231f, 1.0f };        // 主文字
inline constexpr eui::Color kMuted{ 0.451f, 0.502f, 0.573f, 1.0f };       // 次要文字
inline constexpr eui::Color kLine{ 0.702f, 0.780f, 0.878f, 0.90f };       // 时间线
inline constexpr eui::Color kDanger{ 0.812f, 0.235f, 0.208f, 1.0f };

// 记录类型 → 圆点颜色（淡蓝体系里的四种区分）
inline constexpr eui::Color kKindEvent{ 0.235f, 0.510f, 0.855f, 1.0f };   // 事
inline constexpr eui::Color kKindTodo{ 0.918f, 0.545f, 0.180f, 1.0f };    // 待办
inline constexpr eui::Color kKindNote{ 0.204f, 0.678f, 0.420f, 1.0f };    // 资料
inline constexpr eui::Color kKindThought{ 0.541f, 0.400f, 0.878f, 1.0f }; // 想法
inline constexpr eui::Color kKindUnknown{ 0.545f, 0.596f, 0.667f, 1.0f };

}  // namespace colors

/** 一条记录该用哪个圆点颜色。 */
inline eui::Color kindColor(core::Kind kind)
{
    switch (kind)
    {
        case core::Kind::Event:   return colors::kKindEvent;
        case core::Kind::Todo:    return colors::kKindTodo;
        case core::Kind::Note:    return colors::kKindNote;
        case core::Kind::Thought: return colors::kKindThought;
        default:                  return colors::kKindUnknown;
    }
}

}  // namespace app
