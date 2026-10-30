// 详情面板：点开一张卡片之后，显示完整内容，并且可以改内容、改时间、删除。
//
// 三个编辑动作，语义各不相同（别混淆）：
//   1. 保存内容：改**原文** → 重新解析时间 → 草稿打回 pending → 后台重生成标题
//   2. 保存时间：只改 occurred（直接写 UTC），**不动原文** —— 手改时间优先于解析
//   3. AI 时间草稿（timeDraft）：让模型推一个时间，**只填进输入框**，
//      用户看一眼再点保存 —— 时间改错比标题改错严重得多，不能自动落盘。

#include "pages.hpp"

#include <chrono>
#include <optional>
#include <string>

#include "app/corpus.hpp"
#include "app/entry_service.hpp"
#include "app/time_input.hpp"
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
constexpr float kInputHeight = 36.0f;

void composeButton(eui::Ui& ui, const std::string& id, float x, float y, float width,
                   const std::string& label, bool emphasized, std::function<void()> onClick)
{
    ui.stack(id)
        .position(x, y)
        .size(width, kButtonHeight)
        .onClick([onClick = std::move(onClick)] { repaint(); onClick(); })
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
              const std::string& text, bool danger = false)
{
    ui.text(id)
        .position(x, y)
        .size(width, kLineCaption)
        .text(text)
        .fontSize(kFontCaption)
        .color(danger ? colors::kDanger : colors::kMuted)
        .build();
}

void fieldLabel(eui::Ui& ui, const std::string& id, float x, float y, float width,
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

    const bool  deleted = selected->deleted_at.has_value();
    const auto* tz      = std::chrono::current_zone();

    const float panelX   = area.x + area.width - kPanelWidth;
    const float panelY   = area.y;
    const float panelW   = kPanelWidth;
    const float panelH   = area.height;
    const float contentX = panelX + kPanelPad;
    const float contentW = panelW - kPanelPad * 2.0f;

    ui.rect("detail.bg")
        .position(panelX, panelY).size(panelW, panelH).radius(14.0f)
        .color(colors::kPageBg).border(1.0f, colors::kCardBorder).build();

    ui.text("detail.title")
        .position(contentX, panelY + 14.0f)
        .size(contentW - 56.0f, kLineTitle)
        .text(app::displayTitle(*selected))
        .fontSize(kFontRange).lineHeight(kLineTitle).wrap(true)
        .color(colors::kText).build();

    composeButton(ui, "detail.close", panelX + panelW - kPanelPad - 40.0f, panelY + 14.0f, 40.0f,
                  "✕", false, [&state] { state.selectedId.clear(); });

    float y = panelY + 14.0f + kLineTitle + 8.0f;

    // ---- 元信息 ----
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
    y += kLineCaption + 4.0f;

    if (deleted)
    {
        metaLine(ui, "detail.deleted", contentX, y, contentW, "这条已删除（软删除，可恢复）");
        y += kLineCaption + 4.0f;
    }

    // ---- 时间（可直接改 / 清除 / 让模型推一个）----
    fieldLabel(ui, "detail.time.label", contentX, y, contentW,
               "时间（本地，格式 2026-10-30 08:30；结束留空 = 没有结束）");
    y += kLineCaption + 2.0f;

    const float startW = contentW * 0.56f;
    const float endW   = contentW - startW - 8.0f;

    components::input(ui, "detail.time.start")
        .position(contentX, y).size(startW, kInputHeight)
        .fontSize(kFontCaption).placeholder("开始")
        .value(state.editStart)
        .onChange([&state](const std::string& value) { state.editStart = value; })
        .build();

    components::input(ui, "detail.time.end")
        .position(contentX + startW + 8.0f, y).size(endW, kInputHeight)
        .fontSize(kFontCaption).placeholder("结束（可空）")
        .value(state.editEnd)
        .onChange([&state](const std::string& value) { state.editEnd = value; })
        .build();
    y += kInputHeight + 6.0f;

    const float saveTimeW = 100.0f;
    const float clearW    = 88.0f;
    const float aiTimeW   = 116.0f;

    composeButton(ui, "detail.time.save", contentX, y, saveTimeW, "保存时间", true,
                  [&state, id = selected->id] {
                      core::Core note{ app::dataPath() };
                      if (auto loaded = note.load(); !loaded) { state.error = loaded.error(); return; }

                      auto found = note.findAny(id);
                      if (!found) { state.error = "找不到这条记录"; return; }

                      const auto startUtc = app::parseLocalDateTime(state.editStart, found->created_at);
                      if (!startUtc)
                      {
                          state.timeError = "开始时间看不懂，格式：2026-10-30 08:30";
                          return;
                      }

                      std::optional<std::chrono::sys_seconds> endUtc;
                      if (!state.editEnd.empty())
                      {
                          endUtc = app::parseLocalDateTime(state.editEnd, found->created_at);
                          if (!endUtc)
                          {
                              state.timeError = "结束时间看不懂，格式：2026-10-30 10:00";
                              return;
                          }
                          if (*endUtc <= *startUtc)
                          {
                              state.timeError = "结束时间必须晚于开始时间";
                              return;
                          }
                      }

                      // 判断这次保存是"采纳 AI 建议"还是"手改"：
                      // 文本和 AI 给的一模一样才算采纳（用户动过就不算）。
                      const bool fromAi = !state.aiStart.empty() &&
                                          state.editStart == state.aiStart &&
                                          state.editEnd == state.aiEnd;

                      core::Entry entry   = *found;
                      entry.occurred      = core::TimeRange{ *startUtc, endUtc };
                      entry.occurred_source = fromAi ? core::TimeSource::Ai
                                                     : core::TimeSource::Manual;

                      if (auto updated = note.update(entry); !updated)
                      {
                          state.error = updated.error();
                          return;
                      }

                      // ---- 回读校验 ----
                      // 只写不验的话，"看起来没保存"就只能是猜。
                      // 这里把磁盘上的真值读回来对一遍：不一致就把原因摆在面板上。
                      core::Core verify{ app::dataPath() };
                      if (auto loaded = verify.load(); !loaded)
                      {
                          state.timeError = "保存后读回失败: " + loaded.error();
                          return;
                      }

                      const auto stored = verify.findAny(id);
                      if (!stored || !stored->occurred)
                      {
                          state.timeError = "保存后读回不一致：磁盘上这条没有时间了";
                          return;
                      }

                      const bool sameStart = stored->occurred->start == *startUtc;
                      const bool sameEnd   = stored->occurred->end.has_value() == endUtc.has_value() &&
                                           (!endUtc || *stored->occurred->end == *endUtc);

                      if (!sameStart || !sameEnd)
                      {
                          state.timeError = std::string{"保存后读回不一致（磁盘="} +
                                            app::formatLocalDateTime(stored->occurred->start,
                                                                     *std::chrono::current_zone()) +
                                            "）—— 可能有别的窗口/进程覆盖了它";
                          return;
                      }

                      // 采纳 AI 的时间 = 一条真实语料：存下来，以后拿来补规则。
                      if (fromAi)
                      {
                          app::appendParseCase("tests/corpus/parse-cases.txt",
                                               app::ParseCase{ entry.raw_text, entry.created_at,
                                                               *startUtc, endUtc },
                                               *std::chrono::current_zone());
                      }

                      // 输入框对齐磁盘真值（避免"面板显示的和存的不一样"）
                      state.editStart = app::formatLocalDateTime(*startUtc, *std::chrono::current_zone());
                      state.editEnd   = endUtc ? app::formatLocalDateTime(*endUtc, *std::chrono::current_zone())
                                               : std::string{};
                      state.timeError.clear();
                      state.timeStatus = "已保存 " + state.editStart +
                                         (fromAi ? "（来源 AI）" : "（来源 手动）");
                      reloadData(state);
                  });

    composeButton(ui, "detail.time.clear", contentX + saveTimeW + 8.0f, y, clearW, "清除", false,
                  [&state, id = selected->id] {
                      core::Core note{ app::dataPath() };
                      if (auto loaded = note.load(); !loaded) { state.error = loaded.error(); return; }
                      auto found = note.findAny(id);
                      if (!found) { state.error = "找不到这条记录"; return; }

                      core::Entry entry = *found;
                      entry.occurred.reset();
                      entry.occurred_source = core::TimeSource::None;
                      if (auto updated = note.update(entry); !updated)
                      {
                          state.error = updated.error();
                          return;
                      }
                      state.editStart.clear();
                      state.editEnd.clear();
                      state.timeError.clear();
                      reloadData(state);
                  });

    if (llmKeyConfigured())
    {
        composeButton(ui, "detail.time.ai",
                      contentX + saveTimeW + clearW + 16.0f, y,
                      std::max(80.0f, contentW - saveTimeW - clearW - 16.0f), "AI 时间草稿", false,
                      [&state, id = selected->id, raw = selected->raw_text] {
                          state.timeError = draftBusy() ? "已有任务在跑，等它结束" : "";
                          startDraftInBackground(id, raw, DraftTask::Time);
                      });
    }
    y += kButtonHeight + 8.0f;

    if (!state.timeError.empty())
    {
        metaLine(ui, "detail.time.error", contentX, y, contentW, state.timeError, true);
        y += kLineCaption + 2.0f;
    }
    else if (!state.timeStatus.empty())
    {
        metaLine(ui, "detail.time.status", contentX, y, contentW, state.timeStatus);
        y += kLineCaption + 2.0f;
    }

    // ---- 内容（原文）----
    fieldLabel(ui, "detail.edit.label", contentX, y, contentW,
               "内容（改完点保存：会重新解析时间并重生成标题）");
    y += kLineCaption + 2.0f;

    const float buttonsY   = panelY + panelH - kButtonHeight - kPanelPad;
    const float editHeight = std::max(90.0f, buttonsY - y - 10.0f);

    components::input(ui, "detail.edit")
        .position(contentX, y).size(contentW, editHeight)
        .multiline(true).scrollbar(true).fontSize(kFontBody)
        .value(state.editText)
        .onChange([&state](const std::string& value) { state.editText = value; })
        .build();

    // ---- 底部按钮 ----
    const float deleteW  = 96.0f;
    const float redraftW = 116.0f;
    const float saveW    = 88.0f;

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

    // 保存内容：改原文 → 重算 occurred → 打回 pending → 后台重生成
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

                      // 时间怎么处理（这是最容易出问题的一步）：
                      //   * 新原文能解析出时间 → 用它（来源 Rule）
                      //   * 解析不出，但原来的时间是**用户手动 / AI 定的** → 保留
                      //     否则"改个错别字"会把手改的时间悄悄抹掉
                      //   * 解析不出，且原来的时间来自规则 → 清空（用户把时间删掉了）
                      const auto reparsed = app::occurredFromText(state.editText, entry.created_at);

                      const bool userOwnedTime =
                          entry.occurred &&
                          (entry.occurred_source == core::TimeSource::Manual ||
                           entry.occurred_source == core::TimeSource::Ai);

                      entry.raw_text = state.editText;
                      if (reparsed)
                      {
                          entry.occurred        = reparsed;
                          entry.occurred_source = core::TimeSource::Rule;
                      }
                      else if (!userOwnedTime)
                      {
                          entry.occurred.reset();
                          entry.occurred_source = core::TimeSource::None;
                      }
                      // else：保留原来的时间和来源（改错别字不该丢时间）

                      // 原文变了，旧标题/摘要就成了过期数据 —— 打回 pending 等重生成。
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
