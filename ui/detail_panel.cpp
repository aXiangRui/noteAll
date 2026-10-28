// 详情面板：点开一张卡片之后，才显示完整内容。
//
// 为什么要有它（这是用户明确要求的）：
//   时间轴是"扫一眼"的地方，卡片上只该有标题；
//   原文可能是一整段话，塞进 150px 宽的列里既看不清也挤掉别的卡片。
//   所以：列表只放标题，详情才读全文。
//
// 面板画在内容区右侧，盖在周视图上面（不改周视图的布局计算）。

#include "pages.hpp"

#include <string>

namespace app
{

namespace
{

using namespace metrics;

constexpr float kPanelWidth = 360.0f;
constexpr float kPanelPad = 18.0f;
constexpr float kRowGap = 10.0f;

void labelValue(eui::Ui& ui, const std::string& id, float x, float& y, float width,
                const std::string& label, const std::string& value)
{
    ui.text(id + ".label")
        .position(x, y)
        .size(width, kLineCaption)
        .text(label)
        .fontSize(kFontCaption)
        .color(colors::kMuted)
        .build();

    y += kLineCaption;

    ui.text(id + ".value")
        .position(x, y)
        .size(width, kLineBody)
        .text(value)
        .fontSize(kFontBody)
        .color(colors::kText)
        .build();

    y += kLineBody + kRowGap;
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

    const float panelX = area.x + area.width - kPanelWidth;
    const float panelY = area.y;
    const float panelW = kPanelWidth;
    const float panelH = area.height;
    const float contentX = panelX + kPanelPad;
    const float contentW = panelW - kPanelPad * 2.0f;

    ui.rect("detail.bg")
        .position(panelX, panelY)
        .size(panelW, panelH)
        .radius(14.0f)
        .color(colors::kPageBg)
        .border(1.0f, colors::kCardBorder)
        .build();

    // 顶部：标题 + 关闭
    ui.text("detail.title")
        .position(contentX, panelY + 14.0f)
        .size(contentW - 56.0f, kLineTitle)
        .text(app::displayTitle(*selected))
        .fontSize(kFontRange)
        .lineHeight(kLineTitle)
        .wrap(true)
        .color(colors::kText)
        .build();

    ui.stack("detail.close")
        .position(panelX + panelW - kPanelPad - 40.0f, panelY + 14.0f)
        .size(40.0f, 36.0f)
        .onClick([&state] { state.selectedId.clear(); })
        .content([&] {
            ui.rect("detail.close.bg")
                .position(0.0f, 0.0f).size(40.0f, 36.0f).radius(9.0f)
                .color(colors::kAccent).border(1.0f, colors::kCardBorder).build();
            ui.text("detail.close.label")
                .position(0.0f, 6.0f).size(40.0f, kLineCaption)
                .text("✕").fontSize(kFontCaption)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(colors::kText).build();
        })
        .build();

    float y = panelY + 14.0f + kLineTitle + 10.0f;

    // 时间
    const std::string timeLine = app::timeLabel(*selected, *std::chrono::current_zone());
    // 记录发生在哪一天（本地）：直接用数据层算好的，别在这里重复时区换算
    const std::string dayLine =
        app::monthDayLabel(app::localDayOf(*selected, *std::chrono::current_zone()));

    labelValue(ui, "detail.time", contentX, y, contentW, "时间", timeLine);
    labelValue(ui, "detail.kind", contentX, y, contentW, "类型",
               core::toString(selected->kind) + "　（记录于 " + dayLine + "）");

    // 标签
    std::string tagsText;
    for (std::size_t i = 0; i < selected->tags.size(); ++i)
    {
        tagsText += (i == 0 ? "" : "、") + selected->tags[i];
    }
    labelValue(ui, "detail.tags", contentX, y, contentW, "标签",
               tagsText.empty() ? "（无）" : tagsText);

    labelValue(ui, "detail.draft", contentX, y, contentW, "草稿状态",
               core::toString(selected->draft_status)
                   + (selected->draft_error.empty() ? "" : "　" + selected->draft_error));

    // 原文（这才是"实际内容"，换行显示，按面板高度截断）
    ui.text("detail.raw.label")
        .position(contentX, y)
        .size(contentW, kLineCaption)
        .text("原文")
        .fontSize(kFontCaption)
        .color(colors::kMuted)
        .build();

    y += kLineCaption;

    const float rawHeight = std::max(kLineBody * 2.0f, panelY + panelH - y - kPanelPad);

    ui.rect("detail.raw.bg")
        .position(contentX - 6.0f, y - 4.0f)
        .size(contentW + 12.0f, rawHeight + 8.0f)
        .radius(10.0f)
        .color(colors::kCardBg)
        .border(1.0f, colors::kCardBorder)
        .build();

    ui.text("detail.raw.text")
        .position(contentX, y)
        .size(contentW, rawHeight)
        .text(selected->raw_text)
        .fontSize(kFontBody)
        .lineHeight(kLineBody)
        .wrap(true)
        .color(colors::kText)
        .build();
}

}  // namespace app
