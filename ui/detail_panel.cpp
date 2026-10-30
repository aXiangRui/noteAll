// 详情面板：点开一张卡片之后，显示完整内容，并且**可以改、可以删**。
//
// 为什么要有它：
//   时间轴是"扫一眼"的地方，卡片上只该有标题；
//   但记事软件最常做的事是"改错"和"删掉" —— 所以详情面板是唯一能编辑的地方。
//
// 编辑的语义（重要）：改的是**原文**。保存后：
//   1. 重新解析时间（occurred 全部重算，和 add 走同一个函数）
//   2. 草稿状态打回 pending（标题/摘要都是基于原文的，必须重生成）
//   3. 立刻发起一次后台草稿生成
// 这样"改一句话"和"重新记一条"的结果完全一致，不会出现两套语义。

#include "pages.hpp"

#include <string>

#include "app/entry_service.hpp"
#include "core/core.hpp"
#include "draft_runner.hpp"

namespace app
{

namespace
{

using namespace metrics;

constexpr float kPanelWidth = 380.0f;
constexpr float kPanelPad = 18.0f;
constexpr float kButtonHeight = 40.0f;

void composeButton(eui::Ui& ui, const std::string& id, float x, float y, float width,
                   const std::string& label, bool emphasized, std::function<void()> onClick)
{
    ui.stack(id)
        .position(x, y)
        .size(width, kButtonHeight)
        .onClick(std::move(onClick))
        .content([&] {
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f).size(width, kButtonHeight).radius(10.0f)
                .color(emphasized ? colors::kAccent : colors::kColumnBg)
                .border(1.0f, colors::kCardBorder).build();
            ui.text(id + ".label")
                .position(0.0f, 8.0f).size(width, kLineCaption)
                .text(label).fontSize(kFontCaption)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(colors::kText).build();
        })
        .build();
}

void metaLine(eui::Ui& ui, const std::string& id, float x, float y, float width,
              const std::string& text)
{
    ui.text(id)
        .position(x, y)
        .size(width, kLineCaption)
        .text(text)
        .fontSize(kFontCaption)
        .color(colors::kMuted)
        .build();
}

}  // namespace

void composeDetailPanel(eui::Ui& ui, AppState& state, const PageRect& area)
{
    // 找到选中的那条（可能已经被删掉了 —— 那就自动关掉面板）
    const core::Entry* selected = nullptr;
    for (const auto& entry : state.entries)
    {
        if (entry.id == state.selectedId)
        {
            selected = &entry;
            break;
        }
    }

    if (selected == nullptr)
    {
        state.selectedId.clear();
        return;
    }

    const bool deleted = selected->deleted_at.has_value();

    const float panelX   = area.x + area.width - kPanelWidth;
    const float panelY   = area.y;
    const float panelW   = kPanelWidth;
    const float panelH   = area.height;
    const float contentX = panelX + kPanelPad;
    const float contentW = panelW - kPanelPad * 2.0f;

    ui.rect("detail.bg")
        .position(panelX, panelY)
        .size(panelW, panelH)
        .radius(14.0f)
        .color(colors::kPageBg)
        .border(1.0f, colors::kCardBorder)
        .build();

    // ---- 顶部：标题 + 关闭 ----
    ui.text("detail.title")
        .position(contentX, panelY + 14.0f)
        .size(contentW - 56.0f, kLineTitle)
        .text(app::displayTitle(*selected))
        .fontSize(kFontRange)
        .lineHeight(kLineTitle)
        .wrap(true)
        .color(colors::kText)
        .build();

    composeButton(ui, "detail.close", panelX + panelW - kPanelPad - 40.0f, panelY + 14.0f, 40.0f,
                  "✕", false, [&state] { state.selectedId.clear(); });

    // ---- 元信息 ----
    float y = panelY + 14.0f + kLineTitle + 8.0f;

    metaLine(ui, "detail.time", contentX, y, contentW,
             "时间：" + app::timeLabel(*selected, *std::chrono::current_zone()));
    y += kLineCaption + 2.0f;

    std::string tagsText;
    for (std::size_t i = 0; i < selected->tags.size(); ++i)
    {
        tagsText += (i == 0 ? "" : "、") + selected->tags[i];
    }

    metaLine(ui, "detail.kind", contentX, y, contentW,
             "类型：" + core::toString(selected->kind) + "　标签：" +
                 (tagsText.empty() ? "（无）" : tagsText));
    y += kLineCaption + 2.0f;

    metaLine(ui, "detail.draft", contentX, y, contentW,
             "草稿：" + core::toString(selected->draft_status) +
                 (selected->draft_error.empty() ? "" : "（" + selected->draft_error + "）"));
    y += kLineCaption + 10.0f;

    if (deleted)
    {
        metaLine(ui, "detail.deleted", contentX, y, contentW, "这条记录已删除（软删除，可恢复）");
        y += kLineCaption + 6.0f;
    }

    // ---- 编辑区：改的是原文 ----
    ui.text("detail.edit.label")
        .position(contentX, y)
        .size(contentW, kLineCaption)
        .text("内容（改完点保存：会重新解析时间并重生成标题）")
        .fontSize(kFontCaption)
        .color(colors::kMuted)
        .build();
    y += kLineCaption + 2.0f;

    const float buttonsY   = panelY + panelH - kButtonHeight - kPanelPad;
    const float editHeight = std::max(120.0f, buttonsY - y - 12.0f);

    components::input(ui, "detail.edit")
        .position(contentX, y)
        .size(contentW, editHeight)
        .multiline(true)
        .scrollbar(true)
        .fontSize(kFontBody)
        .value(state.editText)
        .onChange([&state](const std::string& value) { state.editText = value; })
        .build();

    // ---- 底部按钮 ----
    const float deleteW = 96.0f;
    const float redraftW = 116.0f;
    const float saveW = 88.0f;

    if (deleted)
    {
        composeButton(ui, "detail.restore", contentX, buttonsY, deleteW, "恢复", false,
                      [&state, id = selected->id] {
                          core::Core note{ app::dataPath() };
                          if (auto loaded = note.load(); !loaded) { state.error = loaded.error(); return; }
                          if (auto ok = note.restore(id); !ok) { state.error = ok.error(); return; }
                          reloadData(state);
                      });
    }
    else
    {
        composeButton(ui, "detail.delete", contentX, buttonsY, deleteW, "删除", false,
                      [&state, id = selected->id] {
                          core::Core note{ app::dataPath() };
                          if (auto loaded = note.load(); !loaded) { state.error = loaded.error(); return; }
                          if (auto ok = note.remove(id); !ok) { state.error = ok.error(); return; }
                          reloadData(state);
                      });

        composeButton(ui, "detail.redraft", contentX + deleteW + 8.0f, buttonsY, redraftW,
                      "重生成标题", false, [&state, id = selected->id, raw = selected->raw_text] {
                          startDraftInBackground(id, raw);
                      });
    }

    // 保存：改原文 → 重算 occurred → 打回 pending → 后台重生成
    composeButton(ui, "detail.save", panelX + panelW - kPanelPad - saveW, buttonsY, saveW, "保存",
                  true, [&state, id = selected->id] {
                      if (state.editText.empty())
                      {
                          state.error = "内容不能为空";
                          return;
                      }

                      core::Core note{ app::dataPath() };
                      if (auto loaded = note.load(); !loaded) { state.error = loaded.error(); return; }

                      auto found = note.findAny(id);
                      if (!found) { state.error = "找不到这条记录"; return; }

                      core::Entry entry = *found;
                      entry.raw_text    = state.editText;
                      entry.occurred    = app::occurredFromText(entry.raw_text, entry.created_at);

                      // 原文变了，原来的标题/摘要就是过期数据 —— 打回 pending 等重生成。
                      entry.draft_status   = core::DraftStatus::Pending;
                      entry.draft_attempts = 0;
                      entry.draft_error.clear();

                      if (auto updated = note.update(entry); !updated)
                      {
                          state.error = updated.error();
                          return;
                      }

                      reloadData(state);
                      startDraftInBackground(entry.id, entry.raw_text);
                  });
}

}  // namespace app
