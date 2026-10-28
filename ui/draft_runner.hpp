#pragma once

#include <string>

// 后台生成一条记录的草稿（title / summary / tags / kind）。
//
// 为什么必须后台：一次模型调用 0.7–1.1 秒，而 compose() 是渲染循环里的函数。
// 在 onClick 里同步调用 = 界面卡住 1 秒（用户以为崩了）。这是 GUI 里最容易
// 犯的错误，也是我在设计评审时反复强调的"add 必须零延迟"的延伸。
namespace app
{

/**
 * 发起一次后台草稿生成。
 *
 * @returns false = 已经有一个在跑（GUI 单线程语义：一次只补一条）
 */
bool startDraftInBackground(std::string entryId, std::string rawText);

/** 是否正在生成（用于界面显示"生成中…"）。 */
bool draftBusy();

/**
 * 把后台结果应用回内存并落盘。
 *
 * **必须在主线程 / compose 里调用**，因为它会改 AppState 并写文件。
 * @returns true = 这一帧刚处理完一个结果（调用方可顺带刷新）
 */
bool pollDraftResult(class AppState& state);

}  // namespace app
