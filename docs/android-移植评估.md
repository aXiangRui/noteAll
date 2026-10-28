# Android 移植评估（EUI-NEO）

> 结论先给：**技术上可行，模板是现成的，但别急着做**。
> 障碍不在 UI，在「构建链版本 + 时区 + HTTP 层」这三个地方。

## 一、EUI-NEO 提供什么

远端仓库有个独立的 **`Android` 分支**（`refs/heads/Android`），里面是一套完整的 Gradle + NDK 打包工程：

```
Android/
├── README.md                      ← 官方编译条件与命令
├── settings.gradle  build.gradle
├── gradlew  gradlew.bat           ← 自带 wrapper，不需要全局装 gradle
├── app/build.gradle               ← ndkVersion / minSdk / abiFilters / cmake 参数
├── app/src/main/AndroidManifest.xml
├── app/src/main/java/com/sudoevolve/euineo/MainActivity.java
├── app/src/main/cpp/android_app.cpp          ← C++ 入口（namespace app + dslAppConfig + compose）
├── cmake/CMakeLists.txt
├── cmake/eui_android_app.cmake               ← 真正的构建逻辑
└── scripts/build-apk.ps1                     ← 一键打包
```

**关键发现：主分支已经有 SDL2 后端**（`core/app/sdl2_app_main.cpp` + `EUI_WINDOW_BACKEND=sdl2`），
Android 分支只是在外面套了 Gradle 壳。所以移植不需要切分支，把 `Android/` 拷过来即可。

## 二、官方要求的编译条件 vs 这台机器

| 要求 | 机器现状 | 结论 |
|---|---|---|
| Windows 10/11 | ✅ | |
| JDK（JBR 或任意 17+） | ✅ `E:\jdk`（OpenJDK 17.0.2），`JAVA_HOME` 已设 | |
| SDK Platform 36 | ✅ `E:\AndroidSdk\platforms\android-36` | |
| Build-Tools | ✅ 35.0.0 / 36.0.0 | |
| **NDK `27.0.12077973`** | ⚠️ 只装了 **28.2.13676358** | **要装 27 或改 gradle 的 ndkVersion** |
| CMake 3.22.1+ | ✅ 4.4（但 4.x 对老 `cmake_minimum_required` 更严格，可能报错） | 可能要压制警告 |
| Vulkan 设备，minSdk 26 | 取决于你的手机 | **必须是真机**，模拟器多半不行 |
| 首次构建联网下 SDL2 2.32.10 | 需要 github / libsdl.org，国内可能要代理 | |

固定死的构建参数（`eui_android_app.cmake`）：

```cmake
set(EUI_WINDOW_BACKEND sdl2 FORCE)
set(EUI_RENDER_BACKEND vulkan FORCE)     # 只有 Vulkan，没有 GLES
set(CMAKE_CXX_STANDARD 17)               # ⚠️ 我们是 C++23
abiFilters "arm64-v8a"                   # 只有 64 位 ARM
```

## 三、我们的代码能不能直接用

| 我们的模块 | Android 上 | 说明 |
|---|---|---|
| `app/calendar`（周视图数据） | ✅ | 纯 chrono，但 `current_zone()` 有风险（见下） |
| `app/api_key` | ✅ | 文件读取 |
| `app/entry_service` | ⚠️ | 用了 `std::expected` → 需要 C++23 |
| `core`（store / lock / log） | ✅ | `file_lock.cpp` 有 POSIX 分支（`O_CREAT\|O_EXCL`） |
| `parse` | ✅ | 纯逻辑 |
| `ui/*`（周视图 / 设置 / 详情 / 新增） | ✅ | 只要 EUI 的 DSL 一样就能编译 |
| `ui/draft_runner` | ❌ | 依赖 `llm` → **WinHTTP，Windows 专属** |
| `note.exe`（CLI） | ❌ | `wmain` + WinHTTP，不移植 |

**四个必须处理的具体障碍：**

1. **C++ 标准冲突**：Android 的 cmake 强制 `CMAKE_CXX_STANDARD 17`，我们用了 `std::expected`（C++23）。
   → 给我们的 target 单独 `target_compile_features(... cxx_std_23)`，别全局改（怕搅乱 EUI）。
2. **时区**：`std::chrono::current_zone()` 在 NDK 的 libc++ 上不保证有 tzdata。
   → 需要兜底：`locate_zone` 失败时退成固定 +8 偏移（我们只在国内用，够用）。
   → **这是最大的技术不确定性**，要在真机上最先验证。
3. **数据路径**：Android 上 cwd 不可写，`app::dataPath()` 默认返回 `notes.jsonl` 会失败。
   → 由 Java 侧把 app 私有目录通过环境变量或 JNI 传进来。
4. **草稿功能不可用**：`llm`/`connect` 是 WinHTTP。要么不移植（只读查看），
   要么给 Android 写一个新的 HTTP 后端（libcurl / SDL_net）。**建议先不做**。

## 四、配置步骤（照这个做）

### 0. 装 NDK 27（或改版本）

```powershell
# 方式 A：装官方指定的 27（约 3 GB）
& "E:\AndroidSdk\cmdline-tools\latest\bin\sdkmanager.bat" "ndk;27.0.12077973"

# 方式 B：复用已有的 28，改 Android/app/build.gradle 里的
#   ndkVersion "27.0.12077973"  →  "28.2.13676358"
```

### 1. 把打包工程拷进我们的仓库

```powershell
# 在 main 的 checkout 里执行（Android 分支已 fetch 到 FETCH_HEAD）
cd third_party\EUI-NEO
git archive FETCH_HEAD Android | tar -x -C ..\   # 解出 Android/ 目录
```

### 2. 改 `Android/cmake/eui_android_app.cmake`

```cmake
# EUI 源码指向我们的 third_party（而不是 ../.. 即仓库根）
set(EUI_NEO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/EUI-NEO")

# 用我们的 UI 而不是示例
set(EUI_ANDROID_APP_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../../app.cpp")

# main 里加上我们的库（app / core / parse），并排除 Windows-only 的东西
add_library(main SHARED
    "${EUI_NEO_ROOT}/core/app/sdl2_app_main.cpp"
    "${EUI_ANDROID_APP_SOURCE}"
    "${CMAKE_CURRENT_LIST_DIR}/../../../ui/state.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../ui/week_page.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../ui/settings_page.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../ui/detail_panel.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../ui/compose_panel.cpp"
    ... core/parse/app 的源文件 ...)
target_compile_features(main PRIVATE cxx_std_23)
```

### 3. 处理 assets

Gradle 的 `syncEuiAssets` 目前只拷 `../../assets`（EUI 自己的字体/图标）。
我们的字体在 `assets/gui/`，还要把 EUI 的 `assets/` 也一起塞进 APK。

### 4. 打包 + 安装

```powershell
cd Android
.\gradlew.bat :app:assembleDebug
& "$env:ANDROID_HOME\platform-tools\adb.exe" install -r app\build\outputs\apk\debug\app-debug.apk
& "$env:ANDROID_HOME\platform-tools\adb.exe" shell am start -n com.sudoevolve.euineo/.MainActivity
```

## 五、工作量与建议

| 阶段 | 内容 | 预计 |
|---|---|---|
| POC | 用**它自己的示例 app** 打出一个 APK，确认 Gradle/NDK/Vulkan 链路通 | 1–2 小时（大部分是下载） |
| 移植只读周视图 | 接我们的 app/core/parse + ui，注入数据路径 | 半天–1 天 |
| 时区兜底 | 真机验证 `current_zone()`，不行就固定偏移 | 1–2 小时 |
| 草稿（可选） | 换一个 HTTP 后端（libcurl/SDL_net） | 再 1–2 天，不建议现在做 |

**建议：先做 POC（第 0+1+4 步），不改我们任何代码。**
如果它自己的示例 app 能在你手机上跑起来，说明构建链没问题，再谈移植；
如果 POC 就卡在 NDK/Vulkan/网络下载，那这次就不值得投入。

## 六、还没验证的事

- 手机是否支持 Vulkan（`adb shell dumpsys | findstr vulkan` 或装个 AIDA64 看）；
- NDK 28 能不能编过（可能要压 CMake 4.x 的兼容性警告）；
- 首次 Gradle 构建要联网（Gradle 发行包 + SDL2 源码）；
- EUI 在 Android 上的**中文输入法**支持（真要做编辑功能必须先验，跟 Windows 那次一样）。
