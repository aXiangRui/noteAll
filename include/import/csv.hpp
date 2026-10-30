#pragma once

#include <string>
#include <string_view>
#include <vector>

// CSV 切分：只做"文本 → 行×列"，不碰编码、不碰表头、不碰业务。
//
// 为什么自己写而不是引库（见对话里的取舍）：
//   * 需要满足的语法就 RFC4180 那几条，40 行状态机；
//   * 真正麻烦的事（GBK 转换、前导垃圾行、列名映射、按交易号去重）库一样都不管；
//   * 自己写，边界用例就在自己的测试里，出问题不用跨库猜。
//
// 刻意**不做**的事：
//   * 不做字符集转换（那是 MultiByteToWideChar 的活，自己写 GBK 表是另一种坑）；
//   * 不 trim 字段（适配层知道哪些列该 trim，这里保持原样）；
//   * 不校验每行列数（账单里确实有残缺行，交给调用方判断）。
namespace import
{

struct CsvTable
{
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string>              warnings;   // 坏数据只警告，不抛异常
};

/**
 * 切分 UTF-8 文本。
 *
 * 行为约定：
 *   * 吃掉开头的 UTF-8 BOM（微信账单带 BOM，支付宝历史上是 GBK —— 编码得先转好再进来）；
 *   * 行尾 `\r\n` / `\n` / 单独的 `\r` 都认；
 *   * 引号字段里的 `,`、换行、`""`（转义引号）都按字面处理；
 *   * 末尾逗号产出空字段（`a,` → {"a", ""}）；
 *   * **纯空行被跳过**（否则文件末尾的换行会凭空多出一行）；
 *   * 引号没闭合 → 把剩下的全当内容，并记一条 warning。
 *
 * @param delimiter 分隔符，账单一般就是 ','，但留个口子（欧洲/Excel 会用 ';'）
 */
CsvTable parseCsv(std::string_view text, char delimiter = ',');

}  // namespace import
