// 周视图模块：一周七列，每列是一条严格的 24 小时时间轴。
//
// 和上一版的区别：
//   * 不再是"等高卡片堆"，而是 00:00 → 24:00 的真实刻度尺；
//   * 卡片**上边 = 开始时间，下边 = 结束时间**；
//   * 没有结束时间就**绝不假设 1 小时** —— 那种记录只画一个最小高度的"点"，
//     高度只是为了看得见，不代表时长。
//
// 布局备忘（字号 18–28，字都从 metrics 取）：
//   左侧 58px 刻度槽（00:00 / 06:00 / 12:00 / 18:00 / 24:00）
//   七列等宽；列内 24h 轴高度 = 列高 - 列头 - 底部"未排时间"条

#include "pages.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace app
{

namespace
{

using namespace metrics;

constexpr float kChipHeight = 40.0f;
constexpr float kChipLabelTop = 9.0f;
constexpr float kTopBarHeight = 58.0f;
constexpr float kColumnGap = 8.0f;
constexpr float kColumnPad = 10.0f;
constexpr float kHeaderHeight = 62.0f;
constexpr float kGutterWidth = 58.0f;
constexpr float kGutterGap = 6.0f;
constexpr float kUnplacedHeight = 54.0f;

// 卡片最小高度：只为了让 18px 时刻 + 20px 标题放得下。
// 它**不是**默认时长 —— 时长永远来自 end - start。
constexpr float kMinCardHeight = 56.0f;
constexpr float kCardGapNudge = 4.0f;
constexpr float kMinutesPerDay = 1440.0f;

void composeChip(eui::Ui& ui, const std::string& id, float x, float y, float width,
                 const std::string& label, std::function<void()> onClick)
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
                .color(colors::kColumnBg)
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

    composeChip(ui, "nav.prev", area.x, y, 108.0f, "‹ 上一周", [&state] {
        state.weekStart = std::chrono::year_month_day{
            std::chrono::sys_days{ state.weekStart } - std::chrono::days{ 7 } };
    });
    composeChip(ui, "nav.today", area.x + 116.0f, y, 88.0f, "本周", [&state] {
        state.weekStart = mondayOf(todayLocal());
    });
    composeChip(ui, "nav.next", area.x + 212.0f, y, 108.0f, "下一周 ›", [&state] {
        state.weekStart = std::chrono::year_month_day{
            std::chrono::sys_days{ state.weekStart } + std::chrono::days{ 7 } };
    });
    composeChip(ui, "nav.add", area.x + area.width - 196.0f, y, 96.0f, "＋ 新增", [&state] {
        state.composing = true;
        state.composeText.clear();
        state.composeError.clear();
    });

    composeChip(ui, "nav.reload", area.x + area.width - 88.0f, y, 88.0f, "刷新", [&state] {
        reloadData(state);
    });

    ui.text("week.range")
        .position(area.x + 336.0f, y + 7.0f)
        .size(std::max(120.0f, area.width - 336.0f - 220.0f), kLineCaption)
        .text(app::weekRangeLabel(state.weekStart))
        .fontSize(kFontRange)
        .color(colors::kText)
        .build();
}

/** "HH:MM" —— 刻度与卡片里的时刻都用它。 */
std::string clockLabel(int minutes)
{
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes / 60, minutes % 60);
    return buffer;
}

void composeGutter(eui::Ui& ui, const PageRect& area, float axisTop, float axisHeight)
{
    // 每 6 小时一个刻度（0 / 6 / 12 / 18 / 24）
    for (int hour = 0; hour <= 24; hour += 6)
    {
        const float y = axisTop + axisHeight * static_cast<float>(hour * 60) / kMinutesPerDay;

        ui.text("gutter." + std::to_string(hour))
            .position(area.x, y - kLineCaption * 0.5f)
            .size(kGutterWidth, kLineCaption)
            .text(clockLabel(hour * 60))
            .fontSize(kFontCaption)
            .color(colors::kMuted)
            .build();
    }
}

void composeDayColumn(eui::Ui& ui, AppState& state, const DayColumn& column, int index,
                      const PageRect& area, float columnWidth, float axisTop, float axisHeight)
{
    const std::string id = "day" + std::to_string(index);
    const bool        isToday = column.day == todayLocal();
    const float       minutesPerPixel = axisHeight / kMinutesPerDay;

    ui.stack(id)
        .position(area.x, area.y)
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
                .position(kColumnPad, 8.0f)
                .size(columnWidth - kColumnPad * 2.0f, kLineBody)
                .text(app::weekdayLabel(std::chrono::weekday{ std::chrono::sys_days{ column.day } }))
                .fontSize(kFontBody)
                .color(isToday ? colors::kKindEvent : colors::kText)
                .build();

            ui.text(id + ".date")
                .position(kColumnPad, 34.0f)
                .size(columnWidth - kColumnPad * 2.0f, kLineCaption)
                .text(app::monthDayLabel(column.day))
                .fontSize(kFontCaption)
                .color(colors::kMuted)
                .build();

            // ---- 24h 刻度线（每 3 小时一条淡线）----
            for (int hour = 0; hour <= 21; hour += 3)
            {
                const float lineY =
                    kHeaderHeight + axisHeight * static_cast<float>(hour * 60) / kMinutesPerDay;

                ui.rect(id + ".grid." + std::to_string(hour))
                    .position(0.0f, lineY)
                    .size(columnWidth, 1.0f)
                    .color(colors::kLine)
                    .build();
            }

            // ---- 卡片：上边 = 开始，下边 = 结束 ----
            float lastBottom = kHeaderHeight;

            for (const core::Entry* entry : column.entries)
            {
                const DayPlacement placement = app::placeEntry(*entry, *std::chrono::current_zone());
                if (!placement.hasTime)
                {
                    continue;   // 时间没识别的画在底部"未排"条里
                }

                const float startY =
                    kHeaderHeight + static_cast<float>(placement.startMinutes) * minutesPerPixel;

                // 有结束时间 → 高度按时长算；没有 → 最小可见高度（不代表 1 小时）
                float height = kMinCardHeight;
                if (placement.endMinutes)
                {
                    const float span =
                        static_cast<float>(*placement.endMinutes - placement.startMinutes);
                    height = std::max(kMinCardHeight, span * minutesPerPixel);
                }

                // 轻微重叠处理：按开始时间排序后若压到上一张，就往下推一点。
                // 位置仍以时间为主；挤在一起时宁可略偏，也不要叠到看不清。
                float y = startY;
                if (y < lastBottom + kCardGapNudge)
                {
                    y = lastBottom + kCardGapNudge;
                }
                if (y + height > kHeaderHeight + axisHeight)
                {
                    break;   // 超出轴底先截断（列内滚动留给下一步）
                }
                lastBottom = y + height;

                const std::string itemId = id + "." + entry->id;

                // 整张卡片是一个可点区域：外层 stack 收口，onClick 挂在它身上。
                // 卡片里**只放标题**（有摘要用摘要，没有就截断原文）；
                // 完整内容点开才看 —— 列表要能扫，详情才要读。
                ui.stack(itemId)
                    .position(6.0f, y)
                    .size(columnWidth - 12.0f, height)
                    .onClick([&state, entryId = entry->id] { state.selectedId = entryId; })
                    .content([&] {
                        ui.rect(itemId + ".card")
                            .position(0.0f, 0.0f)
                            .size(columnWidth - 12.0f, height)
                            .radius(10.0f)
                            .color(colors::kCardBg)
                            .border(1.0f, colors::kCardBorder)
                            .build();

                        ui.rect(itemId + ".dot")
                            .position(6.0f, 8.0f)
                            .size(8.0f, 8.0f)
                            .radius(4.0f)
                            .color(app::kindColor(entry->kind))
                            .build();

                        // 上边：开始时间
                        ui.text(itemId + ".start")
                            .position(20.0f, 2.0f)
                            .size(columnWidth - 34.0f, kLineCaption)
                            .text(clockLabel(placement.startMinutes))
                            .fontSize(kFontCaption)
                            .color(colors::kMuted)
                            .build();

                        // 下边：结束时间（只有真的写了结束时间才画）
                        if (placement.endMinutes)
                        {
                            ui.text(itemId + ".end")
                                .position(20.0f, height - kLineCaption - 2.0f)
                                .size(columnWidth - 34.0f, kLineCaption)
                                .text(clockLabel(*placement.endMinutes))
                                .fontSize(kFontCaption)
                                .color(colors::kMuted)
                                .build();
                        }

                        // 中间：标题（只有标题）
                        const float reserved   = placement.endMinutes ? kLineCaption + 2.0f : 2.0f;
                        const float titleHeight = std::max(kLineBody, height - kLineCaption - 6.0f - reserved);

                        ui.text(itemId + ".title")
                            .position(20.0f, kLineCaption + 4.0f)
                            .size(columnWidth - 34.0f, titleHeight)
                            .text(app::cardTitle(*entry))
                            .fontSize(kFontBody)
                            .lineHeight(kLineBody)
                            .wrap(true)
                            .color(colors::kText)
                            .build();
                    })
                    .build();
            }

            // ---- 底部：时间没识别的记录（不占 24h 轴）----
            std::size_t unplaced = 0;
            std::string firstUnplaced;
            for (const core::Entry* entry : column.entries)
            {
                if (!app::placeEntry(*entry, *std::chrono::current_zone()).hasTime)
                {
                    if (unplaced == 0) { firstUnplaced = app::displayTitle(*entry); }
                    ++unplaced;
                }
            }

            if (unplaced > 0)
            {
                const float stripY = kHeaderHeight + axisHeight + 4.0f;

                ui.rect(id + ".unplaced.bg")
                    .position(6.0f, stripY)
                    .size(columnWidth - 12.0f, kUnplacedHeight - 8.0f)
                    .radius(9.0f)
                    .color(colors::kCardBg)
                    .border(1.0f, colors::kCardBorder)
                    .build();

                ui.text(id + ".unplaced.label")
                    .position(14.0f, stripY + 2.0f)
                    .size(columnWidth - 22.0f, kLineCaption)
                    .text("未排时间 " + std::to_string(unplaced) + " 条")
                    .fontSize(kFontCaption)
                    .color(colors::kMuted)
                    .build();

                ui.text(id + ".unplaced.first")
                    .position(14.0f, stripY + 2.0f + kLineCaption)
                    .size(columnWidth - 22.0f, kLineCaption)
                    .text(firstUnplaced)
                    .fontSize(kFontCaption)
                    .color(colors::kText)
                    .build();
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

    const float gridTop    = area.y + kTopBarHeight;
    const float gridHeight = std::max(160.0f, area.height - kTopBarHeight);
    const float axisTop    = gridTop + kHeaderHeight;

    const auto columns = app::buildWeek(state.entries, state.weekStart, *std::chrono::current_zone());

    // 只要有一列存在"未排时间"的记录，就在轴下面留一条 54px 的条
    bool anyUnplaced = false;
    for (const auto& column : columns)
    {
        for (const core::Entry* entry : column.entries)
        {
            if (!app::placeEntry(*entry, *std::chrono::current_zone()).hasTime)
            {
                anyUnplaced = true;
                break;
            }
        }
    }

    const float axisHeight =
        std::max(80.0f, gridHeight - kHeaderHeight - 8.0f - (anyUnplaced ? kUnplacedHeight : 0.0f));

    composeGutter(ui, PageRect{ area.x, gridTop, kGutterWidth, gridHeight }, axisTop, axisHeight);

    const float columnsX     = area.x + kGutterWidth + kGutterGap;
    const float columnsWidth = std::max(280.0f, area.width - kGutterWidth - kGutterGap);
    const float columnWidth  = (columnsWidth - kColumnGap * 6.0f) / 7.0f;

    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        const PageRect columnArea{
            columnsX + static_cast<float>(i) * (columnWidth + kColumnGap),
            gridTop,
            columnWidth,
            gridHeight,
        };

        composeDayColumn(ui, state, columns[i], static_cast<int>(i), columnArea, columnWidth, axisTop,
                         axisHeight);
    }
}

}  // namespace app
