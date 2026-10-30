#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "app/calendar.hpp"
#include "core/entry.hpp"
#include "import/bill.hpp"
#include "ledger/ledger.hpp"
#include "eui_neo.h"

// GUI 的共享状态与配色。
//
// 为什么单独拿出来：每加一个模块（周视图 / 设置 / 以后的月视图、搜索、
// 复习计划…）都要读同一份数据、用同一套颜色。放在 app.cpp 里会变成
// 一个越写越长的巨型文件。
namespace app
{

// 侧边栏里的模块。加一个模块 = 这里加一项 + 侧边栏加一行 + 写一个 compose 函数。
enum class Page
{
    Week,        // 周视图（当前默认）
    Ledger,      // 账本（记账流水）
    Settings,    // 设置
};

struct AppState
{
    Page                      page = Page::Week;
    std::chrono::year_month_day weekStart{};      // 当前周的周一
    std::vector<core::Entry>  entries;            // 内存快照（compose 里不读盘）
    std::string               error;
    std::string               selectedId;   // 详情面板当前显示的记录（空 = 不显示）

    // 「新增」面板：composing = 面板打开，composeText = 正在输入的内容
    bool                      composing = false;
    std::string               composeText;
    std::string               composeError;

    // 搜索（顶栏第二行的输入框）；空 = 不过滤
    std::string               searchText;

    // 详情面板里的编辑缓冲（打开时从 raw_text 填）
    std::string               editText;
    std::string               editStart;   // 开始时间文本：2026-10-30 08:30
    std::string               editEnd;     // 结束时间文本（空 = 没有结束）
    std::string               timeError;
    std::string               timeStatus;  // 「已保存 …」这类一次性反馈
    std::string               aiStart;    // AI 建议的时间（用于判断是否采纳）
    std::string               aiEnd;
    bool                      loaded = false;

    // ---- 账本 ----
    std::vector<ledger::Transaction> ledgerEntries;   // 可见流水（不含软删除）
    std::chrono::year_month          ledgerMonth{};   // 当前看的月份
    std::string                      ledgerError;
    bool                             ledgerLoaded = false;

    // 「+ 记账」面板（录入）
    bool                             ledgerComposing = false;
    std::string                      ledgerText;      // "午饭 32"
    std::string                      ledgerAt;        // 可空 = 现在
    bool                             ledgerIncome = false;
    std::string                      ledgerCategory;  // 空 = 自动猜
    std::string                      ledgerFormError;

    // 「导入账单」面板
    bool                             ledgerImportOpen = false;
    std::string                      ledgerImportPath;
    bool                             ledgerImportReady = false;   // 已经读过并解析成功
    std::string                      ledgerImportInfo;            // 「编码 utf-8　来源 微信 …」
    std::vector<std::string>         ledgerImportNotes;           // 跳过原因（前几条）
    std::string                      ledgerImportError;
    import::BillParseResult          ledgerImportParsed;          // 待确认导入的数据
    std::string                      ledgerNotice;                 // 导入完成后的一句结果
};

/**
 * 请求下一帧重绘。
 *
 * 为什么必须有这个东西：EUI 是**保留模式** —— compose() 只在"有输入事件 /
 * 尺寸变化 / 显式请求"时才跑（dsl_window_runtime.h:72）。我们在点击回调里
 * 改了 state，如果不主动请求，界面要等下一次鼠标动才更新 ——
 * 表现就是"点了没反应""要再点一下才生效"。
 */
void repaint();

/** 本地"今天"（按本地时区，不是 UTC）。 */
std::chrono::year_month_day todayLocal();

/** 只在第一次调用时读盘；之后靠 reloadData() 手动刷新。 */
void ensureLoaded(AppState& state);

/** 重新读盘（点「刷新」时用）。compose 里绝不能调它。 */
void reloadData(AppState& state);

/** 有没有可用的模型 key：环境变量 > 数据目录/note.key > %APPDATA%/noteAll/note.key */
bool llmKeyConfigured();

/** 读账本（ledger.jsonl）。账本和笔记是两个文件，各读各的。 */
void reloadLedger(AppState& state);

/** 账本文件路径（和笔记同目录，固定 ledger.jsonl）。 */
std::filesystem::path ledgerFilePath();

/** 还有多少条欠草稿（draft_status == Pending）。 */
std::size_t pendingDraftCount(AppState const& state);

/** 欠草稿的第一条（按时间倒序，优先补新的）；没有就返回 nullptr。 */
core::Entry const* firstPendingDraft(AppState const& state);

// ---------------------------------------------------------------------------
// 字号与行高
//
// 规则：界面上所有文字都落在 18–28 之间（不含则不写）。
// 全部集中在这里，改一次全局生效；页面里禁止再出现裸的字号数字。
//
// 注意：字号变大后，卡片高度/列头高度/侧栏宽度都要跟着变 ——
// 那些在各自的页面文件里，用同一套常量算出来。
// ---------------------------------------------------------------------------
namespace metrics
{

inline constexpr float kFontCaption = 18.0f;   // 最小：时间、日期、脚注、提示
inline constexpr float kFontBody = 20.0f;      // 正文：任务标题、侧栏项、行值
inline constexpr float kFontRange = 22.0f;     // 周范围
inline constexpr float kFontHeading = 26.0f;   // 页面标题（设置）
inline constexpr float kFontTitle = 28.0f;     // 最大：品牌名

inline constexpr float kLineCaption = 24.0f;
inline constexpr float kLineBody = 26.0f;
inline constexpr float kLineTitle = 34.0f;

}  // namespace metrics

// ---------------------------------------------------------------------------
// 配色：银白底 + 淡蓝强调
//
// 统一放在这里，模块之间才不会各自发明颜色。改名时也只改一处。
// ---------------------------------------------------------------------------
namespace colors
{

inline constexpr eui::Color kPageBg{ 0.957f, 0.969f, 0.980f, 1.0f };      // 页面底：银白
inline constexpr eui::Color kSidebarBg{ 0.925f, 0.941f, 0.961f, 1.0f };   // 侧栏：略深一档
inline constexpr eui::Color kColumnBg{ 0.976f, 0.984f, 0.992f, 1.0f };    // 日期列
inline constexpr eui::Color kTodayBg{ 0.902f, 0.945f, 0.996f, 1.0f };     // 今天那一列
inline constexpr eui::Color kCardBg{ 0.855f, 0.914f, 0.996f, 1.0f };      // 任务卡：淡蓝
inline constexpr eui::Color kCardBorder{ 0.729f, 0.827f, 0.957f, 1.0f };  // 淡蓝描边
inline constexpr eui::Color kAccent{ 0.780f, 0.878f, 0.996f, 1.0f };      // 选中态
inline constexpr eui::Color kText{ 0.145f, 0.180f, 0.231f, 1.0f };        // 主文字
inline constexpr eui::Color kMuted{ 0.451f, 0.502f, 0.573f, 1.0f };       // 次要文字
inline constexpr eui::Color kLine{ 0.702f, 0.780f, 0.878f, 0.90f };       // 时间线
inline constexpr eui::Color kDanger{ 0.812f, 0.235f, 0.208f, 1.0f };

// 记录类型 → 圆点颜色（淡蓝体系里的四种区分）
inline constexpr eui::Color kKindEvent{ 0.235f, 0.510f, 0.855f, 1.0f };   // 事
inline constexpr eui::Color kKindTodo{ 0.918f, 0.545f, 0.180f, 1.0f };    // 待办
inline constexpr eui::Color kKindNote{ 0.204f, 0.678f, 0.420f, 1.0f };    // 资料
inline constexpr eui::Color kKindThought{ 0.541f, 0.400f, 0.878f, 1.0f }; // 想法
inline constexpr eui::Color kKindUnknown{ 0.545f, 0.596f, 0.667f, 1.0f };

}  // namespace colors

/** 一条记录该用哪个圆点颜色。 */
inline eui::Color kindColor(core::Kind kind)
{
    switch (kind)
    {
        case core::Kind::Event:   return colors::kKindEvent;
        case core::Kind::Todo:    return colors::kKindTodo;
        case core::Kind::Note:    return colors::kKindNote;
        case core::Kind::Thought: return colors::kKindThought;
        default:                  return colors::kKindUnknown;
    }
}

}  // namespace app
