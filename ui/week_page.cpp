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

#include "app/time_input.hpp"
#include "draft_runner.hpp"
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
constexpr float kSubBarHeight = 46.0f;   // 第二行：周范围 + 搜索框
constexpr float kColumnGap = 8.0f;
constexpr float kColumnPad = 10.0f;
constexpr float kHeaderHeight = 62.0f;
constexpr float kGutterWidth = 58.0f;
constexpr float kGutterGap = 6.0f;
constexpr float kUnplacedRowHeight = 28.0f;
constexpr float kUnplacedMaxRows = 4.0f;   // 最多铺几行，多了就 "…还有 N 条"
constexpr float kUnplacedHeader = 22.0f;

// 卡片最小高度：只为了让 18px 时刻 + 20px 标题放得下。
// 它**不是**默认时长 —— 时长永远来自 end - start。
constexpr float kMinCardHeight = 64.0f;
constexpr float kCardGapNudge = 4.0f;
constexpr float kMinutesPerDay = 1440.0f;

/**
 * 打开详情所需的一切，**按值**拷好。
 *
 * 为什么不在 lambda 里捕获 entry 指针：compose 里拿到的指针指向 state.entries，
 * 而 reloadData() 会重新赋值这个 vector —— 点击发生在之后的帧里，指针可能已经悬空。
 * 按值拷贝是唯一安全的做法。
 */
struct DetailSeed
{
    std::string id;
    std::string text;
    std::string start;
    std::string end;
};

DetailSeed seedFor(core::Entry const& entry)
{
    const auto* tz = std::chrono::current_zone();

    DetailSeed seed;
    seed.id   = entry.id;
    seed.text = entry.raw_text;
    if (entry.occurred)
    {
        seed.start = app::formatLocalDateTime(entry.occurred->start, *tz);
        if (entry.occurred->end) { seed.end = app::formatLocalDateTime(*entry.occurred->end, *tz); }
    }
    return seed;
}

void applySeed(AppState& state, DetailSeed const& seed)
{
    state.selectedId = seed.id;
    state.editText   = seed.text;
    state.editStart  = seed.start;
    state.editEnd    = seed.end;
    state.aiStart.clear();   // 换了一条记录，旧的 AI 建议不再适用
    state.aiEnd.clear();
    state.timeError.clear();
    state.timeStatus.clear();
}

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

    // 补草稿：历史记录（或上次失败/中断的）一条一条补，点一次补一条。
    // 显示剩余条数，让人知道还要点几次；正在生成时显示"生成中"。
    const std::size_t pending = pendingDraftCount(state);
    if (pending > 0 && llmKeyConfigured())
    {
        const std::string label =
            draftBusy() ? "生成中…" : ("补草稿 " + std::to_string(pending));

        composeChip(ui, "nav.draft", area.x + area.width - 344.0f, y, 136.0f, label, [&state] {
            if (core::Entry const* entry = firstPendingDraft(state))
            {
                // 单条先落一次盘再生成：runner 只认 id + 原文
                startDraftInBackground(entry->id, entry->raw_text);
            }
        });
    }

    composeChip(ui, "nav.reload", area.x + area.width - 88.0f, y, 88.0f, "刷新", [&state] {
        reloadData(state);
    });

    // 第二行：周范围（左） + 搜索框（右） + 命中数
    const float subY = area.y + kTopBarHeight;
    const float searchWidth = 260.0f;
    const float searchX = area.x + area.width - searchWidth;

    ui.text("week.range")
        .position(area.x, subY + 10.0f)
        .size(std::max(120.0f, searchX - area.x - 24.0f), kLineCaption)
        .text(app::weekRangeLabel(state.weekStart))
        .fontSize(kFontRange)
        .color(colors::kText)
        .build();

    components::input(ui, "week.search")
        .position(searchX, subY)
        .size(searchWidth, 36.0f)
        .fontSize(kFontCaption)
        .placeholder("搜索：原文 / 标题 / 标签")
        .value(state.searchText)
        .onChange([&state](const std::string& value) { state.searchText = value; })
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
                    .onClick([&state, seed = seedFor(*entry)] { applySeed(state, seed); })
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

                        // 卡片内的排版：开始时间在左上、结束时间在右下、标题在中间。
                        // 宽度不写死 —— 卡片宽度随窗口变，标题必须按实际像素宽度
                        // 现算能放几个字（elideToFit），否则换窗口就溢出/被裁。
                        const float cardWidth  = columnWidth - 12.0f;
                        const float innerX     = 20.0f;
                        const float innerWidth = cardWidth - innerX - 8.0f;

                        const float startRow = kLineCaption + 4.0f;                          // 时刻行
                        const float endRow   = placement.endMinutes ? kLineCaption + 2.0f : 0.0f;
                        const float titleArea = std::max(kLineBody, height - startRow - endRow - 4.0f);
                        const int   titleLines =
                            std::max(1, static_cast<int>(titleArea / kLineBody));

                        // 上边左：开始时间
                        ui.text(itemId + ".start")
                            .position(innerX, 2.0f)
                            .size(innerWidth, kLineCaption)
                            .text(clockLabel(placement.startMinutes))
                            .fontSize(kFontCaption)
                            .color(colors::kMuted)
                            .build();

                        // 下边右：结束时间（只有真的写了结束时间才画）
                        if (placement.endMinutes)
                        {
                            ui.text(itemId + ".end")
                                .position(innerX, height - kLineCaption - 2.0f)
                                .size(innerWidth, kLineCaption)
                                .text(clockLabel(*placement.endMinutes))
                                .fontSize(kFontCaption)
                                .horizontalAlign(eui::HorizontalAlign::Right)
                                .color(colors::kMuted)
                                .build();
                        }

                        // 中间：标题（按可用宽度/行数截断，保证一定落在卡片里）
                        ui.text(itemId + ".title")
                            .position(innerX, startRow)
                            .size(innerWidth, titleArea)
                            .text(app::elideToFit(app::displayTitle(*entry), innerWidth, kFontBody,
                                                  titleLines))
                            .fontSize(kFontBody)
                            .lineHeight(kLineBody)
                            .wrap(true)
                            .color(colors::kText)
                            .build();
                    })
                    .build();
            }

            // ---- 底部：时间没识别的记录（不占 24h 轴）----
            //
            // ⚠️ 这里的每一行都必须**可点击**。
            // 以前这里只有一句"未排时间 N 条"的文字 —— 而清掉时间的记录会掉进这里，
            // 于是那条记录就再也打不开了（不能改时间、不能删）：死路。
            std::vector<core::Entry const*> unplacedEntries;
            for (const core::Entry* entry : column.entries)
            {
                if (!app::placeEntry(*entry, *std::chrono::current_zone()).hasTime)
                {
                    unplacedEntries.push_back(entry);
                }
            }

            if (!unplacedEntries.empty())
            {
                const float stripY = kHeaderHeight + axisHeight + 4.0f;
                const std::size_t shown =
                    std::min(unplacedEntries.size(), static_cast<std::size_t>(kUnplacedMaxRows));
                const float stripHeight =
                    kUnplacedHeader + static_cast<float>(shown) * kUnplacedRowHeight + 4.0f;

                ui.rect(id + ".unplaced.bg")
                    .position(6.0f, stripY)
                    .size(columnWidth - 12.0f, stripHeight)
                    .radius(9.0f)
                    .color(colors::kCardBg)
                    .border(1.0f, colors::kCardBorder)
                    .build();

                ui.text(id + ".unplaced.label")
                    .position(14.0f, stripY + 1.0f)
                    .size(columnWidth - 22.0f, kLineCaption)
                    .text("未排时间 " + std::to_string(unplacedEntries.size()) + " 条")
                    .fontSize(kFontCaption)
                    .color(colors::kMuted)
                    .build();

                for (std::size_t i = 0; i < shown; ++i)
                {
                    const core::Entry& entry    = *unplacedEntries[i];
                    const std::string   rowId   = id + ".unplaced." + entry.id;
                    const float         rowY    = stripY + kUnplacedHeader +
                                          static_cast<float>(i) * kUnplacedRowHeight;
                    const float rowWidth = columnWidth - 20.0f;

                    ui.stack(rowId)
                        .position(10.0f, rowY)
                        .size(rowWidth, kUnplacedRowHeight - 4.0f)
                        .onClick([&state, seed = seedFor(entry)] { applySeed(state, seed); })
                        .content([&] {
                            ui.rect(rowId + ".bg")
                                .position(0.0f, 0.0f)
                                .size(rowWidth, kUnplacedRowHeight - 4.0f)
                                .radius(6.0f)
                                .color(colors::kColumnBg)
                                .border(1.0f, colors::kCardBorder)
                                .build();

                            ui.rect(rowId + ".dot")
                                .position(5.0f, 6.0f)
                                .size(7.0f, 7.0f)
                                .radius(3.5f)
                                .color(app::kindColor(entry.kind))
                                .build();

                            // 标题按可用宽度截断，保证不溢出这一行
                            ui.text(rowId + ".title")
                                .position(16.0f, 1.0f)
                                .size(rowWidth - 20.0f, kUnplacedRowHeight - 6.0f)
                                .text(app::elideToFit(app::displayTitle(entry), rowWidth - 20.0f,
                                                      kFontCaption, 1))
                                .fontSize(kFontCaption)
                                .color(colors::kText)
                                .build();
                        })
                        .build();
                }

                if (unplacedEntries.size() > shown)
                {
                    ui.text(id + ".unplaced.more")
                        .position(14.0f, stripY + kUnplacedHeader +
                                             static_cast<float>(shown) * kUnplacedRowHeight - 2.0f)
                        .size(columnWidth - 22.0f, kLineCaption)
                        .text("…还有 " + std::to_string(unplacedEntries.size() - shown) + " 条")
                        .fontSize(kFontCaption)
                        .color(colors::kMuted)
                        .build();
                }
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

    const float gridTop    = area.y + kTopBarHeight + kSubBarHeight;
    const float gridHeight = std::max(160.0f, area.height - kTopBarHeight - kSubBarHeight);
    const float axisTop    = gridTop + kHeaderHeight;

    // 搜索在这里生效：过滤在数据层（app::filterEntries），视图只管画。
    const auto filtered = app::filterEntries(state.entries, state.searchText);
    const auto columns  = app::buildWeek(filtered, state.weekStart, *std::chrono::current_zone());

    // 搜索命中反馈（0 条时要明确告诉用户"是没找到"而不是"这周没记录"）
    if (!state.searchText.empty())
    {
        ui.text("week.search.count")
            .position(area.x + area.width - 380.0f, area.y + kTopBarHeight + 10.0f)
            .size(120.0f, kLineCaption)
            .text("找到 " + std::to_string(filtered.size()) + " 条")
            .fontSize(kFontCaption)
            .color(filtered.empty() ? colors::kDanger : colors::kMuted)
            .build();
    }

    // 未排时间的条：高度按"最多几列里最多的那列"来留，
    // 保证七列用的是同一条基线（不同列留不同高度会让时间轴错位）。
    std::size_t maxUnplaced = 0;
    for (const auto& column : columns)
    {
        std::size_t count = 0;
        for (const core::Entry* entry : column.entries)
        {
            if (!app::placeEntry(*entry, *std::chrono::current_zone()).hasTime) { ++count; }
        }
        maxUnplaced = std::max(maxUnplaced, count);
    }

    const std::size_t stripRows =
        std::min(maxUnplaced, static_cast<std::size_t>(kUnplacedMaxRows));
    const float stripHeight = maxUnplaced == 0 ? 0.0f
                                              : kUnplacedHeader +
                                                    static_cast<float>(stripRows) * kUnplacedRowHeight +
                                                    8.0f;

    const float axisHeight =
        std::max(80.0f, gridHeight - kHeaderHeight - 8.0f - stripHeight);

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
