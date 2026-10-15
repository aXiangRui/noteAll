// doctest 的 main 入口。
// 整个测试可执行文件里，这个宏只能出现一次 —— 别把它挪到别的 .cpp 里。
// 测试用例写在 test_*.cpp 中，那些文件只需 #include <doctest/doctest.h>。

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
