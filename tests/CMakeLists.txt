add_executable(tests
    main.cpp            # doctest 的 main 入口（别往里加用例）
    test_store.cpp
    test_parse.cpp)

target_link_libraries(tests PRIVATE core parse)

add_test(NAME unit COMMAND $<TARGET_FILE:tests>)
