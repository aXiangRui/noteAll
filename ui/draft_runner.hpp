#pragma once

#include <string>

// 后台跑模型生成草稿。两种任务：
//   Content —— 标题 / 摘要 / 标签 / 类型
//   Time    —— 只推 开始 / 结束（结果填进编辑框让用户确认，不直接落盘）
//
// 为什么必须后台：一次模型调用 0.7–1.1 秒，而 compose() 是渲染循环里的函数。
// 在 onClick 里同步调用 = 界面卡住 1 秒（用户以为崩了）。这是 GUI 里最容易
// 犯的错误，也是"add 必须零延迟"那条原则的延伸。
namespace app
{

enum class DraftTask
{
    Content,
    Time,
};

/**
 * 发起一次后台草稿生成。
 *
 * @returns false = 已经有一个在跑（GUI 单线程语义：一次只跑一个）
 */
bool startDraftInBackground(std::string entryId, std::string rawText,
                            DraftTask task = DraftTask::Content);

/** 是否正在生成（用于界面显示"生成中…"）。 */
bool draftBusy();

/**
 * 把后台结果应用回内存并落盘。
 *
 * **必须在主线程 / compose 里调用**，因为它会改 AppState 并写文件。
 * @returns true = 这一帧刚处理完一个结果
 */
bool pollDraftResult(class AppState& state);

}  // namespace app
