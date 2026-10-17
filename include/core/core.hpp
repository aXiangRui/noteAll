#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "entry.hpp"

// Core —— 门面（facade）。
//
// 对上层只暴露少量操作，把 store 的细节藏在里面。
// 这是 UI 唯一被允许接触的入口 —— 见 DESIGN.md §3 的依赖方向。
//
// 硬约束：这个头文件里不许出现任何跟"怎么显示"有关的东西。
namespace core
{

class Core
{
public:
    explicit Core(std::filesystem::path dataFile);

    // 启动时调一次。读盘失败的判断交给 main —— **构造函数没法报错**，
    // 所以读盘不能塞进构造函数。
    std::expected<void, std::string> load();

    // load() 跳过的坏行说明。交给上层决定显不显示。
    const std::vector<std::string>& warnings() const;

    // 追加一条：生成 id，落盘。返回写好的 Entry，因为 CLI 要打印 "已记录 <id>"。
    //
    // created_at 由调用方传进来，**不在这里取 now()**：
    //   1. parseTime 的 base 必须正好是 created_at（「上周三」相对的是记录时刻），
    //      两边各取一次 now()，午夜前后就可能差一天；
    //   2. 传进来之后 core 完全可预测，测试不用跟系统时钟赛跑。
    //
    // occurred 是解析器的结果（可空）。core 不认识 parse，也不该认识 ——
    // 谁解析谁负责，core 只负责把结果存下来。
    std::expected<Entry, std::string> add(std::string_view      rawText,
                                          std::chrono::sys_seconds createdAt,
                                          std::optional<TimeRange> occurred = std::nullopt);

    // 按 created_at 倒序（新的在前）
    const std::vector<Entry>& list() const;

    std::optional<Entry> find(std::string_view id) const;

    std::expected<void, std::string> remove(std::string_view id);

private:
    std::filesystem::path    path_;
    std::vector<Entry>       entries_;
    std::vector<std::string> warnings_;
};

// 12 位十六进制 = 48 bit 随机。不用递增计数器的理由见 ROADMAP.md §2.5。
std::string makeId(const std::vector<Entry>& existing);

}  // namespace core
