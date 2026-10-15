#include <doctest/doctest.h>

// 这里写 store（JSONL 持久化）的单元测试。
//
// store 是 M0 里最容易出错、也最好测的一层，建议优先覆盖：
//   - 空文件 / 文件不存在时 load 的行为
//   - 一条 Entry 写出再读回，字段是否完全一致（往返测试 round-trip）
//   - raw_text 里含引号、换行、中文时，JSON 转义是否正确
//   - 原子重写：写一半失败，原文件是否还在
//
// 用 doctest 的写法：
//   TEST_CASE("往返测试") { CHECK(a == b); }
