// 周视图模块：一周七列，每天一条任务时间线。
//
// 这个文件只管"画"。数据摊平是 app::buildWeek（纯函数、有单测），
// 读盘是 app::reloadData。想改布局、配色、卡片密度，只改这里。
//
// 字号一律从 metrics 取（18–28）。改字号时**必须**同步改这里的高度常量，
// 否则文字会互相压住 —— 所以下面每一项高度都是用字号算出来的。

#include "pages.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace app
{

namespace
{

using namespace metrics;

constexpr float kChipHeight = 40.0f;                       // 18px 字 + 上下留白
constexpr float kChipLabelTop = 9.0f;
constexpr float kTopBarHeight = 58.0f;
constexpr float kColumnGap = 8.0f;
constexpr float kColumnPad = 12.0f;
constexpr float kLineX = 16.0f;
constexpr float kItemGap = 10.0f;

// 列头：周几（20）+ 日期（18）
constexpr float kHeaderHeight = 66.0f;

// 卡片：时间（18，行高 24）+ 标题（20，最多两行 × 26）
constexpr float kItemHeight = 92.0f;   // 时间 24 + 标题两行 52 + 上下留白

// 小胶囊按钮：配色完全由我们控制（不用 components::button 的深色默认）
void composeChip(eui::Ui& ui, const std::string& id, float x, float y, float width,
                 const std::string& label, bool emphasized, std::function<void()> onClick)
{
    ui.stack(id)
        .position(x, y)
        .size(width, kChipHeight)
        .onClick(std::move(onClick))
        .content([&] {
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f)
                .size(width, kChipHeight)
                .radius(9.0f)
                .color(emphasized ? colors::kAccent : colors::kColumnBg)
                .border(1.0f, colors::kCardBorder)
                .build();

            ui.text(id + ".label")
                .position(0.0f, kChipLabelTop)
                .size(width, kLineCaption)
                .text(label)
                .fontSize(kFontCaption)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(colors::kText)
                .build();
        })
        .build();
}

void composeTopBar(eui::Ui& ui, AppState& state, const PageRect& area)
{
    const float y = area.y + 4.0f;

    composeChip(ui, "nav.prev", area.x, y, 108.0f, "‹ 上一周", false, [&state] {
        state.weekStart = std::chrono::year_month_day{
            std::chrono::sys_days{ state.weekStart } - std::chrono::days{ 7 } };
    });

    composeChip(ui, "nav.today", area.x + 116.0f, y, 88.0f, "本周", false, [&state] {
        state.weekStart = mondayOf(todayLocal());
    });

    composeChip(ui, "nav.next", area.x + 212.0f, y, 108.0f, "下一周 ›", false, [&state] {
        state.weekStart = std::chrono::year_month_day{
            std::chrono::sys_days{ state.weekStart } + std::chrono::days{ 7 } };
    });

    composeChip(ui, "nav.reload", area.x + area.width - 88.0f, y, 88.0f, "刷新", false, [&state] {
        reloadData(state);
    });

    ui.text("week.range")
        .position(area.x + 336.0f, y + 7.0f)
        .size(std::max(200.0f, area.width - 336.0f - 100.0f), kLineCaption)
        .text(app::weekRangeLabel(state.weekStart))
        .fontSize(kFontRange)
        .color(colors::kText)
        .build();
}

void composeDayColumn(eui::Ui& ui, const DayColumn& column, int index, const PageRect& area,
                      float columnWidth)
{
    const std::string id = "day" + std::to_string(index);
    const bool isToday = column.day == todayLocal();

    ui.stack(id)
        .position(area.x + static_cast<float>(index) * (columnWidth + kColumnGap), area.y)
        .size(columnWidth, area.height)
        .clip()
        .content([&] {
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f)
                .size(columnWidth, area.height)
                .radius(12.0f)
                .color(isToday ? colors::kTodayBg : colors::kColumnBg)
                .border(1.0f, colors::kCardBorder)
                .build();

            ui.text(id + ".weekday")
                .position(kColumnPad, 10.0f)
                .size(columnWidth - kColumnPad * 2.0f, kLineBody)
                .text(app::weekdayLabel(std::chrono::weekday{ std::chrono::sys_days{ column.day } }))
                .fontSize(kFontBody)
                .color(isToday ? colors::kKindEvent : colors::kText)
                .build();

            ui.text(id + ".date")
                .position(kColumnPad, 36.0f)
                .size(columnWidth - kColumnPad * 2.0f, kLineCaption)
                .text(app::monthDayLabel(column.day))
                .fontSize(kFontCaption)
                .color(colors::kMuted)
                .build();

            // 竖线：把一天的节点串成一条时间线
            if (!column.entries.empty())
            {
                ui.rect(id + ".line")
                    .position(kLineX + 3.0f, kHeaderHeight + 6.0f)
                    .size(2.0f, std::max(1.0f, area.height - kHeaderHeight - 16.0f))
                    .radius(1.0f)
                    .color(colors::kLine)
                    .build();
            }

            float y = kHeaderHeight;
            for (const core::Entry* entry : column.entries)
            {
                if (y + kItemHeight > area.height)
                {
                    break;   // 放不下先截断（列内滚动留给下一步）
                }

                const std::string itemId = id + "." + entry->id;

                // 任务卡：淡蓝底 + 淡蓝描边
                ui.rect(itemId + ".card")
                    .position(4.0f, y)
                    .size(columnWidth - 8.0f, kItemHeight)
                    .radius(10.0f)
                    .color(colors::kCardBg)
                    .border(1.0f, colors::kCardBorder)
                    .build();

                // 节点圆点（类型色）
                ui.rect(itemId + ".dot")
                    .position(kLineX - 2.0f, y + 12.0f)
                    .size(10.0f, 10.0f)
                    .radius(5.0f)
                    .color(app::kindColor(entry->kind))
                    .build();

                ui.text(itemId + ".time")
                    .position(kColumnPad + 16.0f, y + 6.0f)
                    .size(columnWidth - kColumnPad - 24.0f, kLineCaption)
                    .text(app::timeLabel(*entry, *std::chrono::current_zone()))
                    .fontSize(kFontCaption)
                    .color(colors::kMuted)
                    .build();

                ui.text(itemId + ".title")
                    .position(kColumnPad + 16.0f, y + 32.0f)
                    .size(columnWidth - kColumnPad - 24.0f, kItemHeight - 40.0f)
                    .text(app::displayTitle(*entry))
                    .fontSize(kFontBody)
                    .lineHeight(kLineBody)
                    .wrap(true)
                    .color(colors::kText)
                    .build();

                y += kItemHeight + kItemGap;
            }

            if (column.entries.empty())
            {
                ui.text(id + ".empty")
                    .position(kColumnPad, kHeaderHeight + 4.0f)
                    .size(columnWidth - kColumnPad * 2.0f, kLineCaption)
                    .text("—")
                    .fontSize(kFontCaption)
                    .color(colors::kMuted)
                    .build();
            }
        })
        .build();
}

}  // namespace

void composeWeekPage(eui::Ui& ui, AppState& state, const PageRect& area)
{
    composeTopBar(ui, state, area);

    const PageRect grid{
        area.x,
        area.y + kTopBarHeight,
        area.width,
        std::max(140.0f, area.height - kTopBarHeight),
    };

    const float columnWidth = (grid.width - kColumnGap * 6.0f) / 7.0f;
    const auto  columns = app::buildWeek(state.entries, state.weekStart, *std::chrono::current_zone());

    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        composeDayColumn(ui, columns[i], static_cast<int>(i), grid, columnWidth);
    }
}

}  // namespace app
