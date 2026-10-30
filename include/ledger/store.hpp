#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "ledger/ledger.hpp"

// 账本的 JSONL 持久化。
//
// 刻意和 core/store 同构（schema 头 + 原子写 + 容错读），但不复用它的代码：
// 那边序列化的是 Entry，这边是 Transaction —— 硬套只会让两边的 schema 纠缠。
// 真正共用的是更底层的两件东西：FileLock（互斥）和 appendLog（异常记录）。
namespace ledger
{

constexpr int kSchemaVersion = 1;

struct LoadedLedger
{
    std::vector<Transaction> transactions;
    std::vector<std::string> warnings;   // 坏行只警告，不拦人
};

std::expected<LoadedLedger, std::string> load(std::filesystem::path const& file);

std::expected<void, std::string> save(std::filesystem::path const&       file,
                                      const std::vector<Transaction>&    transactions);

}  // namespace ledger
