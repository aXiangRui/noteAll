#pragma once

#include "app_state.hpp"
#include "eui_neo.h"

// 每个模块一个 compose 函数，签名统一：
//
//     void composeXxxPage(eui::Ui& ui, AppState& state, const PageRect& area);
//
// 这样加模块不用动别人的代码：新文件 + 一个 enum + 侧边栏一行 + 这里声明一个。
namespace app
{

/** 模块可用的绘图区域（已扣掉侧边栏与底部信息栏）。 */
struct PageRect
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// 侧边栏：模块列表（预留位置给以后的月视图 / 搜索 / 复习计划…）
void composeSidebar(eui::Ui& ui, AppState& state, const PageRect& area);

// 模块：周视图（默认）
void composeWeekPage(eui::Ui& ui, AppState& state, const PageRect& area);

// 模块：设置
void composeSettingsPage(eui::Ui& ui, AppState& state, const PageRect& area);

// 覆盖层：选中一条记录后的详情面板（点卡片才出现，显示完整内容）
void composeDetailPanel(eui::Ui& ui, AppState& state, const PageRect& area);

}  // namespace app
