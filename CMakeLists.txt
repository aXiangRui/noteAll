cmake_minimum_required(VERSION 3.20)

project(noteAll
    VERSION 0.1.0
    DESCRIPTION "个人时间轴记事本"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局设置
# ---------------------------------------------------------------------------
# C++23：本机 MSVC 14.44 已完整支持，`std::expected` 可用（错误处理要用，见 docs/ROADMAP.md）。
# C++23 是 C++20 的超集，升上来没有代价。
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)            # 关掉编译器私有扩展，保证标准一致

# 可执行文件统一输出到 build/bin/。
# 注意：Visual Studio 是「多配置」生成器，会再套一层配置名，实际路径是
#     build/bin/Debug/note.exe
#     build/bin/Release/note.exe
# 在 VSCode 里走 launch.json 的 ${command:cmake.launchTargetPath} 即可，不用记路径。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# 编译选项
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(
        /utf-8            # 源码与字面量按 UTF-8 解析 —— 中文必需，见 DESIGN.md §8.1
        /W4               # 高警告等级：初学阶段把警告当错误看待
        /permissive-      # 严格标准一致性
        /Zc:__cplusplus   # 让 __cplusplus 报出真实的 C++ 版本号
    )

    # MSVC 把 getenv / strcpy 这类标准 C 函数标成"不安全"（C4996），
    # 建议换成 _s 后缀的微软私有版本。这里保持标准写法，关掉这组警告。
    # 注意：这只关掉"不安全"提示，/W4 的其余警告照旧。
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# 第三方库
# header-only 单头文件，直接 vendor 在 third_party/。
# 来源与校验值见 third_party/README.md。
# 到 M2 需要 cpr（依赖 libcurl）时，再引入 vcpkg。
# ---------------------------------------------------------------------------
add_library(third_party INTERFACE)
# SYSTEM：第三方头文件里的警告（nlohmann 就有一个 C4459）不该算到我们头上，
# 也不该为了它去改 vendor 进来的源码。
target_include_directories(third_party SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------------------
# core —— 领域逻辑
# 硬约束：不依赖任何 UI 概念（DESIGN.md §3）
# ---------------------------------------------------------------------------
add_library(core
    src/core/entry.cpp
    src/core/file_lock.cpp
    src/core/note_log.cpp
    src/core/store.cpp
    src/core/core.cpp)

# include/ 与 src/ 镜像对应：include/core/store.hpp <-> src/core/store.cpp
# 所以包含时带上模块前缀，写成 #include "core/store.hpp"
target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(core PUBLIC third_party)

# ---------------------------------------------------------------------------
# view —— 呈现层
# 单向依赖：只允许 view -> core，不许反向
# ---------------------------------------------------------------------------
add_library(view
    src/view/console_view.cpp)
target_link_libraries(view PUBLIC core)

# ---------------------------------------------------------------------------
# parse —— 中文时间解析
# 只依赖 core（TimeRange 在 core/entry.hpp），不依赖 view
# ---------------------------------------------------------------------------
add_library(parse
    src/parse/timeParse.cpp)
target_link_libraries(parse PUBLIC core)

# ---------------------------------------------------------------------------
# flow —— 草稿调度（取任务 / 重试 / 放弃 / 预算 / 陈旧丢弃）
# 依赖 core；不认识 llm 与 UI：生成器和存储都是构造时注入的
# ---------------------------------------------------------------------------
add_library(flow
    src/flow/draft_queue.cpp)
target_include_directories(flow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(flow PUBLIC core)

# ---------------------------------------------------------------------------
# workflow —— 编排引擎（mini-Dify 的数据模型 + 图算法 + 执行器）
# 依赖：只有 third_party（nlohmann/json 在公共头里暴露 Value）
# 不认识 core / llm / UI；节点处理器由上层注册（见 workflow/node_registry.hpp）
# ---------------------------------------------------------------------------
add_library(workflow
    src/workflow/graph.cpp
    src/workflow/node.cpp
    src/workflow/node_registry.cpp
    src/workflow/run_context.cpp
    src/workflow/engine.cpp)

target_include_directories(workflow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# run_context.hpp 直接暴露 nlohmann::json（Value 别名），所以是 PUBLIC。
target_link_libraries(workflow PUBLIC third_party)

# ---------------------------------------------------------------------------
# connect —— 网络传输（WinHTTP）
# 只管 HTTP 收发，不认识 DeepSeek、JSON 语义、提示词
#
# 平台约束：WinHTTP 是 Windows 系统组件，所以整块都在 if(WIN32) 里 ——
# 光把链接包进去是不够的，Linux 上连 winhttp.h 都没有，源文件根本编不过。
# ---------------------------------------------------------------------------
if(WIN32)
    add_library(connect
        src/LLM-connect/connect.cpp)

    target_include_directories(connect PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

    # 显式钉住 API 版本：WINHTTP_OPTION_DECOMPRESSION、WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    # 这类较新的选项要靠 _WIN32_WINNT 才可见，不定义时可能莫名其妙编不过。
    target_compile_definitions(connect PRIVATE _WIN32_WINNT=0x0A00)

    # PRIVATE 而不是 PUBLIC：链接 winhttp 是实现细节，不该传染给链接 connect 的上层。
    # 不链 third_party：这一层不认识 JSON，也不该认识（见上面的文件头注释）。
    # WinHTTP 是系统组件，不用第三方库；#pragma comment 也可以，
    # 但写在这里更显眼，换编译器时不会悄悄失效。
    target_link_libraries(connect PRIVATE winhttp)

    # -----------------------------------------------------------------------
    # llm —— 聊天协议层（OpenAI 兼容：DeepSeek / ollama / llama.cpp server 同一套）
    # 依赖方向：llm -> net(connect) + third_party；**不认识 core**
    # 草稿队列与确认流程在 workflow 层，不在这一层
    # -----------------------------------------------------------------------
    add_library(llm
        src/llm/draft.cpp
        src/llm/http_chat_client.cpp)

    target_include_directories(llm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    # third_party 只有 .cpp 里用到（nlohmann），所以 PRIVATE。
    target_link_libraries(llm PUBLIC connect PRIVATE third_party)
else()
    message(STATUS "connect：WinHTTP 只在 Windows 上可用，本平台跳过这个静态库")
endif()

# ---------------------------------------------------------------------------
# 可执行文件
#
# note 只链 core / view / parse —— 这就是"记录事情"这条主线。
# connect（字节收发）、llm（聊天协议）、flow（草稿调度）、workflow（编排引擎）
# 都还在、照样编译，但**没有接进这个程序**：它们是实验性的，等真要用时再接。
# ---------------------------------------------------------------------------
add_executable(note main.cpp)
target_link_libraries(note PRIVATE core view parse)

# 草稿功能需要 llm 层（Windows-only）。没它 note 其余功能照常。
if(TARGET llm)
    target_link_libraries(note PRIVATE llm)
endif()

# ---------------------------------------------------------------------------
# 工具 —— 手动调试用，不进测试
# ---------------------------------------------------------------------------
add_executable(parse_demo
    tools/parse_demo.cpp)
target_link_libraries(parse_demo PRIVATE parse)

# llm_smoke：手动验证「能不能真的发出一条消息」。
# key 从环境变量 NOTE_LLM_KEY 读，**不走命令行**（会进 shell 历史与进程列表）。
# 只有 Windows 上才有 llm 层。
if(TARGET llm)
    add_executable(llm_smoke
        tools/llm_smoke.cpp)
    # third_party：--draft 模式要用 nlohmann 解析模型回复
    target_link_libraries(llm_smoke PRIVATE llm third_party)
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
