#pragma once

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

    // 追加一条：生成 id + created_at(UTC)，落盘。返回写好的 Entry，
    // 因为 CLI 要打印 "已记录 <id>"。
    std::expected<Entry, std::string> add(std::string_view rawText);

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
