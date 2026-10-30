#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 读文本文件并统一成 UTF-8。
//
// 账单的现实情况：
//   * 微信导出：UTF-8 **带 BOM**
//   * 支付宝导出（历史/部分版本）：**GBK**
// 所以不能直接读成 std::string 就完事 —— 编码错了的表现是"中文全是乱码"，
// 更糟的是 BOM 没去掉时列名变成 "\xEF\xBB\xBF交易时间"，按列名找列会静默失败。
namespace import
{

struct TextFile
{
    std::string              utf8;
    std::string              encoding;    // "utf-8" / "gbk" / "unknown"
    std::vector<std::string> warnings;
};

/**
 * 读文件并按需转换编码。
 *
 * 判定顺序：BOM(UTF-8) → 是否是合法 UTF-8 → 试 GBK → 都不行就原样返回 + warning。
 * 注意 GBK 转换在非 Windows 平台上直接跳过（我们只支持 Windows 构建）。
 */
std::expected<TextFile, std::string> readTextFile(const std::filesystem::path& file);

/** GBK → UTF-8。转换失败返回空。 */
std::optional<std::string> gbkToUtf8(std::string_view bytes);

/** 是不是合法的 UTF-8（用来判断"要不要走 GBK"）。 */
bool isValidUtf8(std::string_view bytes);

}  // namespace import
