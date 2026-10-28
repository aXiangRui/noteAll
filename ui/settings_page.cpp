// 设置模块：当前只显示"数据在哪 / 有多少条 / 模型配了没有"。
//
// 它现在的价值是把这些信息从"翻文档"变成"看一眼"。以后真正的设置项
// （默认视图、字体大小、模型端点）都加在这个文件里，不影响周视图。
//
// 布局用 y 游标往下排，不要写一堆魔法数字 —— 字号一改（18–28），
// 硬编码的绝对坐标就全废了。

#include "pages.hpp"

#include <algorithm>
#include <string>

namespace app
{

namespace
{

using namespace metrics;

constexpr float kSectionGap = 14.0f;
constexpr float kRowHeight = 28.0f;
constexpr float kRowGap = 6.0f;
constexpr float kValueIndent = 116.0f;
constexpr float kChipHeight = 40.0f;

void composeSection(eui::Ui& ui, const std::string& id, float x, float y, float width,
                    float height, const std::string& title)
{
    ui.rect(id + ".bg")
        .position(x, y)
        .size(width, height)
        .radius(12.0f)
        .color(colors::kColumnBg)
        .border(1.0f, colors::kCardBorder)
        .build();

    ui.text(id + ".title")
        .position(x + 18.0f, y + 14.0f)
        .size(width - 36.0f, kLineBody)
        .text(title)
        .fontSize(kFontBody)
        .color(colors::kKindEvent)
        .build();
}

/** 一行「标签：值」，y 已经是这一行的顶边。 */
void composeRow(eui::Ui& ui, const std::string& id, float x, float y, float width,
                const std::string& label, const std::string& value)
{
    ui.text(id + ".label")
        .position(x, y)
        .size(kValueIndent - 12.0f, kLineBody)
        .text(label)
        .fontSize(kFontCaption)
        .color(colors::kMuted)
        .build();

    ui.text(id + ".value")
        .position(x + kValueIndent, y)
        .size(width - kValueIndent, kLineBody)
        .text(value)
        .fontSize(kFontBody)
        .color(colors::kText)
        .build();
}

void composeButton(eui::Ui& ui, const std::string& id, float x, float y, float width,
                   const std::string& label, std::function<void()> onClick)
{
    ui.stack(id)
        .position(x, y)
        .size(width, kChipHeight)
        .onClick(std::move(onClick))
        .content([&] {
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f).size(width, kChipHeight).radius(9.0f)
                .color(colors::kAccent).border(1.0f, colors::kCardBorder).build();
            ui.text(id + ".label")
                .position(0.0f, 9.0f).size(width, kLineCaption)
                .text(label).fontSize(kFontCaption)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(colors::kText).build();
        })
        .build();
}

}  // namespace

void composeSettingsPage(eui::Ui& ui, AppState& state, const PageRect& area)
{
    const float width = std::min(780.0f, area.width);
    float       y = area.y;

    ui.text("settings.heading")
        .position(area.x, y)
        .size(width, kLineTitle)
        .text("设置")
        .fontSize(kFontHeading)
        .color(colors::kText)
        .build();
    y += kLineTitle + 8.0f;

    // ---- 数据 ----
    std::size_t alive = 0;
    std::size_t deleted = 0;
    for (const auto& entry : state.entries)
    {
        (entry.deleted_at ? deleted : alive) += 1;
    }

    const float dataHeight = 14.0f + kLineBody + 12.0f + kRowHeight * 3.0f + kRowGap * 2.0f +
                             kChipHeight + 16.0f;
    composeSection(ui, "settings.data", area.x, y, width, dataHeight, "数据");

    float rowY = y + 14.0f + kLineBody + 12.0f;
    composeRow(ui, "settings.data.path", area.x + 18.0f, rowY, width - 36.0f,
               "数据文件", app::dataPath().string());
    rowY += kRowHeight + kRowGap;
    composeRow(ui, "settings.data.count", area.x + 18.0f, rowY, width - 36.0f,
               "记录数", std::to_string(alive) + " 条（已删除 " + std::to_string(deleted) + " 条）");
    rowY += kRowHeight + kRowGap;
    composeRow(ui, "settings.data.timezone", area.x + 18.0f, rowY, width - 36.0f,
               "时区", std::string{ std::chrono::current_zone()->name() });

    composeButton(ui, "settings.data.reload", area.x + 18.0f, rowY + kRowHeight + 10.0f, 108.0f,
                  "重新读取", [&state] { reloadData(state); });

    y += dataHeight + kSectionGap;

    // ---- 模型（草稿生成）----
    const bool   keyReady = llmKeyConfigured();
    const std::string hintText =
        keyReady ? "草稿会调用模型生成摘要 / 标签 / 类型。没网或余额不足时不阻塞记录，"
                   "失败原因可以用 note show <id> 看到。"
                 : "把 DeepSeek 的 key 放进数据文件旁边的 note.key（内容就是 key 本身），"
                   "或者设环境变量 NOTE_LLM_KEY。详见 docs/配置.md。";

    const float llmHeight = 14.0f + kLineBody + 12.0f + kRowHeight + 10.0f + kLineBody * 2.0f + 16.0f;
    composeSection(ui, "settings.llm", area.x, y, width, llmHeight, "模型（草稿生成）");

    rowY = y + 14.0f + kLineBody + 12.0f;
    composeRow(ui, "settings.llm.key", area.x + 18.0f, rowY, width - 36.0f,
               "API key", keyReady ? "已配置" : "未配置");

    ui.text("settings.llm.hint")
        .position(area.x + 18.0f, rowY + kRowHeight + 10.0f)
        .size(width - 36.0f, kLineBody * 2.0f)
        .text(hintText)
        .fontSize(kFontCaption)
        .lineHeight(kLineCaption)
        .wrap(true)
        .color(colors::kMuted)
        .build();

    y += llmHeight + kSectionGap;

    // ---- 关于 ----
    const float aboutHeight = 14.0f + kLineBody + 12.0f + kRowHeight * 3.0f + 14.0f;
    composeSection(ui, "settings.about", area.x, y, width, aboutHeight, "关于");

    rowY = y + 14.0f + kLineBody + 12.0f;
    composeRow(ui, "settings.about.version", area.x + 18.0f, rowY, width - 36.0f,
               "版本", "noteAll 0.1.0");
    rowY += kRowHeight;
    composeRow(ui, "settings.about.ui", area.x + 18.0f, rowY, width - 36.0f,
               "界面", "EUI-NEO（C++17 + OpenGL 自绘）");
    rowY += kRowHeight;
    composeRow(ui, "settings.about.theme", area.x + 18.0f, rowY, width - 36.0f,
               "主题", "银白底 / 淡蓝任务卡 / 思源黑体");

    y += aboutHeight;

    if (!state.error.empty())
    {
        ui.text("settings.error")
            .position(area.x, y + 8.0f)
            .size(width, kLineCaption * 2.0f)
            .text(state.error)
            .fontSize(kFontCaption)
            .lineHeight(kLineCaption)
            .wrap(true)
            .color(colors::kDanger)
            .build();
    }
}

}  // namespace app
