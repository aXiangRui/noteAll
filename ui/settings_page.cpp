// 设置模块：当前只显示"数据在哪 / 有多少条 / 模型配了没有"。
//
// 它现在的价值是把这些信息从"翻文档"变成"看一眼"。以后真正的设置项
// （默认视图、字体大小、模型端点）都加在这个文件里，不影响周视图。

#include "pages.hpp"

#include <string>

namespace app
{

namespace
{

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
        .position(x + 16.0f, y + 12.0f)
        .size(width - 32.0f, 22.0f)
        .text(title)
        .fontSize(15.0f)
        .color(colors::kKindEvent)
        .build();
}

void composeRow(eui::Ui& ui, const std::string& id, float x, float y, float width,
                const std::string& label, const std::string& value)
{
    ui.text(id + ".label")
        .position(x, y)
        .size(96.0f, 20.0f)
        .text(label)
        .fontSize(13.0f)
        .color(colors::kMuted)
        .build();

    ui.text(id + ".value")
        .position(x + 100.0f, y)
        .size(width - 116.0f, 20.0f)
        .text(value)
        .fontSize(13.0f)
        .color(colors::kText)
        .build();
}

}  // namespace

void composeSettingsPage(eui::Ui& ui, AppState& state, const PageRect& area)
{
    const float width = std::min(720.0f, area.width);

    ui.text("settings.heading")
        .position(area.x, area.y)
        .size(width, 30.0f)
        .text("设置")
        .fontSize(22.0f)
        .color(colors::kText)
        .build();

    // ---- 数据 ----
    composeSection(ui, "settings.data", area.x, area.y + 44.0f, width, 158.0f, "数据");

    std::size_t alive = 0;
    std::size_t deleted = 0;
    for (const auto& entry : state.entries)
    {
        (entry.deleted_at ? deleted : alive) += 1;
    }

    composeRow(ui, "settings.data.path", area.x + 16.0f, area.y + 74.0f, width,
               "数据文件", app::dataPath().string());
    composeRow(ui, "settings.data.count", area.x + 16.0f, area.y + 102.0f, width,
               "记录数", std::to_string(alive) + " 条（已删除 " + std::to_string(deleted) + " 条）");
    composeRow(ui, "settings.data.timezone", area.x + 16.0f, area.y + 130.0f, width,
               "时区", std::string{ std::chrono::current_zone()->name() });

    ui.stack("settings.data.reload")
        .position(area.x + 16.0f, area.y + 158.0f)
        .size(96.0f, 30.0f)
        .onClick([&state] { reloadData(state); })
        .content([&] {
            ui.rect("settings.data.reload.bg")
                .position(0.0f, 0.0f).size(96.0f, 30.0f).radius(8.0f)
                .color(colors::kAccent).border(1.0f, colors::kCardBorder).build();
            ui.text("settings.data.reload.label")
                .position(0.0f, 7.0f).size(96.0f, 18.0f)
                .text("重新读取").fontSize(13.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(colors::kText).build();
        })
        .build();

    // ---- 模型（草稿生成）----
    composeSection(ui, "settings.llm", area.x, area.y + 218.0f, width, 132.0f, "模型（草稿生成）");

    composeRow(ui, "settings.llm.key", area.x + 16.0f, area.y + 248.0f, width,
               "API key", llmKeyConfigured() ? "已配置" : "未配置");

    ui.text("settings.llm.hint")
        .position(area.x + 16.0f, area.y + 276.0f)
        .size(width - 32.0f, 60.0f)
        .text(llmKeyConfigured()
                  ? "草稿会调用模型生成摘要/标签/类型。没网或余额不足时不会阻塞记录，"
                    "失败原因可以在 note show <id> 里看到。"
                  : "把 DeepSeek 的 key 放进数据文件旁边的 note.key（内容就是 key 本身），"
                    "或者设环境变量 NOTE_LLM_KEY。详见 docs/配置.md。")
        .fontSize(12.0f)
        .lineHeight(18.0f)
        .wrap(true)
        .color(colors::kMuted)
        .build();

    // ---- 关于 ----
    composeSection(ui, "settings.about", area.x, area.y + 366.0f, width, 118.0f, "关于");
    composeRow(ui, "settings.about.version", area.x + 16.0f, area.y + 396.0f, width,
               "版本", "noteAll 0.1.0");
    composeRow(ui, "settings.about.ui", area.x + 16.0f, area.y + 424.0f, width,
               "界面", "EUI-NEO（C++17 + OpenGL，自绘 UI）");
    composeRow(ui, "settings.about.theme", area.x + 16.0f, area.y + 452.0f, width,
               "主题", "银白底 / 淡蓝任务卡");

    if (!state.error.empty())
    {
        ui.text("settings.error")
            .position(area.x, area.y + 500.0f)
            .size(width, 40.0f)
            .text(state.error)
            .fontSize(12.0f)
            .lineHeight(16.0f)
            .wrap(true)
            .color(colors::kDanger)
            .build();
    }
}

}  // namespace app
