// note_gui —— 周视图：一周七天 × 每天的任务时间线。
//
// 设计取舍：
//   * 数据准备全在 app::buildWeek（纯函数、有单测），这里只负责"画"；
//   * 每列是一条竖线串起来的时间线（节点的圆点按 kind 上色），
//     这样"一天里先后发生什么"一眼能看出来，比一堆卡片更像流程图；
//   * 只读：这一版不提供编辑，先把"看得清"做对，写操作留给下一步。
//
// 刷新时机：启动时读一次；点「刷新」按钮再读（CLI 里刚记的东西就能出现）。

#include "eui_neo.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "app/calendar.hpp"
#include "core/core.hpp"

namespace app {

namespace {

struct ViewState {
    std::chrono::year_month_day weekStart{};   // 周一
    std::vector<core::Entry>  entries;
    std::string               error;
    bool                      loaded = false;
};

ViewState state;

// 配色（深色底）：不用 theme，先写死，等视觉定了再抽出去
const eui::Color kPageBg{ 0.07f, 0.08f, 0.10f, 1.0f };
const eui::Color kColumnBg{ 0.11f, 0.12f, 0.16f, 1.0f };
const eui::Color kTodayBg{ 0.14f, 0.19f, 0.26f, 1.0f };
const eui::Color kCardBg{ 0.16f, 0.18f, 0.23f, 1.0f };
const eui::Color kText{ 0.93f, 0.95f, 0.98f, 1.0f };
const eui::Color kMuted{ 0.60f, 0.65f, 0.72f, 1.0f };
const eui::Color kLine{ 0.28f, 0.32f, 0.40f, 0.85f };

// 记录类型 → 圆点颜色：一眼区分"事 / 待办 / 资料 / 想法"
eui::Color kindColor(core::Kind kind) {
    switch (kind) {
        case core::Kind::Event:   return { 0.36f, 0.66f, 0.98f, 1.0f };
        case core::Kind::Todo:    return { 0.98f, 0.62f, 0.29f, 1.0f };
        case core::Kind::Note:    return { 0.42f, 0.82f, 0.58f, 1.0f };
        case core::Kind::Thought: return { 0.72f, 0.60f, 0.98f, 1.0f };
        default:                  return { 0.50f, 0.55f, 0.62f, 1.0f };
    }
}

std::chrono::year_month_day todayLocal() {
    const auto now = std::chrono::system_clock::now();
    const std::chrono::zoned_time zt{ std::chrono::current_zone(), now };
    const auto localDay = std::chrono::floor<std::chrono::days>(zt.get_local_time());
    return std::chrono::year_month_day{ localDay };
}

void reload() {
    state.error.clear();
    state.entries.clear();

    core::Core note{ app::dataPath() };
    if (auto loaded = note.load(); !loaded) {
        state.error = "读取数据失败: " + loaded.error();
        state.loaded = true;
        return;
    }

    state.entries = note.listAll();
    for (const auto& w : note.warnings()) {
        state.error += w + "\n";
    }
    state.loaded = true;
}

void ensureLoaded() {
    if (state.loaded) {
        return;
    }
    state.weekStart = app::mondayOf(todayLocal());
    reload();
}

void composeTopBar(eui::Ui& ui, float width, float pad) {
    const float barY = 12.0f;
    const float buttonW = 108.0f;

    components::button(ui, "nav.prev")
        .position(pad, barY).size(buttonW, 32.0f)
        .text("‹ 上一周")
        .onClick([] {
            state.weekStart = std::chrono::year_month_day{
                std::chrono::sys_days{ state.weekStart } - std::chrono::days{ 7 } };
        })
        .build();

    components::button(ui, "nav.today")
        .position(pad + buttonW + 8.0f, barY).size(80.0f, 32.0f)
        .text("本周")
        .onClick([] {
            state.weekStart = app::mondayOf(todayLocal());
        })
        .build();

    components::button(ui, "nav.next")
        .position(pad + buttonW + 8.0f + 80.0f + 8.0f, barY).size(buttonW, 32.0f)
        .text("下一周 ›")
        .onClick([] {
            state.weekStart = std::chrono::year_month_day{
                std::chrono::sys_days{ state.weekStart } + std::chrono::days{ 7 } };
        })
        .build();

    components::button(ui, "nav.reload")
        .position(pad + width - 80.0f, barY).size(80.0f, 32.0f)
        .text("刷新")
        .onClick([] {
            reload();
        })
        .build();

    ui.text("week.range")
        .position(pad + 380.0f, barY + 4.0f)
        .size(width - 380.0f - 90.0f, 26.0f)
        .text(app::weekRangeLabel(state.weekStart))
        .fontSize(18.0f)
        .color(kText)
        .build();
}

void composeDayColumn(eui::Ui& ui, const app::DayColumn& column, int index, float x, float top,
                      float columnWidth, float columnHeight) {
    const std::string id = "day" + std::to_string(index);
    const bool isToday = column.day == todayLocal();
    const float headerHeight = 46.0f;
    const float itemHeight = 50.0f;
    const float itemGap = 8.0f;
    const float lineX = 14.0f;

    ui.stack(id)
        .position(x, top)
        .size(columnWidth, columnHeight)
        .clip()
        .content([&] {
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f)
                .size(columnWidth, columnHeight)
                .radius(12.0f)
                .color(isToday ? kTodayBg : kColumnBg)
                .build();

            ui.text(id + ".weekday")
                .position(10.0f, 8.0f)
                .size(columnWidth - 20.0f, 22.0f)
                .text(app::weekdayLabel(std::chrono::weekday{ std::chrono::sys_days{ column.day } }))
                .fontSize(15.0f)
                .color(isToday ? kText : kMuted)
                .build();

            ui.text(id + ".date")
                .position(10.0f, 28.0f)
                .size(columnWidth - 20.0f, 16.0f)
                .text(app::monthDayLabel(column.day))
                .fontSize(12.0f)
                .color(kMuted)
                .build();

            // 竖线：把一天的节点串成一条时间线
            if (!column.entries.empty()) {
                ui.rect(id + ".line")
                    .position(lineX + 3.0f, headerHeight + 4.0f)
                    .size(2.0f, std::max(1.0f, columnHeight - headerHeight - 12.0f))
                    .radius(1.0f)
                    .color(kLine)
                    .build();
            }

            float y = headerHeight;
            for (const core::Entry* entry : column.entries) {
                if (y + itemHeight > columnHeight) {
                    break;   // 放不下了就先截断（滚动留给下一步）
                }

                const std::string itemId = id + "." + entry->id;

                ui.rect(itemId + ".card")
                    .position(4.0f, y)
                    .size(columnWidth - 8.0f, itemHeight)
                    .radius(8.0f)
                    .color(kCardBg)
                    .build();

                // 节点圆点
                ui.rect(itemId + ".dot")
                    .position(lineX - 2.0f, y + 8.0f)
                    .size(10.0f, 10.0f)
                    .radius(5.0f)
                    .color(kindColor(entry->kind))
                    .build();

                ui.text(itemId + ".time")
                    .position(28.0f, y + 4.0f)
                    .size(columnWidth - 36.0f, 16.0f)
                    .text(app::timeLabel(*entry, *std::chrono::current_zone()))
                    .fontSize(11.0f)
                    .color(kMuted)
                    .build();

                ui.text(itemId + ".title")
                    .position(28.0f, y + 20.0f)
                    .size(columnWidth - 36.0f, 26.0f)
                    .text(app::displayTitle(*entry))
                    .fontSize(13.0f)
                    .lineHeight(16.0f)
                    .wrap(true)
                    .color(kText)
                    .build();

                y += itemHeight + itemGap;
            }

            if (column.entries.empty()) {
                ui.text(id + ".empty")
                    .position(12.0f, headerHeight + 6.0f)
                    .size(columnWidth - 24.0f, 18.0f)
                    .text("—")
                    .fontSize(12.0f)
                    .color(kMuted)
                    .build();
            }
        })
        .build();
}

}  // namespace

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("noteAll · 周视图")
        .windowSize(1280, 720);
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    ensureLoaded();

    const float pad = 16.0f;
    const float gap = 10.0f;
    const float width = std::max(320.0f, screen.width - pad * 2.0f);

    ui.rect("page.bg")
        .position(0.0f, 0.0f)
        .size(screen.width, screen.height)
        .color(kPageBg)
        .build();

    composeTopBar(ui, width, pad);

    const float top = 56.0f;
    const float columnsTop = top + 8.0f;
    const float columnHeight = std::max(180.0f, screen.height - columnsTop - pad);
    const float columnWidth = (width - gap * 6.0f) / 7.0f;

    const auto columns = app::buildWeek(state.entries, state.weekStart, *std::chrono::current_zone());
    for (std::size_t i = 0; i < columns.size(); ++i) {
        const float x = pad + static_cast<float>(i) * (columnWidth + gap);
        composeDayColumn(ui, columns[i], static_cast<int>(i), x, columnsTop, columnWidth, columnHeight);
    }

    // 底部：数据来源与错误。
    // 这行是必要的 —— 双击启动时"当前目录"是 exe 所在目录，
    // 可能和你在终端里跑 CLI 的目录不同，于是看到"没有数据"。
    ui.text("datapath")
        .position(pad, screen.height - 20.0f)
        .size(width, 16.0f)
        .text("数据: " + app::dataPath().string())
        .fontSize(11.0f)
        .color(kMuted)
        .build();

    if (!state.error.empty()) {
        ui.text("error")
            .position(pad, screen.height - 38.0f)
            .size(width, 16.0f)
            .text(state.error)
            .fontSize(11.0f)
            .color({ 0.95f, 0.45f, 0.42f, 1.0f })
            .build();
    }
}

}  // namespace app
