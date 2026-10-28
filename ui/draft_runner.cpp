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
    std::string              entryId;
    std::string              title;
    std::string              summary;
    std::vector<std::string> tags;
    std::string              kind;
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

bool startDraftInBackground(std::string entryId, std::string rawText)
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

    std::thread worker([entryId = std::move(entryId), rawText = std::move(rawText)] {
        Result result;
        result.entryId = entryId;

        auto client = makeClient();
        if (!client)
        {
            result.error = client.error();
        }
        else
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
    return true;
}

}  // namespace app
