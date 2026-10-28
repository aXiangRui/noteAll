# assets/gui —— 我们自己的界面资源

这个目录会被 CMake 在链接后**拷到 exe 旁边**（`<exe>/assets/`），
因为 EUI-NEO 找字体时只看文件名、并且优先找 `<exe>/assets/<文件名>`。

## 字体

| 文件 | 用途 | 大小 |
|---|---|---|
| `SourceHanSansSC-Regular-2.otf` | 界面正文（默认字体） | 15.7 MB |

设置方式：`app.cpp` 里的 `DslAppConfig{}.textFont("SourceHanSansSC-Regular-2.otf")`。

**来源**：思源黑体（Source Han Sans SC Regular），Adobe 与 Google 联合发布。
**许可**：SIL Open Font License 1.1 —— 可以随程序一起分发，但不能单独售卖字体本身。
（正式分发时建议把 OFL 许可文本一起带上；个人使用不必纠结。）

原始下载：<https://github.com/adobe-fonts/source-han-sans/releases>

## 换字体的步骤

1. 把 `.otf` / `.ttf` 放进这个目录（**必须是扁平结构**，不要建子目录 ——
   EUI-NEO 只取文件名去找 `assets/<文件名>`）；
2. 改 `app.cpp` 里的 `.textFont("文件名.otf")`；
3. 重新构建 —— CMake 的 POST_BUILD 会自动拷到 exe 旁边。

注意：字体文件很大，换字体时别把四个字重全放进来（每个 15 MB 以上）。
真要多字重，先确认 `TextBuilder::fontWeight()` 能按名字挑到对应文件。
