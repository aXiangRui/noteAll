#include "draft_runner.hpp"

#include <cstdlib>
#include <ctime>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/api_key.hpp"
#include "app/entry_service.hpp"
#include "app/time_input.hpp"
#include "app_state.hpp"
#include "core/core.hpp"
#include "llm/draft.hpp"
#include "llm/http_chat_client.hpp"

namespace app
{

namespace
{

struct Result
{
    bool                     hasResult = false;
    DraftTask                task = DraftTask::Content;
    std::string              entryId;
    std::string              title;
    std::string              summary;
    std::vector<std::string> tags;
    std::string              kind;
    std::string              startLocal;   // Time 任务才有
    std::string              endLocal;
    std::string              error;
};

std::mutex g_mutex;
bool       g_running = false;
Result     g_result;

// 读 key + 建客户端。放在这里而不是 app 库，是因为它依赖 llm（Windows-only）。
std::expected<std::unique_ptr<llm::IChatClient>, std::string> makeClient()
{
    const auto key = app::findApiKey(app::dataPath());
    if (!key)
    {
        return std::unexpected(std::string{ "没有配置 API key（note.key 或 NOTE_LLM_KEY）" });
    }

    llm::ChatConfig config;
    config.baseUrl = std::getenv("NOTE_LLM_BASE") != nullptr ? std::getenv("NOTE_LLM_BASE")
                                                            : "https://api.deepseek.com";
    config.path = std::getenv("NOTE_LLM_PATH") != nullptr ? std::getenv("NOTE_LLM_PATH")
                                                          : "/chat/completions";
    config.model = std::getenv("NOTE_LLM_MODEL") != nullptr ? std::getenv("NOTE_LLM_MODEL")
                                                            : "deepseek-chat";
    config.apiKey = *key;

    if (config.baseUrl.rfind("http://", 0) == 0)
    {
        config.transport.proxyMode = net::ConnectOptions::ProxyMode::Direct;
    }

    auto created = llm::HttpChatClient::create(std::move(config));
    if (!created)
    {
        return std::unexpected(created.error().message);
    }
    return std::unique_ptr<llm::IChatClient>(std::move(*created));
}

}  // namespace

bool draftBusy()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_running;
}

bool startDraftInBackground(std::string entryId, std::string rawText, DraftTask task)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_running)
        {
            return false;
        }
        g_running = true;
        g_result  = Result{};
    }

    // 「今天」必须在主线程算好再传进去：worker 里再算一次可能跨天，
    // 而且模型需要的是**调用时**的今天，不是"跑完时的今天"。
    const std::string todayLocal = app::formatLocalDateTime(app::nowUtc(), *std::chrono::current_zone()).substr(0, 10);

    std::thread worker([entryId = std::move(entryId), rawText = std::move(rawText), task,
                        todayLocal] {
        Result result;
        result.entryId = entryId;
        result.task    = task;

        auto client = makeClient();
        if (!client)
        {
            result.error = client.error();
        }
        else if (task == DraftTask::Content)
        {
            auto fields = llm::extractDraft(**client, rawText);
            if (!fields)
            {
                result.error = fields.error().message;
            }
            else
            {
                result.title   = fields->title;
                result.summary = fields->summary;
                result.tags    = fields->tags;
                result.kind    = fields->kind;
            }
        }
        else
        {
            auto fields = llm::extractTimeDraft(**client, rawText, todayLocal);
            if (!fields)
            {
                result.error = fields.error().message;
            }
            else
            {
                result.startLocal = fields->startLocal.value_or("");
                result.endLocal   = fields->endLocal.value_or("");
            }
        }

        result.hasResult = true;

        std::lock_guard<std::mutex> lock(g_mutex);
        g_result  = std::move(result);
        g_running = false;
    });

    worker.detach();
    return true;
}

bool pollDraftResult(AppState& state)
{
    Result done;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_result.hasResult)
        {
            return false;
        }
        done      = std::move(g_result);
        g_result  = Result{};
    }

    // ---- 时间草稿：结果只填进编辑框，让用户看一眼再保存 ----
    // 时间改错比标题改错严重得多（会跳到别的日子），所以**绝不自动落盘**。
    if (done.task == DraftTask::Time)
    {
        if (!done.error.empty())
        {
            state.timeError = done.error;
        }
        else
        {
            state.editStart = done.startLocal;
            state.editEnd   = done.endLocal;
            state.timeError = done.startLocal.empty() ? "模型认为这句里没有时间" : "";
        }
        return true;
    }

    // 先在内存快照里改，再落盘（core 的 update 会自己加锁 + 重读）
    core::Core note{ app::dataPath() };
    if (auto loaded = note.load(); !loaded)
    {
        state.error = "草稿写回失败: " + loaded.error();
        return true;
    }

    auto found = note.findAny(done.entryId);
    if (!found)
    {
        return true;   // 记录被删了，什么都不用做
    }

    core::Entry& entry = *found;
    if (done.error.empty())
    {
        entry.title        = done.title;
        entry.summary      = done.summary;
        entry.tags         = done.tags;
        entry.kind         = core::kindFromString(done.kind);
        entry.draft_status = core::DraftStatus::Done;
        entry.draft_error.clear();
    }
    else
    {
        entry.draft_attempts += 1;
        entry.draft_error     = done.error;
        entry.draft_status    = core::DraftStatus::Failed;
    }

    if (auto updated = note.update(entry); !updated)
    {
        state.error = "草稿写回失败: " + updated.error();
        return true;
    }

    reloadData(state);   // 重新读一遍，让列表和详情看到新标题

    // 自动接力：这一条成功了、还有欠账，就接着补下一条（点一次「补草稿」能补齐）。
    // 只在**成功**时接力 —— 失败还继续就是无限烧 token。
    if (done.error.empty())
    {
        if (core::Entry const* next = firstPendingDraft(state))
        {
            startDraftInBackground(next->id, next->raw_text);
        }
    }

    return true;
}

}  // namespace app
