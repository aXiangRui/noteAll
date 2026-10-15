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
target_include_directories(third_party INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------------------
# core —— 领域逻辑
# 硬约束：不依赖任何 UI 概念（DESIGN.md §3）
# ---------------------------------------------------------------------------
add_library(core
    src/core/entry.cpp
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
# 可执行文件
# ---------------------------------------------------------------------------
add_executable(note main.cpp)
target_link_libraries(note PRIVATE core view parse)

# ---------------------------------------------------------------------------
# 工具 —— 手动调试用，不进测试
# ---------------------------------------------------------------------------
add_executable(parse_demo
    tools/parse_demo.cpp)
target_link_libraries(parse_demo PRIVATE parse)

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
