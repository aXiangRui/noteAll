# 第三方库

这里 vendor 的是**单头文件（header-only）**库：源码直接放进项目，不用包管理器。

> **为什么 M0 不用 vcpkg**：`nlohmann/json` 和 `doctest` 都是单头文件，直接丢进
> `third_party/` 就完事，省掉一整套安装配置。真正需要 vcpkg 是 **M2** 接 `cpr`
> 的时候 —— 它依赖 libcurl，那才是真依赖。

| 库 | 版本 | 用途 | 文件 |
|---|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | JSONL 序列化 | `nlohmann/json.hpp` |
| [doctest](https://github.com/doctest/doctest) | 2.4.11 | 单元测试 | `doctest/doctest.h` |

## 校验下载是否完整

```bash
sha256sum third_party/nlohmann/json.hpp third_party/doctest/doctest.h
```

应为：

```
9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6  third_party/nlohmann/json.hpp
44faa038e9c3f9728efbda143748d01124ea0a27f4bf78f35a15d8fab2e039fb  third_party/doctest/doctest.h
```

## 重新下载（GitHub 直连不通时）

本机实测 **github.com 与 raw.githubusercontent.com 均不可达**，`gh-proxy.com` 可用：

```bash
curl -fsSL -o third_party/nlohmann/json.hpp \
  "https://gh-proxy.com/https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"

curl -fsSL -o third_party/doctest/doctest.h \
  "https://gh-proxy.com/https://github.com/doctest/doctest/releases/download/v2.4.11/doctest.h"
```

备选代理：`ghproxy.net`（较慢但可用）。
清华/中科大的 `github-release` 镜像**没有**收录这两个库（实测 404）。

> 升级版本时记得同步更新上面的 SHA256 和本表。

## 校验记录

| 日期 | 结果 |
|---|---|
| 2026-10-14 | ✅ 两个文件的 SHA256 与上表**逐字节一致**；`json.hpp` 宏为 `NLOHMANN_JSON_VERSION 3.11.3`，`doctest.h` 为 `DOCTEST_VERSION 2.4.11`（与 `tests.exe` 自报的 `doctest version is "2.4.11"` 吻合） |
