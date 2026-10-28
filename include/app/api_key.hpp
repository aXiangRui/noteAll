#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// API key 的查找规则（CLI 和 GUI 共用一份）。
//
// 顺序：环境变量 NOTE_LLM_KEY → <数据文件目录>/note.key → %APPDATA%/noteAll/note.key
//
// 为什么要抽出来：main.cpp 报错时要列出"找过哪些路径"，GUI 设置页要显示
// "是否已配置"，草稿线程要真正读 key —— 三处各写一遍，迟早会不一致
// （最尴尬的是设置页说"已配置"、真生成时说"没找到"）。
namespace app
{

/** 读一个 .key 文件：整个文件内容就是 key（去 BOM、首尾空白、成对引号）。 */
std::optional<std::string> readKeyFile(const std::filesystem::path& path);

/** 按顺序找 key；找不到返回空。 */
std::optional<std::string> findApiKey(const std::filesystem::path& dataFile);

/** 会按顺序找过哪些路径（给报错文案用）。 */
std::vector<std::filesystem::path> apiKeySearchPaths(const std::filesystem::path& dataFile);

}  // namespace app
