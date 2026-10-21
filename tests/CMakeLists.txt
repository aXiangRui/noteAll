add_executable(tests
    main.cpp            # doctest 的 main 入口（别往里加用例）
    test_store.cpp      # JSONL 持久化
    test_core.cpp       # Core 门面
    test_draft_queue.cpp # 草稿队列（flow，桩实现，用例先红）
    test_workflow.cpp    # 编排引擎（workflow，图算法已实现，执行器先红）
    test_parse.cpp)

target_link_libraries(tests PRIVATE core parse flow workflow)

# connect 是 Windows-only：用例也跟着门控，否则非 Windows 上 configure 就挂了。
if(TARGET connect)
    target_sources(tests PRIVATE test_connect.cpp)
    # ws2_32 是测试自己的回环服务器要用的（connect 库本身不依赖它）
    target_link_libraries(tests PRIVATE connect ws2_32)
endif()

# llm 也只在 Windows 上存在（它依赖 connect）。
if(TARGET llm)
    target_sources(tests PRIVATE test_chat.cpp)
    target_link_libraries(tests PRIVATE llm)
endif()

add_test(NAME unit COMMAND $<TARGET_FILE:tests>)
