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
#include "import/charset.hpp"
#include "import/importer.hpp"
#include "import/charset.hpp"
#include "import/importer.hpp"
#include "ledger/book.hpp"
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

namespace
{

/** 方向/分类的小胶囊（可选中）。 */
void composeToggle(eui::Ui& ui, const std::string& id, float x, float y, float width,
                   const std::string& label, bool selected, std::function<void()> onClick)
{
    const float height = 32.0f;

    ui.stack(id)
        .position(x, y)
        .size(width, height)
        .onClick([onClick = std::move(onClick)] { repaint(); onClick(); })
        .content([&] {
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f).size(width, height).radius(8.0f)
                .color(selected ? colors::kAccent : colors::kPageBg)
                .border(1.0f, colors::kCardBorder).build();
            ui.text(id + ".label")
                .position(0.0f, 5.0f).size(width, kLineCaption)
                .text(label).fontSize(kFontCaption)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(colors::kText).build();
        })
        .build();
}

/** 「+ 记账」面板：一句话搞定，其余都能覆盖。 */
void composeLedgerForm(eui::Ui& ui, AppState& state, const PageRect& area)
{
    // 遮罩
    ui.rect("ledger.form.scrim")
        .position(area.x, area.y).size(area.width, area.height)
        .radius(14.0f).color({ 0.96f, 0.97f, 0.98f, 0.82f })
        .build();

    const float panelW = std::min(620.0f, area.width - 40.0f);
    const float panelH = 336.0f;
    const float panelX = area.x + (area.width - panelW) * 0.5f;
    const float panelY = area.y + 40.0f;
    const float pad    = 18.0f;
    const float innerW = panelW - pad * 2.0f;

    ui.rect("ledger.form.bg")
        .position(panelX, panelY).size(panelW, panelH)
        .radius(16.0f).color(colors::kColumnBg).border(1.0f, colors::kCardBorder).build();

    ui.text("ledger.form.title")
        .position(panelX + pad, panelY + 14.0f).size(innerW, kLineBody)
        .text("记一笔").fontSize(kFontBody).color(colors::kKindEvent).build();

    ui.text("ledger.form.hint")
        .position(panelX + pad, panelY + 42.0f).size(innerW, kLineCaption)
        .text("一句话就行：金额写在句末（午饭 32 / 打车12块 / 工资 8000）")
        .fontSize(kFontCaption).color(colors::kMuted).build();

    components::input(ui, "ledger.form.text")
        .position(panelX + pad, panelY + 68.0f).size(innerW, 40.0f)
        .fontSize(kFontBody)
        .placeholder("午饭 32")
        .value(state.ledgerText)
        .onChange([&state](const std::string& value) {
            state.ledgerText = value;
            repaint();
        })
        .build();

    // 方向
    const float dirY = panelY + 120.0f;
    composeToggle(ui, "ledger.form.expense", panelX + pad, dirY, 88.0f, "支出",
                  !state.ledgerIncome, [&state] { state.ledgerIncome = false; });
    composeToggle(ui, "ledger.form.income", panelX + pad + 96.0f, dirY, 88.0f, "收入",
                  state.ledgerIncome, [&state] { state.ledgerIncome = true; });

    // 分类（自动 + 前几个常用）
    const auto categories = state.ledgerIncome ? ledger::incomeCategories()
                                               : ledger::expenseCategories();

    float chipX = panelX + pad + 200.0f;
    composeToggle(ui, "ledger.form.cat.auto", chipX, dirY, 68.0f, "自动",
                  state.ledgerCategory.empty(), [&state] { state.ledgerCategory.clear(); });

    chipX += 76.0f;
    const float rightEdge = panelX + panelW - pad;
    for (const auto& category : categories)
    {
        const float chipWidth = 76.0f;
        if (chipX + chipWidth > rightEdge) { break; }

        composeToggle(ui, "ledger.form.cat." + category, chipX, dirY, chipWidth, category,
                      state.ledgerCategory == category,
                      [&state, category] { state.ledgerCategory = category; });
        chipX += chipWidth + 8.0f;
    }

    // 时间（可空）
    ui.text("ledger.form.at.label")
        .position(panelX + pad, panelY + 164.0f).size(innerW, kLineCaption)
        .text("时间（留空 = 现在；格式 2026-10-30 12:00）")
        .fontSize(kFontCaption).color(colors::kMuted).build();

    components::input(ui, "ledger.form.at")
        .position(panelX + pad, panelY + 188.0f).size(innerW, 40.0f)
        .fontSize(kFontBody)
        .placeholder("现在")
        .value(state.ledgerAt)
        .onChange([&state](const std::string& value) {
            state.ledgerAt = value;
            repaint();
        })
        .build();

    // 错误
    if (!state.ledgerFormError.empty())
    {
        ui.text("ledger.form.error")
            .position(panelX + pad, panelY + 236.0f).size(innerW, kLineCaption)
            .text(state.ledgerFormError).fontSize(kFontCaption).color(colors::kDanger).build();
    }

    // 按钮
    const float buttonsY = panelY + panelH - 56.0f;
    const float buttonW  = 120.0f;

    composeToggle(ui, "ledger.form.cancel", panelX + panelW - pad - buttonW, buttonsY, buttonW,
                  "取消", false, [&state] {
                      state.ledgerComposing = false;
                      state.ledgerFormError.clear();
                  });

    composeToggle(ui, "ledger.form.ok", panelX + panelW - pad * 2.0f - buttonW * 2.0f, buttonsY,
                  buttonW, "确认", true, [&state] {
                      if (state.ledgerText.empty())
                      {
                          state.ledgerFormError = "先写一句，比如「午饭 32」";
                          return;
                      }

                      const auto parsed = ledger::parseAmountNote(state.ledgerText);
                      if (!parsed)
                      {
                          state.ledgerFormError = "没找到金额：金额写在句末，比如「午饭 32」";
                          return;
                      }

                      const auto direction = state.ledgerIncome ? ledger::Direction::Income
                                                                 : ledger::guessDirection(parsed->note);
                      const std::string category =
                          state.ledgerCategory.empty() ? ledger::guessCategory(parsed->note, direction)
                                                       : state.ledgerCategory;

                      std::optional<std::chrono::sys_seconds> occurred = nowUtc();
                      if (!state.ledgerAt.empty())
                      {
                          occurred = parseLocalDateTime(state.ledgerAt, nowUtc());
                          if (!occurred)
                          {
                              state.ledgerFormError = "时间看不懂，格式 2026-10-30 12:00";
                              return;
                          }
                      }

                      const auto file = ledgerFilePath();
                      ledger::Book book{ file };
                      if (auto loaded = book.load(); !loaded)
                      {
                          state.ledgerFormError = "读取账本失败: " + loaded.error();
                          return;
                      }

                      if (auto added = book.add(parsed->amountCents, direction, *occurred, category,
                                                parsed->note, nowUtc());
                          !added)
                      {
                          state.ledgerFormError = "记账失败: " + added.error();
                          return;
                      }

                      state.ledgerText.clear();
                      state.ledgerAt.clear();
                      state.ledgerCategory.clear();
                      state.ledgerIncome     = false;
                      state.ledgerFormError.clear();
                      state.ledgerComposing  = false;

                      reloadLedger(state);
                      repaint();
                  });
}

}  // namespace

/** 「导入账单」面板：读文件 → 预览（不写盘）→ 确认导入。 */
void composeLedgerImportPanel(eui::Ui& ui, AppState& state, const PageRect& area)
{
    ui.rect("ledger.import.scrim")
        .position(area.x, area.y).size(area.width, area.height)
        .radius(14.0f).color({ 0.96f, 0.97f, 0.98f, 0.82f })
        .build();

    const float panelW = std::min(680.0f, area.width - 40.0f);
    const float panelH = 300.0f;
    const float panelX = area.x + (area.width - panelW) * 0.5f;
    const float panelY = area.y + 40.0f;
    const float pad    = 18.0f;
    const float innerW = panelW - pad * 2.0f;

    ui.rect("ledger.import.bg")
        .position(panelX, panelY).size(panelW, panelH)
        .radius(16.0f).color(colors::kColumnBg).border(1.0f, colors::kCardBorder).build();

    ui.text("ledger.import.title")
        .position(panelX + pad, panelY + 14.0f).size(innerW, kLineBody)
        .text("导入账单").fontSize(kFontBody).color(colors::kKindEvent).build();

    ui.text("ledger.import.hint")
        .position(panelX + pad, panelY + 42.0f).size(innerW, kLineCaption)
        .text("支持微信 / 支付宝导出的 CSV（UTF-8、GBK 都能读）；先预览，确认后才写账本")
        .fontSize(kFontCaption).color(colors::kMuted).build();

    components::input(ui, "ledger.import.path")
        .position(panelX + pad, panelY + 68.0f).size(innerW, 40.0f)
        .fontSize(kFontBody)
        .placeholder("C:\\Users\\你\\Downloads\\微信支付账单.csv")
        .value(state.ledgerImportPath)
        .onChange([&state](const std::string& value) {
            state.ledgerImportPath = value;
            state.ledgerImportReady = false;   // 路径变了，旧预览作废
            repaint();
        })
        .build();

    // ---- 读取预览 ----
    composeToggle(ui, "ledger.import.read", panelX + pad, panelY + 116.0f, 120.0f, "读取预览",
                  false, [&state] {
                      state.ledgerImportError.clear();
                      state.ledgerImportInfo.clear();
                      state.ledgerImportNotes.clear();
                      state.ledgerImportReady = false;

                      if (state.ledgerImportPath.empty())
                      {
                          state.ledgerImportError = "先填文件路径";
                          return;
                      }

                      const auto text = import::readTextFile(state.ledgerImportPath);
                      if (!text)
                      {
                          state.ledgerImportError = text.error();
                          return;
                      }

                      auto parsed = import::parseBill(text->utf8);

                      ledger::Book book{ ledgerFilePath() };
                      if (auto loaded = book.load(); !loaded)
                      {
                          state.ledgerImportError = "读取账本失败: " + loaded.error();
                          return;
                      }

                      auto preview = import::importBill(book, parsed, false);
                      if (!preview)
                      {
                          state.ledgerImportError = preview.error();
                          return;
                      }

                      state.ledgerImportInfo = "编码 " + text->encoding + "　来源 " +
                                               import::toString(parsed.source) +
                                               (parsed.headerLineNumber > 0
                                                    ? "　表头第 " +
                                                          std::to_string(parsed.headerLineNumber) +
                                                          " 行"
                                                    : "") +
                                               "　" + preview->summary();

                      state.ledgerImportNotes = parsed.skipped;
                      state.ledgerImportParsed = std::move(parsed);
                      state.ledgerImportReady  = !state.ledgerImportParsed.rows.empty();

                      if (!state.ledgerImportReady && state.ledgerImportError.empty())
                      {
                          state.ledgerImportError = "这个文件里没有可导入的记录";
                      }
                      repaint();
                  });

    // ---- 预览信息 ----
    float infoY = panelY + 160.0f;

    if (!state.ledgerImportInfo.empty())
    {
        ui.text("ledger.import.info")
            .position(panelX + pad, infoY).size(innerW, kLineCaption)
            .text(state.ledgerImportInfo).fontSize(kFontCaption).color(colors::kText).build();
        infoY += kLineCaption + 4.0f;
    }

    for (std::size_t i = 0; i < state.ledgerImportNotes.size() && i < 3; ++i)
    {
        ui.text("ledger.import.note." + std::to_string(i))
            .position(panelX + pad + 12.0f, infoY).size(innerW - 12.0f, kLineCaption)
            .text(app::elideToFit("· " + state.ledgerImportNotes[i], innerW - 12.0f, kFontCaption, 1))
            .fontSize(kFontCaption).color(colors::kMuted).build();
        infoY += kLineCaption;
    }

    if (!state.ledgerImportError.empty())
    {
        ui.text("ledger.import.error")
            .position(panelX + pad, infoY).size(innerW, kLineCaption)
            .text(app::elideToFit(state.ledgerImportError, innerW, kFontCaption, 1))
            .fontSize(kFontCaption).color(colors::kDanger).build();
    }

    // ---- 按钮 ----
    const float buttonsY = panelY + panelH - 56.0f;
    const float buttonW  = 120.0f;

    composeToggle(ui, "ledger.import.cancel", panelX + panelW - pad - buttonW, buttonsY, buttonW,
                  "取消", false, [&state] {
                      state.ledgerImportOpen  = false;
                      state.ledgerImportReady = false;
                      state.ledgerImportError.clear();
                  });

    composeToggle(ui, "ledger.import.ok", panelX + panelW - pad * 2.0f - buttonW * 2.0f, buttonsY,
                  buttonW, "确认导入", true, [&state] {
                      if (!state.ledgerImportReady) { return; }

                      ledger::Book book{ ledgerFilePath() };
                      if (auto loaded = book.load(); !loaded)
                      {
                          state.ledgerImportError = "读取账本失败: " + loaded.error();
                          return;
                      }

                      auto outcome = import::importBill(book, state.ledgerImportParsed, true);
                      if (!outcome)
                      {
                          state.ledgerImportError = outcome.error();
                          return;
                      }

                      state.ledgerNotice      = "导入完成：" + outcome->summary();
                      state.ledgerImportOpen  = false;
                      state.ledgerImportReady = false;
                      state.ledgerImportInfo.clear();
                      state.ledgerImportNotes.clear();
                      state.ledgerImportParsed = {};

                      reloadLedger(state);
                      repaint();
                  });
}
void composeLedgerPage(eui::Ui& ui, AppState& state, const PageRect& area)
{
    if (!state.ledgerLoaded) { reloadLedger(state); }

    const float y0 = area.y + 4.0f;

    // ---- 顶部：视图切换 + 月份切换 ----
    // 图表是账本的**子视图**，不是独立模块 —— 两者看的是同一个月份、同一份数据。
    composeToggle(ui, "ledger.view.list", area.x, y0 + 4.0f, 72.0f, "列表",
                  state.ledgerView == LedgerView::List,
                  [&state] { state.ledgerView = LedgerView::List; });

    composeToggle(ui, "ledger.view.charts", area.x + 80.0f, y0 + 4.0f, 72.0f, "图表",
                  state.ledgerView == LedgerView::Charts,
                  [&state] { state.ledgerView = LedgerView::Charts; });

    composeChip(ui, "ledger.prev", area.x + 168.0f, y0, 96.0f, "‹ 上一月", [&state] {
        state.ledgerMonth -= std::chrono::months{ 1 };
        state.ledgerScroll = 0.0f;   // 换月后回到顶部
        state.chartsDay    = 0;      // 选中的日也要清（否则会显示上个月的某天）
    });

    composeChip(ui, "ledger.now", area.x + 272.0f, y0, 80.0f, "本月", [&state] {
        const std::chrono::zoned_time     zt{ std::chrono::current_zone(), nowUtc() };
        const std::chrono::year_month_day today{
            std::chrono::floor<std::chrono::days>(zt.get_local_time()) };
        state.ledgerMonth = std::chrono::year_month{ today.year(), today.month() };
        state.ledgerScroll = 0.0f;
        state.chartsDay    = 0;
    });

    composeChip(ui, "ledger.next", area.x + 360.0f, y0, 96.0f, "下一月 ›", [&state] {
        state.ledgerMonth += std::chrono::months{ 1 };
        state.ledgerScroll = 0.0f;
        state.chartsDay    = 0;
    });

    char monthBuffer[16];
    std::snprintf(monthBuffer, sizeof(monthBuffer), "%d-%02u",
                  static_cast<int>(state.ledgerMonth.year()),
                  static_cast<unsigned>(state.ledgerMonth.month()));

    ui.text("ledger.month")
        .position(area.x + 468.0f, y0 + 8.0f)
        .size(160.0f, kLineCaption)
        .text(monthBuffer)
        .fontSize(kFontRange)
        .color(colors::kText)
        .build();

    composeChip(ui, "ledger.import", area.x + area.width - 244.0f, y0, 116.0f, "导入账单", [&state] {
        state.ledgerImportOpen  = true;
        state.ledgerImportError.clear();
        state.ledgerImportReady = false;
    });

    composeChip(ui, "ledger.add", area.x + area.width - 116.0f, y0, 116.0f, "＋ 记账", [&state] {
        state.ledgerComposing = true;
        state.ledgerFormError.clear();
    });

    ui.text("ledger.file")
        .position(area.x + area.width - 470.0f, y0 + 10.0f)
        .size(340.0f, kLineCaption)
        .text("账本: " + std::string{ "ledger.jsonl" })
        .fontSize(kFontCaption)
        .color(colors::kMuted)
        .build();

    const float contentTop = y0 + kChipHeight + 12.0f;

    if (state.ledgerView == LedgerView::Charts)
    {
        composeChartsView(ui, state, area, contentTop);
    }
    else
    {
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

    const float rowsTop    = listTop + kHeaderRowHeight + 4.0f;
    const float rowsHeight = std::max(40.0f, listTop + listHeight - rowsTop - 6.0f);
    const auto* tz         = std::chrono::current_zone();

    // ---- 可滚动列表 ----
    // 为什么不用 components::scrollView：它内部是 column 流式布局（wrapContent），
    // 而我们的 UI 全部绝对定位，混用会被列布局覆盖 y。
    // 这里直接给 stack 挂 scrollState：滚轮/触摸由运行时处理，偏移量交回我们的 state，
    // 子元素坐标 = 原始 y - 偏移量。clamp 自己做，避免越界空白。
    const float contentHeight = static_cast<float>(rows.size()) * kRowHeight;
    const float maxOffset     = std::max(0.0f, contentHeight - rowsHeight);

    state.ledgerScroll = std::clamp(state.ledgerScroll, 0.0f, maxOffset);

    ui.stack("ledger.rows")
        .position(area.x, rowsTop)
        .size(area.width, rowsHeight)
        .clip()
        .scrollState("ledger.rows", state.ledgerScroll, maxOffset, kRowHeight * 1.5f)
        .onScrollOffsetChanged([&state](float value) {
            state.ledgerScroll = value;
            repaint();
        })
        .content([&] {
            float rowY = -state.ledgerScroll;
            std::size_t index = 0;

            for (const auto* t : rows)
            {
                const std::string rowId = "ledger.row." + t->id;

                // 隔行底色，长列表更容易读
                if (index % 2 == 1)
                {
                    ui.rect(rowId + ".bg")
                        .position(6.0f, rowY).size(area.width - 12.0f, kRowHeight)
                        .radius(6.0f).color(colors::kPageBg).build();
                }

                // 日期时间：只显示 "10-30 23:00"（年份对月度视图是噪音）
                const std::string localText = app::formatLocalDateTime(t->occurred, *tz);
                const std::string shortTime =
                    localText.size() > 5 ? localText.substr(5) : localText;

                const bool isIncome = t->direction == ledger::Direction::Income;

                ui.text(rowId + ".date")
                    .position(padX, rowY + 6.0f).size(colDate, kLineCaption)
                    .text(shortTime).fontSize(kFontCaption).color(colors::kMuted).build();

                ui.text(rowId + ".money")
                    .position(padX + colDate, rowY + 6.0f).size(colMoney, kLineCaption)
                    .text(moneyText(t->amountCents, isIncome)).fontSize(kFontBody)
                    .color(isIncome ? colors::kKindNote : colors::kKindTodo).build();

                ui.text(rowId + ".cat")
                    .position(padX + colDate + colMoney, rowY + 6.0f).size(colCat, kLineCaption)
                    .text(t->category).fontSize(kFontCaption).color(colors::kText).build();

                // 备注按列宽截断，保证不越界
                ui.text(rowId + ".note")
                    .position(padX + colDate + colMoney + colCat, rowY + 6.0f)
                    .size(colNote, kLineCaption)
                    .text(app::elideToFit(t->note, colNote, kFontCaption, 1))
                    .fontSize(kFontCaption).color(colors::kText).build();

                rowY += kRowHeight;
                ++index;
            }
        })
        .build();

    // 滚动条（只在内容超出时画）：位置按 offset / maxOffset 比例
    if (maxOffset > 0.5f)
    {
        const float trackX     = area.x + area.width - 10.0f;
        const float trackY     = rowsTop + 4.0f;
        const float trackH     = rowsHeight - 8.0f;
        const float thumbH     = std::max(28.0f, trackH * (rowsHeight / contentHeight));
        const float thumbTravel = trackH - thumbH;
        const float thumbY     = trackY + thumbTravel * (state.ledgerScroll / maxOffset);

        ui.rect("ledger.scrollbar.thumb")
            .position(trackX, thumbY).size(4.0f, thumbH).radius(2.0f)
            .color(colors::kAccent).build();
    }

    if (rows.empty())
    {
        ui.text("ledger.empty")
            .position(area.x + padX, rowsTop + 8.0f).size(area.width - padX * 2.0f, kLineCaption)
            .text("这个月还没记账（命令行：note ledger add \"午饭 32\"）")
            .fontSize(kFontCaption).color(colors::kMuted).build();
    }

    }   // ← 列表视图到这里结束

    // 导入完成的结果提示（一次性）
    if (!state.ledgerNotice.empty())
    {
        ui.text("ledger.notice")
            .position(area.x, area.y + area.height - kLineCaption)
            .size(area.width, kLineCaption)
            .text(state.ledgerNotice).fontSize(kFontCaption).color(colors::kKindNote).build();
    }

    if (state.ledgerComposing) { composeLedgerForm(ui, state, area); }
    if (state.ledgerImportOpen) { composeLedgerImportPanel(ui, state, area); }

    if (!state.ledgerError.empty())
    {
        ui.text("ledger.error")
            .position(area.x, area.y + area.height - kLineCaption)
            .size(area.width, kLineCaption)
            .text(state.ledgerError).fontSize(kFontCaption).color(colors::kDanger).build();
    }
}

}  // namespace app
