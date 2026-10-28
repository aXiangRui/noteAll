// ui_demo —— 步骤 0：验证 EUI-NEO 能不能做记事软件的 GUI。
//
// 只验证三件事，别的都不做（不读 notes.jsonl、不碰 core）：
//   1. 中文输入法：在输入框里打拼音，候选框要出现在光标旁边，上屏后文字要进来；
//   2. 多行编辑：回车换行、光标移动、选区、Ctrl+Z 撤销；
//   3. 长列表滚动：200+ 行的虚拟列表，滚轮要顺。
//
// 这三个过不了，GUI 这条路线就得换框架 —— 所以在写任何界面设计之前先跑它。
//
// 用法：ui_demo.exe（必须和 assets/ 目录同级）

#include "eui_neo.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace app {

namespace {

struct DemoState {
    std::string              draft;    // 输入框里的内容
    std::vector<std::string> saved;    // 点「存进列表」之后的内容
};

DemoState state;

// UTF-8 码点个数（中文一个字算 1，不是 3 —— 不然字数显示很怪）
std::size_t codePointCount(const std::string& text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        // 续字节（10xxxxxx）不算新字符
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

// 列表里 200 行滚动测试用
constexpr int kScrollRows = 200;

}  // namespace

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("EUI-NEO 检查：输入法 / 多行编辑 / 列表滚动")
        .windowSize(920, 700);
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    const float pad = 24.0f;
    const float width = std::min(880.0f, std::max(320.0f, screen.width - pad * 2.0f));

    ui.text("hint")
        .position(pad, 14.0f)
        .size(width, 26.0f)
        .text("① 输入框里打拼音看候选框　② 多行编辑/撤销　③ 下方列表滚轮滚动")
        .fontSize(14.0f)
        .build();

    // ---- 1. 输入框：用多行，记事软件的真实场景 ----
    components::input(ui, "demo.input")
        .position(pad, 44.0f)
        .size(width, 132.0f)
        .multiline(true)
        .scrollbar(true)
        .fontSize(16.0f)
        .placeholder("在这里打一段中文，试试拼音输入、换行、Ctrl+Z")
        .value(state.draft)
        .onChange([](const std::string& value) {
            state.draft = value;
        })
        .build();

    // ---- 2. 状态回显：确认输入框里的内容真的进了字符串 ----
    const std::string info =
        "字符数 " + std::to_string(codePointCount(state.draft)) +
        "　|　已存进列表 " + std::to_string(state.saved.size()) + " 条";
    ui.text("info")
        .position(pad, 182.0f)
        .size(width * 0.62f, 24.0f)
        .text(info)
        .fontSize(14.0f)
        .build();

    components::button(ui, "demo.save")
        .position(pad + width - 150.0f, 178.0f)
        .size(150.0f, 34.0f)
        .text("把这段存进列表")
        .onClick([] {
            if (!state.draft.empty()) {
                state.saved.push_back(state.draft);
            }
        })
        .build();

    // ---- 3. 虚拟列表：200 行滚动测试 ----
    const float listTop = 224.0f;
    const float listHeight = std::max(120.0f, screen.height - listTop - 20.0f);

    components::virtualList(ui, "demo.list")
        .position(pad, listTop)
        .size(width, listHeight)
        .itemCount(static_cast<std::int64_t>(std::min<std::size_t>(state.saved.size(), 20) + kScrollRows))
        .rowHeight(30.0f)
        .row([](eui::Ui& rowUi, const std::string& rowId, std::int64_t rowIndex, float contentWidth,
                float itemHeight) {
            const std::size_t index = static_cast<std::size_t>(rowIndex);
            // 前几行放你刚输入的内容，后面全是滚动测试行
            const bool mine = index < state.saved.size() && index < 20;
            const std::string label = mine ? ("[" + std::to_string(index) + "] " + state.saved[index])
                                           : ("第 " + std::to_string(rowIndex) + " 行 · 滚动测试");

            rowUi.text(rowId + ".text")
                .size(contentWidth, itemHeight)
                .text(label)
                .fontSize(14.0f)
                .build();
        })
        .build();
}

}  // namespace app
