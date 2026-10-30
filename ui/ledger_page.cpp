// 账本模块：月度汇总 + 流水列表。这是「最基础的列表界面」。
//
// 刻意只做"看"：
//   * 月度汇总（支出 / 收入 / 净 / 笔数）
//   * 本月流水（日期、金额、分类、备注）
//   * 上一月 / 本月 / 下一月
// 录入暂时仍走命令行 `note ledger add`。先确认这个列表读起来顺不顺眼，
// 再决定要不要在界面上做录入（那会引出分类下拉、金额键盘、日期选择一堆东西）。

#include "pages.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include "app/entry_service.hpp"
#include "app/time_input.hpp"
#include "ledger/money.hpp"

namespace app
{

namespace
{

using namespace metrics;

constexpr float kRowHeight = 34.0f;
constexpr float kHeaderRowHeight = 30.0f;
constexpr float kCardPad = 16.0f;
constexpr float kChipHeight = 40.0f;

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

std::string moneyText(std::int64_t cents, bool income)
{
    return std::string{ income ? "+" : "-" } + ledger::formatCents(cents);
}

// 本月该月的记录（按发生时间倒序，和 CLI 一致）
std::vector<ledger::Transaction const*> monthRows(AppState const& state, std::chrono::year_month month)
{
    const auto* tz = std::chrono::current_zone();

    std::vector<ledger::Transaction const*> rows;
    for (const auto& t : state.ledgerEntries)
    {
        const std::chrono::zoned_time     zt{ tz, t.occurred };
        const std::chrono::year_month_day day{
            std::chrono::floor<std::chrono::days>(zt.get_local_time()) };
        if (std::chrono::year_month{ day.year(), day.month() } != month) { continue; }
        rows.push_back(&t);
    }

    std::sort(rows.begin(), rows.end(),
              [](ledger::Transaction const* a, ledger::Transaction const* b) {
                  return a->occurred > b->occurred;
              });
    return rows;
}

void composeSummary(eui::Ui& ui, AppState& state, const PageRect& area, float y,
                    std::int64_t expense, std::int64_t income, std::size_t count)
{
    const float width = area.width;
    const float height = 86.0f;

    ui.rect("ledger.summary.bg")
        .position(area.x, y).size(width, height)
        .radius(12.0f).color(colors::kColumnBg).border(1.0f, colors::kCardBorder).build();

    struct Cell
    {
        const char*  label;
        std::string  value;
        eui::Color   color;
    };

    const Cell cells[] = {
        { "支出", ledger::formatCents(expense), colors::kKindTodo },
        { "收入", ledger::formatCents(income), colors::kKindNote },
        { "结余", ledger::formatCents(income - expense), colors::kKindEvent },
        { "笔数", std::to_string(count), colors::kMuted },
    };

    const float cellWidth = (width - kCardPad * 2.0f) / 4.0f;
    for (int i = 0; i < 4; ++i)
    {
        const float cellX = area.x + kCardPad + static_cast<float>(i) * cellWidth;

        ui.text("ledger.summary." + std::to_string(i) + ".label")
            .position(cellX, y + 12.0f).size(cellWidth, kLineCaption)
            .text(cells[i].label).fontSize(kFontCaption).color(colors::kMuted).build();

        ui.text("ledger.summary." + std::to_string(i) + ".value")
            .position(cellX, y + 40.0f).size(cellWidth - 8.0f, kLineBody + 4.0f)
            .text(cells[i].value).fontSize(kFontHeading).color(cells[i].color).build();
    }
}

}  // namespace

void composeLedgerPage(eui::Ui& ui, AppState& state, const PageRect& area)
{
    if (!state.ledgerLoaded) { reloadLedger(state); }

    const float y0 = area.y + 4.0f;

    // ---- 顶部：标题 + 月份切换 ----
    composeChip(ui, "ledger.prev", area.x, y0, 96.0f, "‹ 上一月", [&state] {
        state.ledgerMonth -= std::chrono::months{ 1 };
    });

    composeChip(ui, "ledger.now", area.x + 104.0f, y0, 80.0f, "本月", [&state] {
        const std::chrono::zoned_time     zt{ std::chrono::current_zone(), nowUtc() };
        const std::chrono::year_month_day today{
            std::chrono::floor<std::chrono::days>(zt.get_local_time()) };
        state.ledgerMonth = std::chrono::year_month{ today.year(), today.month() };
    });

    composeChip(ui, "ledger.next", area.x + 192.0f, y0, 96.0f, "下一月 ›", [&state] {
        state.ledgerMonth += std::chrono::months{ 1 };
    });

    char monthBuffer[16];
    std::snprintf(monthBuffer, sizeof(monthBuffer), "%d-%02u",
                  static_cast<int>(state.ledgerMonth.year()),
                  static_cast<unsigned>(state.ledgerMonth.month()));

    ui.text("ledger.month")
        .position(area.x + 300.0f, y0 + 8.0f)
        .size(160.0f, kLineCaption)
        .text(monthBuffer)
        .fontSize(kFontRange)
        .color(colors::kText)
        .build();

    ui.text("ledger.file")
        .position(area.x + area.width - 320.0f, y0 + 10.0f)
        .size(320.0f, kLineCaption)
        .text("账本: " + std::string{ "ledger.jsonl" })
        .fontSize(kFontCaption)
        .color(colors::kMuted)
        .build();

    // ---- 汇总 ----
    const auto rows = monthRows(state, state.ledgerMonth);

    std::int64_t expense = 0;
    std::int64_t income  = 0;
    for (const auto* t : rows)
    {
        (t->direction == ledger::Direction::Income ? income : expense) += t->amountCents;
    }

    const float summaryY = y0 + kChipHeight + 10.0f;
    composeSummary(ui, state, area, summaryY, expense, income, rows.size());

    // ---- 列表 ----
    const float listTop = summaryY + 86.0f + 12.0f;
    const float listHeight = std::max(120.0f, area.y + area.height - listTop);

    ui.rect("ledger.list.bg")
        .position(area.x, listTop).size(area.width, listHeight)
        .radius(12.0f).color(colors::kColumnBg).border(1.0f, colors::kCardBorder).build();

    // 表头
    const float padX = 16.0f;
    const float colDate = 170.0f;    // 日期时间
    const float colMoney = 130.0f;   // 金额
    const float colCat = 110.0f;     // 分类
    const float colNote = area.width - padX * 2.0f - colDate - colMoney - colCat;

    const auto header = [&](float x, float offset, float width, const char* text) {
        ui.text(std::string{ "ledger.head." } + text)
            .position(area.x + padX + offset, listTop + 8.0f)
            .size(width, kLineCaption)
            .text(text).fontSize(kFontCaption).color(colors::kMuted).build();
    };
    (void)header;

    ui.text("ledger.head.date")
        .position(area.x + padX, listTop + 8.0f).size(colDate, kLineCaption)
        .text("时间").fontSize(kFontCaption).color(colors::kMuted).build();
    ui.text("ledger.head.money")
        .position(area.x + padX + colDate, listTop + 8.0f).size(colMoney, kLineCaption)
        .text("金额").fontSize(kFontCaption).color(colors::kMuted).build();
    ui.text("ledger.head.cat")
        .position(area.x + padX + colDate + colMoney, listTop + 8.0f).size(colCat, kLineCaption)
        .text("分类").fontSize(kFontCaption).color(colors::kMuted).build();
    ui.text("ledger.head.note")
        .position(area.x + padX + colDate + colMoney + colCat, listTop + 8.0f).size(colNote, kLineCaption)
        .text("备注").fontSize(kFontCaption).color(colors::kMuted).build();

    const float rowsTop   = listTop + kHeaderRowHeight + 4.0f;
    const float rowsMaxY  = listTop + listHeight - 6.0f;
    const auto* tz        = std::chrono::current_zone();

    float rowY = rowsTop;
    std::size_t shown = 0;
    for (const auto* t : rows)
    {
        if (rowY + kRowHeight > rowsMaxY) { break; }

        const std::string rowId = "ledger.row." + t->id;

        // 隔行底色，长列表更容易读
        if (shown % 2 == 1)
        {
            ui.rect(rowId + ".bg")
                .position(area.x + 6.0f, rowY).size(area.width - 12.0f, kRowHeight)
                .radius(6.0f).color(colors::kPageBg).build();
        }

        // 日期时间：只显示 "10-30 23:00"（年份对月度视图是噪音）
        const std::string localText = app::formatLocalDateTime(t->occurred, *tz);
        const std::string shortTime = localText.size() > 5 ? localText.substr(5) : localText;

        const bool isIncome = t->direction == ledger::Direction::Income;

        ui.text(rowId + ".date")
            .position(area.x + padX, rowY + 6.0f).size(colDate, kLineCaption)
            .text(shortTime).fontSize(kFontCaption).color(colors::kMuted).build();

        ui.text(rowId + ".money")
            .position(area.x + padX + colDate, rowY + 6.0f).size(colMoney, kLineCaption)
            .text(moneyText(t->amountCents, isIncome)).fontSize(kFontBody)
            .color(isIncome ? colors::kKindNote : colors::kKindTodo).build();

        ui.text(rowId + ".cat")
            .position(area.x + padX + colDate + colMoney, rowY + 6.0f).size(colCat, kLineCaption)
            .text(t->category).fontSize(kFontCaption).color(colors::kText).build();

        // 备注按列宽截断，保证不越界
        ui.text(rowId + ".note")
            .position(area.x + padX + colDate + colMoney + colCat, rowY + 6.0f).size(colNote, kLineCaption)
            .text(app::elideToFit(t->note, colNote, kFontCaption, 1))
            .fontSize(kFontCaption).color(colors::kText).build();

        rowY += kRowHeight;
        ++shown;
    }

    if (rows.empty())
    {
        ui.text("ledger.empty")
            .position(area.x + padX, rowsTop + 8.0f).size(area.width - padX * 2.0f, kLineCaption)
            .text("这个月还没记账（命令行：note ledger add \"午饭 32\"）")
            .fontSize(kFontCaption).color(colors::kMuted).build();
    }
    else if (shown < rows.size())
    {
        ui.text("ledger.more")
            .position(area.x + padX, rowsMaxY - kLineCaption)
            .size(area.width - padX * 2.0f, kLineCaption)
            .text("…还有 " + std::to_string(rows.size() - shown) + " 笔没显示（窗口再大点）")
            .fontSize(kFontCaption).color(colors::kMuted).build();
    }

    if (!state.ledgerError.empty())
    {
        ui.text("ledger.error")
            .position(area.x, area.y + area.height - kLineCaption)
            .size(area.width, kLineCaption)
            .text(state.ledgerError).fontSize(kFontCaption).color(colors::kDanger).build();
    }
}

}  // namespace app
