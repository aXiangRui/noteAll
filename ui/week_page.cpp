// 周视图模块：一周七列，每天一条任务时间线。
//
// 这个文件只管"画"。数据摊平是 app::buildWeek（纯函数、有单测），
// 读盘是 app::reloadData。想改布局、配色、卡片密度，只改这里。

#include "pages.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace app
{

namespace
{

constexpr float kTopBarHeight = 44.0f;
constexpr float kHeaderHeight = 46.0f;
constexpr float kItemHeight = 50.0f;
constexpr float kItemGap = 8.0f;
constexpr float kLineX = 14.0f;

// 小胶囊按钮：不用 components::button，是为了完全控制银白配色
void composeChip(eui::Ui& ui, const std::string& id, float x, float y, float width,
                 const std::string& label, bool emphasized, std::function<void()> onClick)
{
    ui.stack(id)
        .position(x, y)
        .size(width, 30.0f)
        .onClick(std::move(onClick))
        .content([&] {
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f)
                .size(width, 30.0f)
                .radius(8.0f)
                .color(emphasized ? colors::kAccent : colors::kColumnBg)
                .border(1.0f, colors::kCardBorder)
                .build();

            ui.text(id + ".label")
                .position(0.0f, 7.0f)
                .size(width, 18.0f)
                .text(label)
                .fontSize(13.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(colors::kText)
                .build();
        })
        .build();
}

void composeTopBar(eui::Ui& ui, AppState& state, const PageRect& area)
{
    const float y = area.y + 6.0f;

    composeChip(ui, "nav.prev", area.x, y, 76.0f, "‹ 上一周", false, [&state] {
        state.weekStart = std::chrono::year_month_day{
            std::chrono::sys_days{ state.weekStart } - std::chrono::days{ 7 } };
    });

    composeChip(ui, "nav.today", area.x + 82.0f, y, 60.0f, "本周", false, [&state] {
        state.weekStart = mondayOf(todayLocal());
    });

    composeChip(ui, "nav.next", area.x + 148.0f, y, 76.0f, "下一周 ›", false, [&state] {
        state.weekStart = std::chrono::year_month_day{
            std::chrono::sys_days{ state.weekStart } + std::chrono::days{ 7 } };
    });

    composeChip(ui, "nav.reload", area.x + area.width - 68.0f, y, 68.0f, "刷新", false, [&state] {
        reloadData(state);
    });

    ui.text("week.range")
        .position(area.x + 250.0f, y + 6.0f)
        .size(std::max(160.0f, area.width - 250.0f - 80.0f), 22.0f)
        .text(app::weekRangeLabel(state.weekStart))
        .fontSize(17.0f)
        .color(colors::kText)
        .build();
}

void composeDayColumn(eui::Ui& ui, const DayColumn& column, int index, const PageRect& area,
                      float columnWidth)
{
    const std::string id = "day" + std::to_string(index);
    const bool isToday = column.day == todayLocal();

    ui.stack(id)
        .position(area.x + static_cast<float>(index) * (columnWidth + 8.0f), area.y)
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
                .position(12.0f, 8.0f)
                .size(columnWidth - 24.0f, 22.0f)
                .text(app::weekdayLabel(std::chrono::weekday{ std::chrono::sys_days{ column.day } }))
                .fontSize(15.0f)
                .color(isToday ? colors::kKindEvent : colors::kText)
                .build();

            ui.text(id + ".date")
                .position(12.0f, 28.0f)
                .size(columnWidth - 24.0f, 16.0f)
                .text(app::monthDayLabel(column.day))
                .fontSize(12.0f)
                .color(colors::kMuted)
                .build();

            // 竖线：把一天的节点串成一条时间线
            if (!column.entries.empty())
            {
                ui.rect(id + ".line")
                    .position(kLineX + 3.0f, kHeaderHeight + 4.0f)
                    .size(2.0f, std::max(1.0f, area.height - kHeaderHeight - 12.0f))
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
                    .radius(9.0f)
                    .color(colors::kCardBg)
                    .border(1.0f, colors::kCardBorder)
                    .build();

                // 节点圆点（类型色）
                ui.rect(itemId + ".dot")
                    .position(kLineX - 2.0f, y + 9.0f)
                    .size(10.0f, 10.0f)
                    .radius(5.0f)
                    .color(app::kindColor(entry->kind))
                    .build();

                ui.text(itemId + ".time")
                    .position(30.0f, y + 5.0f)
                    .size(columnWidth - 40.0f, 16.0f)
                    .text(app::timeLabel(*entry, *std::chrono::current_zone()))
                    .fontSize(11.0f)
                    .color(colors::kMuted)
                    .build();

                ui.text(itemId + ".title")
                    .position(30.0f, y + 21.0f)
                    .size(columnWidth - 40.0f, 24.0f)
                    .text(app::displayTitle(*entry))
                    .fontSize(13.0f)
                    .lineHeight(16.0f)
                    .wrap(true)
                    .color(colors::kText)
                    .build();

                y += kItemHeight + kItemGap;
            }

            if (column.entries.empty())
            {
                ui.text(id + ".empty")
                    .position(14.0f, kHeaderHeight + 4.0f)
                    .size(columnWidth - 28.0f, 16.0f)
                    .text("—")
                    .fontSize(12.0f)
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
        std::max(120.0f, area.height - kTopBarHeight),
    };

    const float columnWidth = (grid.width - 8.0f * 6.0f) / 7.0f;
    const auto  columns = app::buildWeek(state.entries, state.weekStart, *std::chrono::current_zone());

    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        composeDayColumn(ui, columns[i], static_cast<int>(i), grid, columnWidth);
    }
}

}  // namespace app
