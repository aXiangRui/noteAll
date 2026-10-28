// 新增面板：一个多行输入框 + 确认 / 取消。
//
// 确认之后的顺序（这是用户定的默认行为）：
//   1. addFromText：解析时间 → 落盘（毫秒级，界面不会卡）
//   2. startDraftInBackground：立刻发起模型生成草稿（约 1 秒，**后台跑**）
//   3. 生成完由 draft_runner 写回标题/摘要/标签/类型
//
// 所以"新增"是一次动作，用户不用再手动敲 note draft。

#include "pages.hpp"

#include <string>

#include "app/entry_service.hpp"
#include "core/core.hpp"
#include "draft_runner.hpp"

namespace app
{

namespace
{

constexpr float kPanelWidth = 560.0f;
constexpr float kPanelHeight = 300.0f;
constexpr float kPanelPad = 20.0f;

void composeButton(eui::Ui& ui, const std::string& id, float x, float y, float width,
                   const std::string& label, bool emphasized, std::function<void()> onClick)
{
    ui.stack(id)
        .position(x, y)
        .size(width, 44.0f)
        .onClick(std::move(onClick))
        .content([&] {
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f).size(width, 44.0f).radius(10.0f)
                .color(emphasized ? colors::kAccent : colors::kColumnBg)
                .border(1.0f, colors::kCardBorder).build();
            ui.text(id + ".label")
                .position(0.0f, 10.0f).size(width, metrics::kLineCaption)
                .text(label).fontSize(metrics::kFontCaption)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(colors::kText).build();
        })
        .build();
}

}  // namespace

void composeComposePanel(eui::Ui& ui, AppState& state, const PageRect& area)
{
    // 半透明遮罩：让底下的周视图暗下去，注意力集中在输入框
    ui.rect("compose.scrim")
        .position(area.x, area.y)
        .size(area.width, area.height)
        .radius(14.0f)
        .color({ 0.96f, 0.97f, 0.98f, 0.82f })
        .build();

    const float panelX = area.x + (area.width - kPanelWidth) * 0.5f;
    const float panelY = area.y + 40.0f;

    ui.rect("compose.bg")
        .position(panelX, panelY)
        .size(kPanelWidth, kPanelHeight)
        .radius(16.0f)
        .color(colors::kColumnBg)
        .border(1.0f, colors::kCardBorder)
        .build();

    ui.text("compose.title")
        .position(panelX + kPanelPad, panelY + 14.0f)
        .size(kPanelWidth - kPanelPad * 2.0f, metrics::kLineBody)
        .text("记一条")
        .fontSize(metrics::kFontBody)
        .color(colors::kKindEvent)
        .build();

    ui.text("compose.hint")
        .position(panelX + kPanelPad, panelY + 44.0f)
        .size(kPanelWidth - kPanelPad * 2.0f, metrics::kLineCaption)
        .text("直接写，能认出「明天下午三点」这类时间")
        .fontSize(metrics::kFontCaption)
        .color(colors::kMuted)
        .build();

    // 多行输入（IME 已验证可用）
    components::input(ui, "compose.input")
        .position(panelX + kPanelPad, panelY + 72.0f)
        .size(kPanelWidth - kPanelPad * 2.0f, 120.0f)
        .multiline(true)
        .scrollbar(true)
        .fontSize(metrics::kFontBody)
        .placeholder("比如：明天下午三点跟老张开会讨论排期")
        .value(state.composeText)
        .onChange([&state](const std::string& value) { state.composeText = value; })
        .build();

    // 错误/忙碌提示
    if (!state.composeError.empty())
    {
        ui.text("compose.error")
            .position(panelX + kPanelPad, panelY + 200.0f)
            .size(kPanelWidth - kPanelPad * 2.0f, metrics::kLineCaption)
            .text(state.composeError)
            .fontSize(metrics::kFontCaption)
            .color(colors::kDanger)
            .build();
    }
    else if (draftBusy())
    {
        ui.text("compose.busy")
            .position(panelX + kPanelPad, panelY + 200.0f)
            .size(kPanelWidth - kPanelPad * 2.0f, metrics::kLineCaption)
            .text("上一条的草稿还在生成…")
            .fontSize(metrics::kFontCaption)
            .color(colors::kMuted)
            .build();
    }

    const float buttonsY = panelY + kPanelHeight - 64.0f;
    const float buttonW = 120.0f;

    composeButton(ui, "compose.cancel", panelX + kPanelWidth - kPanelPad - buttonW, buttonsY, buttonW,
                  "取消", false, [&state] {
                      state.composing = false;
                      state.composeText.clear();
                      state.composeError.clear();
                  });

    composeButton(ui, "compose.ok", panelX + kPanelWidth - kPanelPad - buttonW * 2.0f - 12.0f,
                  buttonsY, buttonW, "确认", true, [&state] {
                      if (state.composeText.empty())
                      {
                          state.composeError = "内容不能为空";
                          return;
                      }

                      // 1) 先落盘（毫秒级）
                      core::Core note{ app::dataPath() };
                      if (auto loaded = note.load(); !loaded)
                      {
                          state.composeError = "读取数据失败: " + loaded.error();
                          return;
                      }

                      auto added = app::addFromText(note, state.composeText);
                      if (!added)
                      {
                          state.composeError = "记录失败: " + added.error();
                          return;
                      }

                      // 2) 再后台生成草稿（约 1 秒，不阻塞界面）
                      startDraftInBackground(added->id, added->raw_text);

                      state.composeText.clear();
                      state.composeError.clear();
                      state.composing = false;
                      reloadData(state);
                  });
}

}  // namespace app
