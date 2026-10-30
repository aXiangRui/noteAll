#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "import/csv.hpp"

// CSV 切分的边界用例 —— 这个文件就是"自己写解析器"的全部风险清单。
// 只要这些过，换库/自写都不影响上层适配层。

namespace
{

std::vector<std::string> row(std::size_t index, const import::CsvTable& table)
{
    REQUIRE(table.rows.size() > index);
    return table.rows[index];
}

}  // namespace

TEST_CASE("csv：最基本的两行")
{
    const auto table = import::parseCsv("a,b,c\n1,2,3\n");

    REQUIRE(table.rows.size() == 2);
    CHECK(row(0, table) == std::vector<std::string>{ "a", "b", "c" });
    CHECK(row(1, table) == std::vector<std::string>{ "1", "2", "3" });
    CHECK(table.warnings.empty());
}

TEST_CASE("csv：引号里的逗号不是分隔符")
{
    const auto table = import::parseCsv("\"北京, 朝阳\",32\n");

    REQUIRE(table.rows.size() == 1);
    CHECK(row(0, table) == std::vector<std::string>{ "北京, 朝阳", "32" });
}

TEST_CASE("csv：引号里的换行保留原文")
{
    const auto table = import::parseCsv("\"第一行\n第二行\",x\n");

    REQUIRE(table.rows.size() == 1);
    REQUIRE(row(0, table).size() == 2);
    CHECK(row(0, table)[0] == "第一行\n第二行");
    CHECK(row(0, table)[1] == "x");
}

TEST_CASE("csv：\"\" 表示一个字面引号")
{
    const auto table = import::parseCsv("\"他说\"\"你好\"\"\",x\n");

    REQUIRE(table.rows.size() == 1);
    CHECK(row(0, table)[0] == "他说\"你好\"");
    CHECK(row(0, table)[1] == "x");
}

TEST_CASE("csv：CRLF 行尾，字段里不残留 \\r")
{
    const auto table = import::parseCsv("a,b\r\n1,2\r\n");

    REQUIRE(table.rows.size() == 2);
    CHECK(row(0, table) == std::vector<std::string>{ "a", "b" });
    CHECK(row(1, table) == std::vector<std::string>{ "1", "2" });
}

TEST_CASE("csv：吃掉 UTF-8 BOM（微信账单带 BOM）")
{
    const auto table = import::parseCsv("\xEF\xBB\xBF交易时间,金额\n2026-10-30 12:00,32.00\n");

    REQUIRE(table.rows.size() == 2);
    CHECK(row(0, table)[0] == "交易时间");   // 不是 "\xEF\xBB\xBF交易时间"
    CHECK(row(0, table)[1] == "金额");
}

TEST_CASE("csv：末尾逗号产出空字段")
{
    const auto table = import::parseCsv("a,b,\n");

    REQUIRE(table.rows.size() == 1);
    CHECK(row(0, table) == std::vector<std::string>{ "a", "b", "" });
}

TEST_CASE("csv：纯空行被跳过，文件末尾的换行不额外产出行")
{
    const auto table = import::parseCsv("\n\na,b\n\n1,2\n\n");

    REQUIRE(table.rows.size() == 2);
    CHECK(row(0, table) == std::vector<std::string>{ "a", "b" });
    CHECK(row(1, table) == std::vector<std::string>{ "1", "2" });
}

TEST_CASE("csv：空文件 / 只有换行 → 0 行")
{
    CHECK(import::parseCsv("").rows.empty());
    CHECK(import::parseCsv("\n").rows.empty());
    CHECK(import::parseCsv("\r\n\r\n").rows.empty());
}

TEST_CASE("csv：最后一列没有换行符也能收")
{
    const auto table = import::parseCsv("a,b\n1,2");

    REQUIRE(table.rows.size() == 2);
    CHECK(row(1, table) == std::vector<std::string>{ "1", "2" });
}

TEST_CASE("csv：引号没闭合 → 把剩余当内容 + 一条 warning")
{
    const auto table = import::parseCsv("a,\"没闭合\n第二行");

    REQUIRE(table.rows.size() == 1);
    REQUIRE(row(0, table).size() == 2);
    CHECK(row(0, table)[0] == "a");
    CHECK(row(0, table)[1] == "没闭合\n第二行");
    REQUIRE(table.warnings.size() == 1);
    CHECK(table.warnings[0].find("引号") != std::string::npos);
}

TEST_CASE("csv：字段数不一致照收（残缺行交给调用方判断）")
{
    const auto table = import::parseCsv("a,b,c\n1,2\n1,2,3,4\n");

    REQUIRE(table.rows.size() == 3);
    CHECK(row(1, table).size() == 2);   // 少一列
    CHECK(row(2, table).size() == 4);   // 多一列
}

TEST_CASE("csv：换分隔符（Excel 在某些区域会用分号）")
{
    const auto table = import::parseCsv("a;b;c\n", ';');

    REQUIRE(table.rows.size() == 1);
    CHECK(row(0, table) == std::vector<std::string>{ "a", "b", "c" });
}
