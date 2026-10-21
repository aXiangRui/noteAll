#pragma once

#include <chrono>
#include <deque>
#include <thread>
#include <utility>
#include <vector>

#include "llm/chat.hpp"

// 测试专用：脚本化的假客户端。
//
// workflow 层（草稿队列 / 重试 / 毒丸 / 陈旧判定）全靠它测 —— 不需要网络、
// 不需要 API key，还能精确控制"第 N 次调用失败"这种时序。
namespace testutil
{

class FakeChatClient final : public llm::IChatClient
{
public:
    using Result = std::expected<llm::ChatReply, llm::ChatError>;

    explicit FakeChatClient(std::vector<Result>         script,
                            std::chrono::milliseconds   delay = std::chrono::milliseconds{0})
        : script_(script.begin(), script.end()), delay_(delay)
    {
    }

    std::expected<llm::ChatReply, llm::ChatError> complete(llm::ChatRequest const& request) override
    {
        ++calls_;
        last_request_ = request;

        if (delay_.count() > 0) { std::this_thread::sleep_for(delay_); }

        if (script_.empty())
        {
            // 脚本用尽要能被发现，不能悄悄返回一个"成功"。
            return std::unexpected(llm::ChatError{llm::ChatErrorKind::BadResponse, 0,
                                                  "FakeChatClient 脚本用尽"});
        }

        Result result = std::move(script_.front());
        script_.pop_front();
        return result;
    }

    [[nodiscard]] int calls() const noexcept { return calls_; }
    [[nodiscard]] llm::ChatRequest const& lastRequest() const noexcept { return last_request_; }

private:
    std::deque<Result>        script_;
    std::chrono::milliseconds delay_;
    int                       calls_ = 0;
    llm::ChatRequest          last_request_;
};

}  // namespace testutil
