// 图表页：支出/收入构成（饼图 + 图例）+ 近 6 个月趋势（柱状）。
//
// 为什么单独一页而不是塞进账本页：
//   账本页的职责是"查某一笔"，图表页的职责是"看结构"。放一起会让两件事都变挤，
//   而且图表要占宽度（两个饼 + 两组图例），在列表页里塞不下。
//
// 数据全部来自 ledger 层（summary / monthlyTotals），这一层只负责画。

#include "pages.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "app/entry_service.hpp"
#include "ledger/book.hpp"
#include "ledger/money.hpp"

namespace app
{

namespace
{

using namespace metrics;

constexpr float kChipHeight = 40.0f;
constexpr float kPieSize = 300.0f;
constexpr float kLegendWidth = 220.0f;

// 分类配色。固定顺序 + 固定颜色 = 同一个月里"餐饮"永远是同一个颜色，
// 换月对比时才不会看错（随机配色是图表的第一大坑）。
const std::vector<eui::Color>& palette()
{
    static const std::vector<eui::Color> colors = {
        { 0.33f, 0.60f, 0.89f, 1.0f },   // 蓝
        { 0.38f, 0.76f, 0.60f, 1.0f },   // 绿
        { 0.96f, 0.65f, 0.26f, 1.0f },   // 橙
        { 0.87f, 0.42f, 0.53f, 1.0f },   // 粉
        { 0.61f, 0.54f, 0.89f, 1.0f },   // 紫
        { 0.42f, 0.72f, 0.77f, 1.0f },   // 青
        { 0.80f, 0.70f, 0.36f, 1.0f },   // 金
        { 0.66f, 0.67f, 0.72f, 1.0f },   // 灰
    };
    return colors;
}

eui::Color paletteAt(std::size_t index)
{
    const auto& colors = palette();
    return colors[index % colors.size()];
}

void composeChip(eui::Ui& ui, const std::string& id, float x, float y, float width,
                 const std::string& label, std::function<void()> onClick)
{
    ui.stack(id)
        .position(x, y)
        .size(width, kChipHeight)
        .onClick([onClick = std::move(onClick)] { repaint(); onClick(); })
        .content([&] {
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f).size(width, kChipHeight).radius(9.0f)
                .color(colors::kColumnBg).border(1.0f, colors::kCardBorder).build();
            ui.text(id + ".label")
                .position(0.0f, 9.0f).size(width, kLineCaption)
                .text(label).fontSize(kFontCaption)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(colors::kText).build();
        })
        .build();
}

std::string percentText(std::int64_t part, std::int64_t total)
{
    if (total <= 0) { return "0%"; }

    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%.0f%%", 100.0 * static_cast<double>(part) /
                                                        static_cast<double>(total));
    return buffer;
}

// 一个饼 + 右侧图例（图例带金额和占比 —— 只有颜色的图例在这个场景里没用）
void composePieWithLegend(eui::Ui& ui, const std::string& id, float x, float y,
                          const std::string& title, const std::vector<ledger::CategorySum>& items,
                          std::int64_t total)
{
    std::vector<float>       values;
    std::vector<std::string> labels;
    std::vector<eui::Color>  sliceColors;

    values.reserve(items.size());
    labels.reserve(items.size());
    sliceColors.reserve(items.size());

    for (std::size_t i = 0; i < items.size(); ++i)
    {
        values.push_back(static_cast<float>(items[i].cents));
        labels.push_back(items[i].category);
        sliceColors.push_back(paletteAt(i));
    }

    // 饼图放在一个定位好的 stack 里：pieChart 自己不接受 position（它总是落在父容器原点）
    ui.stack(id + ".wrap")
        .position(x, y)
        .size(kPieSize, kPieSize)
        .content([&] {
            components::pieChart(ui, id + ".pie")
                .theme(components::theme::light())
                .size(kPieSize, kPieSize)
                .title(title)
                .values(values)
                .labels(labels)
                .colors(sliceColors)
                .build();
        })
        .build();

    // 图例
    const float legendX = x + kPieSize + 12.0f;
    float       legendY = y + 44.0f;

    if (items.empty())
    {
        ui.text(id + ".empty")
            .position(legendX, legendY).size(kLegendWidth, kLineCaption)
            .text("这个月没有记录").fontSize(kFontCaption).color(colors::kMuted).build();
        return;
    }

    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const std::string rowId = id + ".legend." + std::to_string(i);

        ui.rect(rowId + ".dot")
            .position(legendX, legendY + 5.0f).size(10.0f, 10.0f).radius(3.0f)
            .color(paletteAt(i)).build();

        ui.text(rowId + ".name")
            .position(legendX + 16.0f, legendY).size(78.0f, kLineCaption)
            .text(items[i].category).fontSize(kFontCaption).color(colors::kText).build();

        ui.text(rowId + ".amount")
            .position(legendX + 96.0f, legendY).size(84.0f, kLineCaption)
            .text(ledger::formatCents(items[i].cents)).fontSize(kFontCaption)
            .color(colors::kText).build();

        ui.text(rowId + ".pct")
            .position(legendX + 178.0f, legendY).size(44.0f, kLineCaption)
            .text(percentText(items[i].cents, total)).fontSize(kFontCaption)
            .color(colors::kMuted).build();

        legendY += kLineCaption + 4.0f;
    }

    ui.text(id + ".legend.total")
        .position(legendX, legendY + 2.0f).size(kLegendWidth, kLineCaption)
        .text("合计 " + ledger::formatCents(total)).fontSize(kFontCaption)
        .color(colors::kMuted).build();
}

// 近 N 个月支出柱状图（纯 rect，不需要额外组件）
void composeTrend(eui::Ui& ui, const std::string& id, float x, float y, float width, float height,
                  const std::vector<ledger::MonthTotal>& months)
{
    ui.rect(id + ".bg")
        .position(x, y).size(width, height).radius(12.0f)
        .color(colors::kColumnBg).border(1.0f, colors::kCardBorder).build();

    ui.text(id + ".title")
        .position(x + 16.0f, y + 10.0f).size(width - 32.0f, kLineCaption)
        .text("近 " + std::to_string(months.size()) + " 个月支出")
        .fontSize(kFontCaption).color(colors::kMuted).build();

    if (months.empty()) { return; }

    std::int64_t maxCents = 1;
    for (const auto& month : months) { maxCents = std::max(maxCents, month.expenseCents); }

    const float chartTop    = y + 40.0f;
    const float chartHeight = height - 40.0f - 26.0f;
    const float slotWidth   = (width - 32.0f) / static_cast<float>(months.size());
    const float barWidth    = std::min(46.0f, slotWidth * 0.5f);

    for (std::size_t i = 0; i < months.size(); ++i)
    {
        const float slotX = x + 16.0f + static_cast<float>(i) * slotWidth;
        const float barH  = chartHeight * static_cast<float>(months[i].expenseCents) /
                           static_cast<float>(maxCents);
        const float barX  = slotX + (slotWidth - barWidth) * 0.5f;

        ui.rect(id + ".bar." + std::to_string(i))
            .position(barX, chartTop + chartHeight - barH)
            .size(barWidth, std::max(2.0f, barH))
            .radius(4.0f)
            .color(i + 1 == months.size() ? colors::kKindTodo : paletteAt(0))
            .build();

        // 金额（柱顶）
        ui.text(id + ".barvalue." + std::to_string(i))
            .position(slotX, chartTop + chartHeight - barH - kLineCaption)
            .size(slotWidth, kLineCaption)
            .text(ledger::formatCents(months[i].expenseCents))
            .fontSize(kFontCaption)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(colors::kMuted).build();

        // 月份（柱底）
        char label[16];
        std::snprintf(label, sizeof(label), "%u月",
                      static_cast<unsigned>(months[i].month.month()));

        ui.text(id + ".barmonth." + std::to_string(i))
            .position(slotX, chartTop + chartHeight + 4.0f)
            .size(slotWidth, kLineCaption)
            .text(label).fontSize(kFontCaption)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(colors::kMuted).build();
    }
}

std::chrono::year_month currentMonth()
{
    const std::chrono::zoned_time     zt{ std::chrono::current_zone(), nowUtc() };
    const std::chrono::year_month_day today{
        std::chrono::floor<std::chrono::days>(zt.get_local_time()) };
    return std::chrono::year_month{ today.year(), today.month() };
}

}  // namespace

void composeChartsView(eui::Ui& ui, AppState& state, const PageRect& area, float topY)
{
    if (!state.ledgerLoaded) { reloadLedger(state); }

    const float y0 = topY;

    // 本月合计（用 ledger 层算，和账本页同一个口径）
    ledger::Summary summary;
    {
        ledger::Book book{ ledgerFilePath() };
        if (auto loaded = book.load(); loaded) { summary = book.summary(state.ledgerMonth); }
        else { state.ledgerError = loaded.error(); }
    }

    // ---- 两个饼 + 图例 ----
    const float pieY = y0;

    composePieWithLegend(ui, "charts.pie.expense", area.x, pieY, "支出构成",
                         summary.byCategory, summary.expenseCents);

    // 第二个饼放在右侧（宽度够就并排，不够就让它半可见 —— 窗口最小宽度已经限制了）
    const float secondX = area.x + kPieSize + kLegendWidth + 36.0f;
    composePieWithLegend(ui, "charts.pie.income", secondX, pieY, "收入构成",
                         summary.incomeByCategory, summary.incomeCents);

    // ---- 近 6 个月趋势 ----
    const float trendY = pieY + kPieSize + 16.0f;
    const float trendH = std::max(120.0f, area.y + area.height - trendY - 8.0f);

    std::vector<ledger::MonthTotal> months;
    {
        ledger::Book book{ ledgerFilePath() };
        if (auto loaded = book.load(); loaded)
        {
            months = ledger::monthlyTotals(book.list(), state.ledgerMonth, 6);
        }
    }

    composeTrend(ui, "charts.trend", area.x, trendY, area.width, trendH, months);

    if (!state.ledgerError.empty())
    {
        ui.text("charts.error")
            .position(area.x, area.y + area.height - kLineCaption).size(area.width, kLineCaption)
            .text(state.ledgerError).fontSize(kFontCaption).color(colors::kDanger).build();
    }
}

}  // namespace app
