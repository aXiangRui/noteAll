#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "import/bill.hpp"
#include "import/charset.hpp"
#include "import/importer.hpp"
#include "ledger/book.hpp"

// 账单导入的适配层：编码、表头定位、两家格式、去重。
// 用的是**合成样本**（真实账单不能进仓库），字段结构和导出文件一致。

using namespace std::chrono;

namespace
{

std::filesystem::path scratchFile(char const* name)
{
    const auto dir = std::filesystem::temp_directory_path() / "noteall-import-test";
    std::filesystem::create_directories(dir);
    const auto file = dir / name;
    std::filesystem::remove(file);
    std::filesystem::remove(std::filesystem::path{ file }.concat(".lock"));
    std::filesystem::remove(std::filesystem::path{ file }.concat(".tmp"));
    return file;
}

// 微信账单：前面 4 行垃圾 + 表头 + 3 笔
const char* kWeChatBill =
    "微信支付账单明细,,,,,,,,\n"
    "微信昵称：[测试],,,,,,,,\n"
    "起始时间：[2026-10-01 00:00:00] 终止时间：[2026-11-01 00:00:00],,,,,,,,\n"
    "共 3 笔记录,,,,,,,,\n"
    "交易时间,交易类型,交易对方,商品,收/支,金额(元),支付方式,当前状态,交易单号,商户单号,备注\n"
    "2026-10-30 12:10:00,商户消费,美团,午饭,支出,¥32.00,零钱,支付成功,4200002000202610301234567890,,\n"
    "2026-10-29 08:40:00,商户消费,滴滴出行,打车,支出,¥18.50,零钱,支付成功,4200002000202610291234567891,,\n"
    "2026-10-26 09:00:00,转账,某某公司,工资,收入,¥12000.00,零钱,已存入零钱,4200002000202610261234567892,,\n";

// 支付宝账单：列名带全角括号和空格，还有一行虚线
const char* kAlipayBill =
    "支付宝交易记录明细查询\n"
    "账号:[测试]\n"
    "---------------------------------交易记录明细列表------------------------------------\n"
    "交易号                  ,商家订单号   ,交易创建时间          ,付款时间             ,"
    "金额（元） ,收/支 ,交易状态 ,交易对方 ,商品名称 ,备注 ,\n"
    "2026103022001234567890123456,T200P1234,2026-10-30 12:10:00,2026-10-30 12:10:05,"
    "32.00 ,支出 ,交易成功 ,美团 ,午饭 , ,\n";

}  // namespace

// ---------------------------------------------------------------------------
// 编码
// ---------------------------------------------------------------------------

TEST_CASE("charset：GBK 的「中文」能转成 UTF-8")
{
    // GBK: 中 = D6 D0, 文 = CE C4
    const std::string gbk("\xD6\xD0\xCE\xC4", 4);

    CHECK_FALSE(import::isValidUtf8(gbk));   // GBK 字节不是合法 UTF-8

    const auto utf8 = import::gbkToUtf8(gbk);
    REQUIRE(utf8.has_value());
    CHECK(*utf8 == "中文");
}

TEST_CASE("charset：合法 UTF-8 不会被误转")
{
    CHECK(import::isValidUtf8("中文 abc"));
    CHECK(import::isValidUtf8(""));
}

TEST_CASE("charset：读文件时去掉 BOM 并标出编码")
{
    const auto file = scratchFile("bom.csv");
    {
        std::ofstream out(file, std::ios::binary);
        out << "\xEF\xBB\xBF" << "交易时间,金额\n";
    }

    const auto text = import::readTextFile(file);
    REQUIRE(text.has_value());
    CHECK(text->encoding == "utf-8");
    CHECK(text->utf8 == "交易时间,金额\n");   // BOM 没了
}

// ---------------------------------------------------------------------------
// 表头定位 + 微信适配
// ---------------------------------------------------------------------------

TEST_CASE("bill：微信账单——跳过前导垃圾行，认出表头在第 5 行")
{
    const auto parsed = import::parseBill(kWeChatBill);

    CHECK(import::toString(parsed.source) == "微信");
    CHECK(parsed.headerLineNumber == 5);
    CHECK(parsed.headerLine.find("交易时间") != std::string::npos);

    REQUIRE(parsed.rows.size() == 3);

    const auto& first = parsed.rows[0];
    CHECK(first.amountCents == 3200);
    CHECK(ledger::toString(first.direction) == "expense");
    CHECK(first.category == "餐饮");
    CHECK(first.note == "美团 午饭");
    CHECK(first.externalId == "4200002000202610301234567890");

    // 收入行
    CHECK(ledger::toString(parsed.rows[2].direction) == "income");
    CHECK(parsed.rows[2].amountCents == 1200000);
}

TEST_CASE("bill：微信的「不计收支」被跳过并说明原因")
{
    const std::string bill =
        "交易时间,交易类型,交易对方,商品,收/支,金额(元),支付方式,当前状态,交易单号\n"
        "2026-10-30 12:00:00,零钱提现,自己,提现,/,¥100.00,零钱,提现已到账,123456\n";

    const auto parsed = import::parseBill(bill);
    CHECK(parsed.rows.empty());
    REQUIRE(parsed.skipped.size() == 1);
    CHECK(parsed.skipped[0].find("不计收支") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 支付宝适配
// ---------------------------------------------------------------------------

TEST_CASE("bill：支付宝账单——全角括号的「金额（元）」也能认")
{
    const auto parsed = import::parseBill(kAlipayBill);

    CHECK(import::toString(parsed.source) == "支付宝");
    REQUIRE(parsed.rows.size() == 1);

    const auto& row = parsed.rows[0];
    CHECK(row.amountCents == 3200);
    CHECK(ledger::toString(row.direction) == "expense");
    CHECK(row.note == "美团 午饭");
    CHECK(row.externalId == "2026103022001234567890123456");
}

// ---------------------------------------------------------------------------
// 认不出的格式
// ---------------------------------------------------------------------------

TEST_CASE("bill：认不出的格式要明确说「为什么 0 笔」")
{
    const auto parsed = import::parseBill("随便,什么,文件\n1,2,3\n");

    CHECK(import::toString(parsed.source) == "未知");
    CHECK(parsed.rows.empty());
    REQUIRE_FALSE(parsed.skipped.empty());
    CHECK(parsed.skipped[0].find("表头") != std::string::npos);
}

TEST_CASE("bill：时间或金额坏掉的行被跳过，不影响其它行")
{
    const std::string bill =
        "交易时间,交易类型,交易对方,商品,收/支,金额(元),支付方式,当前状态,交易单号\n"
        "2026-13-45 12:00:00,商户消费,美团,午饭,支出,¥32.00,零钱,支付成功,id-1\n"
        "2026-10-30 12:00:00,商户消费,美团,午饭,支出,¥??,零钱,支付成功,id-2\n"
        "2026-10-29 12:00:00,商户消费,美���,午饭,支出,¥18.00,零钱,支付成功,id-3\n";

    const auto parsed = import::parseBill(bill);

    REQUIRE(parsed.rows.size() == 1);   // 只有第三行是好的
    CHECK(parsed.rows[0].externalId == "id-3");
    CHECK(parsed.skipped.size() == 2);
}

// ---------------------------------------------------------------------------
// 去重
// ---------------------------------------------------------------------------

TEST_CASE("importBill：同一个账单导两次不会翻倍")
{
    const auto file = scratchFile("dedupe.jsonl");

    ledger::Book book{ file };
    REQUIRE(book.load().has_value());

    const auto parsed = import::parseBill(kWeChatBill);
    REQUIRE(parsed.rows.size() == 3);

    // 第一次：预览不写盘
    const auto preview = import::importBill(book, parsed, false);
    REQUIRE(preview.has_value());
    CHECK(preview->added == 3);
    CHECK(book.list().empty());   // 预览确实没写

    // 真正导入
    const auto first = import::importBill(book, parsed, true);
    REQUIRE(first.has_value());
    CHECK(first->added == 3);
    REQUIRE(book.list().size() == 3);

    // 再导一次：全靠交易号判重
    ledger::Book reopened{ file };
    REQUIRE(reopened.load().has_value());
    const auto second = import::importBill(reopened, parsed, true);
    REQUIRE(second.has_value());
    CHECK(second->added == 0);
    CHECK(second->duplicates == 3);
    CHECK(reopened.list().size() == 3);
}

TEST_CASE("importBill：软删除的账不会被重新导回来")
{
    const auto file = scratchFile("dedupe-deleted.jsonl");

    ledger::Book book{ file };
    REQUIRE(book.load().has_value());

    const auto parsed = import::parseBill(kWeChatBill);
    REQUIRE(import::importBill(book, parsed, true).has_value());

    // 用户删了一笔
    const std::string removedId = book.list()[0].id;
    REQUIRE(book.remove(removedId).has_value());
    CHECK(book.list().size() == 2);

    // 再导：那笔不该回来
    const auto again = import::importBill(book, parsed, true);
    REQUIRE(again.has_value());
    CHECK(again->added == 0);
    CHECK(book.list().size() == 2);
}
