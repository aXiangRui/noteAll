# 测试分两个目标：
#
#   tests             主线 = 记录功能：store / core / durability / parse
#                     —— 必须全绿，注册进 ctest。
#   tests_experiments 暂缓的模块：connect / llm（已绿）、flow 草稿队列（桩）、
#                     workflow 执行器（桩）—— 保留用例、照样编译（防止代码烂掉），
#                     但**不进 ctest**，免得长期红灯把 CI 信号冲成噪声。
#
# 重新捡起来时：cmake --build build --target tests_experiments
# 等某一块真正接线了，把对应文件搬回 tests 即可。

add_executable(tests
    main.cpp             # doctest 的 main 入口（别往里加用例）
    test_store.cpp       # JSONL 持久化
    test_core.cpp        # Core 门面
    test_durability.cpp  # 数据基石：格式版本 / 墓碑 / 来源 / 文件锁
    test_parse.cpp)      # 中文时间解析

target_link_libraries(tests PRIVATE core parse)

# ---------------------------------------------------------------------------
# 实验性模块（暂缓，不进 ctest）
# ---------------------------------------------------------------------------
add_executable(tests_experiments
    main.cpp
    test_draft_queue.cpp   # flow：草稿队列，10 条先红
    test_workflow.cpp)     # workflow：编排引擎，执行器 1 条先红

target_link_libraries(tests_experiments PRIVATE core flow workflow)

# Windows-only：网络层（connect / llm）也只在 Windows 上存在
if(TARGET connect)
    target_sources(tests_experiments PRIVATE test_connect.cpp)
    # ws2_32 是测试自己的回环服务器要用的（connect 库本身不依赖它）
    target_link_libraries(tests_experiments PRIVATE connect ws2_32)
endif()

if(TARGET llm)
    target_sources(tests_experiments PRIVATE test_chat.cpp)
    target_link_libraries(tests_experiments PRIVATE llm)
endif()

add_test(NAME unit COMMAND $<TARGET_FILE:tests>)
