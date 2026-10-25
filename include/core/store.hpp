#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "entry.hpp"

// store —— JSONL 持久化层。
//
// 职责：把 Entry 读写成 JSONL 文件（每行一条 JSON）。
// 格式见 DESIGN.md §2.4，读写策略也见那里（启动全量读入，修改时原子重写）。
//
// 需要提供的能力大致是：
//   load(path)  -> loadResult     文件不存在时返回空，不报错
//   save(path, entries)                   原子：写临时文件 -> rename
//
// 注意：JSON 的转换函数（to_json / from_json）放这里，
// 不要放到 entry.hpp —— 让数据结构本身保持对序列化格式无感。
namespace core 
{
    /**
     * 数据文件的格式版本。
     *
     * 文件第一行是一条头部记录：{"schema": 2}。
     *   * 没有头部 = 版本 1（本项目最早期的文件），照常读，下次保存自动升级；
     *   * 版本号高于本常量 = 文件是更新版本的程序写的，**拒绝读取** ——
     *     让老程序去写新格式，比直接报错危险得多。
     *
     * 什么时候 +1：字段改名 / 删除 / 语义变化。只是"新增字段并给默认值"不用加。
     */
    constexpr int kSchemaVersion = 2;

    struct LoadResult 
    {
        std::vector<Entry>       entries;
        std::vector<std::string> warnings;   // 坏行说明，交给上层决定显不显示
    };

    // 打不开 / 读失败 -> 失败
    std::expected<LoadResult, std::string> load(const std::filesystem::path& path);

    // 原子写：先写 path.tmp，再 rename 覆盖 path
    std::expected<void, std::string> save(const std::filesystem::path& path,
                                        const std::vector<Entry>& entries);

    // ---- JSON 转换 ----
    // 名字必须是 to_json / from_json，一个字母都不能改：nlohmann 靠 ADL 按精确
    // 名字查找，改了它找不到，`json j = entry;` 直接编译不过。
    // 这是全项目唯一一处 snake_case，见 docs/ROADMAP.md §0.3。
    void to_json  (nlohmann::json& j, const Entry& e);
    void from_json(const nlohmann::json& j, Entry& e);
    void to_json  (nlohmann::json& j, const TimeRange& t);
    void from_json(const nlohmann::json& j, TimeRange& t);

    // ---- 时间 <-> ISO 8601（存 UTC 字符串，nlohmann 不支持 chrono）----
    std::string toIso(std::chrono::sys_seconds t);
    bool fromIso(std::string_view s, std::chrono::sys_seconds& out);   // 失败返回 false，不改 out
}  
