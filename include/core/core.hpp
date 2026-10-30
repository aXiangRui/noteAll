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
    //
    // source 说明这个时间是"谁给的"（规则 / 模型 / 手改）。不传就按 Rule 算，
    // 没有 occurred 时一律记 None —— 不能出现"时间未知但来源写着规则"这种自相矛盾。
    std::expected<Entry, std::string> add(std::string_view      rawText,
                                          std::chrono::sys_seconds createdAt,
                                          std::optional<TimeRange> occurred = std::nullopt,
                                          TimeSource               source = TimeSource::Rule);

    // 按 created_at 倒序（新的在前）。**不含已删除的**（墓碑对上层不可见）。
    const std::vector<Entry>& list() const;

    // 含墓碑的全量（导出 / 恢复 / 排查用）。
    const std::vector<Entry>& listAll() const;

    // 只查未删除的。
    std::optional<Entry> find(std::string_view id) const;

    // 查含墓碑的全量。
    std::optional<Entry> findAny(std::string_view id) const;

    // 软删除：打墓碑（deleted_at），记录仍在文件里。找不到 / 已删除都是错误。
    // deletedAt 缺省取当前时间（墓碑不进时间轴，也不参与排序，所以不影响可测性）。
    std::expected<void, std::string> remove(std::string_view id,
                                            std::optional<std::chrono::sys_seconds> deletedAt = std::nullopt);

    // 撤销删除：清掉墓碑。
    std::expected<void, std::string> restore(std::string_view id);

    // 物理删除：真要清掉才用它（不可恢复）。
    std::expected<void, std::string> purge(std::string_view id);

    // 按 id 整体替换（改草稿、确认解析结果都走这里）。
    // 只允许改未删除的记录；id 必须在。
    std::expected<void, std::string> update(const Entry& entry);

private:
    // 每次修改前的固定动作：拿独占锁 -> 重新读盘 -> 用磁盘状态覆盖内存。
    //
    // 只加锁不重读是不够的：两个进程各自 load 之后再写，后者会拿旧快照
    // 覆盖前者的新增。重读才是真正解决"丢更新"的那一步。
    std::expected<void, std::string> reloadForMutation() ;

    // 由 entries_ 派生出 list() 用的可见列表（跳过墓碑）。
    void reindexVisible();

    std::filesystem::path    path_;
    std::vector<Entry>       entries_;    // 含墓碑
    std::vector<Entry>       visible_;    // 不含墓碑
    std::vector<std::string> warnings_;
};

// 12 位十六进制 = 48 bit 随机。不用递增计数器的理由见 ROADMAP.md §2.5。
std::string makeId(const std::vector<Entry>& existing);
}  // namespace core
