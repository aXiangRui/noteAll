// note_gui 的壳：窗口配置 + 侧边栏 + 模块分发。
//
// 拆分原则（为了以后加模块不打架）：
//   app.cpp            壳：窗口尺寸、背景、侧边栏、底部信息、按 state.page 分发
//   ui/app_state.hpp   共享状态 + 配色（银白 / 淡蓝的唯一来源）
//   ui/state.cpp       读盘、今天、key 是否配置
//   ui/week_page.cpp   模块：周视图
//   ui/settings_page.cpp 模块：设置
//
// 加一个新模块：写 ui/xxx_page.cpp + ui/app_state.hpp 里 Page 加一项 +
// 侧边栏加一行 + pages.hpp 声明 + app.cpp 的分发里加一个分支。

#include "eui_neo.h"

#include <algorithm>
#include <string>

#include "pages.hpp"

namespace app {

namespace {

// 整个 GUI 只有这一份状态（单窗口应用）。
AppState state;

constexpr float kSidebarWidth = 176.0f;
constexpr float kSidebarPad = 14.0f;
constexpr float kContentGap = 16.0f;
constexpr float kFooterHeight = 22.0f;

void composeSidebarItem(eui::Ui& ui, const std::string& id, float y, const std::string& label,
                        Page page) {
    const bool selected = state.page == page;

    ui.stack(id)
        .position(kSidebarPad, y)
        .size(kSidebarWidth - kSidebarPad * 2.0f, 36.0f)
        .onClick([page] { state.page = page; })
        .content([&] {
            if (selected) {
                ui.rect(id + ".pill")
                    .position(0.0f, 0.0f)
                    .size(kSidebarWidth - kSidebarPad * 2.0f, 36.0f)
                    .radius(9.0f)
                    .color(colors::kAccent)
                    .build();
            }

            ui.text(id + ".label")
                .position(12.0f, 9.0f)
                .size(kSidebarWidth - kSidebarPad * 2.0f - 20.0f, 20.0f)
                .text(label)
                .fontSize(14.0f)
                .color(selected ? colors::kKindEvent : colors::kText)
                .build();
        })
        .build();
}

}  // namespace

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("noteAll")
        .windowSize(1320, 760)
        // 正文用思源黑体（assets/gui 里，构建时自动拷到 exe 旁边）。
        // EUI 找字体只认文件名 → 必须平铺在 assets/ 根下，写子目录会找不到。
        // 文件缺失时会退回框架自带的中文字体，不会变成方块。
        .textFont("SourceHanSansSC-Regular-2.otf");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    ensureLoaded(state);

    // 页面底：银白
    ui.rect("page.bg")
        .position(0.0f, 0.0f)
        .size(screen.width, screen.height)
        .color(colors::kPageBg)
        .build();

    // 侧边栏（整高）
    ui.rect("sidebar.bg")
        .position(0.0f, 0.0f)
        .size(kSidebarWidth, screen.height)
        .color(colors::kSidebarBg)
        .build();

    ui.text("sidebar.brand")
        .position(kSidebarPad, 16.0f)
        .size(kSidebarWidth - kSidebarPad * 2.0f, 26.0f)
        .text("noteAll")
        .fontSize(19.0f)
        .color(colors::kKindEvent)
        .build();

    ui.text("sidebar.subtitle")
        .position(kSidebarPad, 42.0f)
        .size(kSidebarWidth - kSidebarPad * 2.0f, 16.0f)
        .text("时间轴笔记本")
        .fontSize(11.0f)
        .color(colors::kMuted)
        .build();

    // 模块列表 —— 预留：月视图 / 搜索 / 复习计划 / 导入 都往这下面加
    composeSidebarItem(ui, "nav.week", 76.0f, "周视图", Page::Week);
    composeSidebarItem(ui, "nav.settings", 118.0f, "设置", Page::Settings);

    ui.text("sidebar.reserved")
        .position(kSidebarPad, 168.0f)
        .size(kSidebarWidth - kSidebarPad * 2.0f, 16.0f)
        .text("（更多模块预留）")
        .fontSize(11.0f)
        .color(colors::kMuted)
        .build();

    // 内容区
    const PageRect content{
        kSidebarWidth + kContentGap,
        16.0f,
        std::max(240.0f, screen.width - kSidebarWidth - kContentGap * 2.0f),
        std::max(160.0f, screen.height - 16.0f - kFooterHeight - 8.0f),
    };

    switch (state.page) {
        case Page::Week:
            composeWeekPage(ui, state, content);
            break;
        case Page::Settings:
            composeSettingsPage(ui, state, content);
            break;
    }

    // 底部：数据来源（双击启动时当前目录是 exe 所在目录，这行能避免"数据丢了"的误会）
    ui.text("footer.data")
        .position(content.x, screen.height - kFooterHeight)
        .size(content.width, 16.0f)
        .text("数据: " + app::dataPath().string())
        .fontSize(11.0f)
        .color(colors::kMuted)
        .build();
}

}  // namespace app
