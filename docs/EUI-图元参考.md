# EUI-NEO 绘制图元参考

> 这份文档是为「在 note_gui 里画东西」准备的速查。所有内容都对着
> `third_party/EUI-NEO` 的源码核对过，行号指那个目录下的文件。
> 用法示例都来自我们自己的 `app.cpp`（周视图）。

## 1. 图元住在哪

```
third_party/EUI-NEO/
├── include/eui_neo.h          ← 我们的程序只 include 这个（门面）
├── include/eui/dsl.h          ← 转手 include core/dsl.h
├── core/dsl.h                 ★ 全部图元与 Builder 在这里（2400+ 行）
├── core/render/               ← 真正光栅化：矩形、字形、图片（text.cpp 管字体）
├── core/layout（在 runtime/）  ← 布局求解：flex 式排布
├── core/window/               ← GLFW 窗口 + 输入法（ImmGetContext / WM_IME_*）
└── core/runtime/              ← 元素树、状态、动画、重绘
```

**你只需要认识 `core/dsl.h`。** 其它几层是它的实现，除非要改框架，不用碰。

## 2. 坐标系与尺寸

- 原点在**左上角**，x 向右、y 向下，单位是 `float`（逻辑像素）。
- `compose(eui::Ui& ui, const eui::Screen& screen)` 里的 `screen.width / screen.height`
  就是窗口内容区尺寸 —— 所有布局都相对它算，别写死。
- 尺寸可以直接给 `float`，也可以 `.fill()`（撑满父容器，`dsl.h:356`）。
- 没有"自动流式排版"的硬性要求：**`.position(x, y)` 绝对定位永远是合法的**，
  这也是本项目的用法（七列各算各的 x）。

## 3. 图元清单（`Ui` 的工厂方法，`dsl.h:1396` 起）

| 方法 | 行号 | 返回 | 用途 |
|---|---|---|---|
| `ui.column(id)` | 1400 | `LayoutBuilder` | 竖排容器 |
| `ui.row(id)` | 1396 | `LayoutBuilder` | 横排容器 |
| `ui.stack(id)` | 1408 | `LayoutBuilder` | 堆叠容器（子元素自己定位；**唯一的裁剪容器**） |
| `ui.flow(id)` | 1404 | `LayoutBuilder` | 流式换行容器 |
| `ui.rect(id)` | 1416 | `RectBuilder` | 矩形（卡片底、竖线、圆点都靠它） |
| `ui.text(id)` | 1428 | `TextBuilder` | 文字 / 图标字形 |
| `ui.image(id)` | 1432 | `ImageBuilder` | PNG/JPG |
| `ui.svg(id)` | 1436 | `SvgBuilder` | SVG（继承 ImageBuilder） |
| `ui.polygon(id)` | 1424 | `PolygonBuilder` | 多边形 |
| `ui.shadertoy(id)` | 1420 | `ShadertoyBuilder` | 着色器画面 |
| `ui.loader(id)` | 1412 | `LoaderBuilder` | 延迟加载子树 |

另外两个非图元但常用的：`ui.state<T>(id)`（`dsl.h:1488`，按 id 存一份局部状态）、
`ui.isFocused(id)`（`dsl.h:1483`）。

## 4. 所有 Builder 共有的方法（`BuilderBase`，`dsl.h:304–820`）

| 方法 | 行号 | 说明 |
|---|---|---|
| `.x(v)` / `.y(v)` / `.position(x, y)` | 304 / 310 / 316 | 绝对定位（相对父容器） |
| `.width(v)` / `.height(v)` / `.size(w, h)` | 324 / 334 / 344 | 尺寸；`v` 可以是 `float` 或 `SizeValue` |
| `.fill()` | 356 | 撑满父容器 |
| `.wrapContent()` | 362 | 尺寸由内容决定 |
| `.margin(...)` | 368 | 外边距（1/2/4 参数三种重载） |
| `.padding(...)` | 403 | 内边距（同样三种重载） |
| `.gap(v)` / `.lineGap(v)` | 393 / 398 | 子元素间距 / 多行间距 |
| `.justifyContent(a)` / `.alignItems(a)` / `.align(main, cross)` | 428 / 433 / 438 | flex 排布 |
| `.zIndex(n)` | 497 | 层序 |
| `.clip(true)` | 502 | **裁剪子元素**（只有开了它的容器才不溢出） |
| `.opacity(v)` / `.scale(v)` / `.rotate(rad)` | 573 / 604 / 614 | 变换 |
| `.onClick(fn)` / `.onHover(fn)` | 654 / 701 | 事件 |
| `.transition(...)` / `.animate(...)` | 795 / 810 | 过渡动画（要配 `dirtyKey` 才稳定） |
| `.dirtyKey(s)` | 805 | 告诉框架"这一帧哪些参数变了"，避免每帧重建 |
| `.content([&]{ ... })` | 820 | 容器里声明的子元素 |

### 形状专属（`ShapeBuilderBase`，`dsl.h:836`）

| 方法 | 行号 |
|---|---|
| `.color(Color)` | 840 |
| `.radius(float)` | 855 |
| `.border(width, Color)` 或 `.border(Border)` | 860 / 865 |
| `.shadow(blur, offsetY, Color)` / `.shadow(blur, offsetX, offsetY, Color)` / `.shadow(Shadow)` | 870 / 875 / 890 |
| `.blur(v)` | 895 |

颜色是 `eui::Color{ r, g, b, a }`，各分量 `0..1`：

```cpp
const eui::Color kCardBg{ 0.16f, 0.18f, 0.23f, 1.0f };
```

### 文字专属（`TextBuilder`，`dsl.h:1137`）

| 方法 | 行号 | 说明 |
|---|---|---|
| `.text(s)` | 1141 | 内容 |
| `.fontFamily(name)` | 1146 | 字体名或字体文件名（见第 6 节） |
| `.fontSize(px)` | 1151 | 字号 |
| `.fontWeight(w)` | 1156 | 字重 |
| `.color(Color)` / `.opacity(v)` | 1161 / 1166 | 颜色 / 透明度 |
| `.maxWidth(px)` | 1171 | 换行宽度 |
| `.wrap(true)` | 1176 | 允许换行 |
| `.lineHeight(px)` | 1191 | 行高 |
| `.horizontalAlign(a)` / `.verticalAlign(a)` | 1181 / 1186 | 对齐 |
| `.icon(codepoint)` / `.icon(s)` | 1196 / 1202 | 画图标字体里的字形（`Font Awesome` 那个 .otf） |

## 5. 三个真实套路（来自 `app.cpp` 的周视图）

### ① 一张卡片：容器 + 底色 + 两行字

```cpp
ui.rect(itemId + ".card")
    .position(4.0f, y)                             // 相对父容器
    .size(columnWidth - 8.0f, 50.0f)
    .radius(8.0f)
    .color(kCardBg)
    .build();

ui.text(itemId + ".time")
    .position(28.0f, y + 4.0f)
    .size(columnWidth - 36.0f, 16.0f)
    .text("15:00")
    .fontSize(11.0f)
    .color(kMuted)
    .build();
```

> **每个图元的 id 必须全局唯一**。我们的写法是 `"day3." + entry->id + ".title"`，
> 用记录 id 保证不撞。撞了的表现很奇怪：状态互相串、文字闪。

### ② 时间线：一根 2px 的竖线 + 圆点

竖线 = 细长 rect；圆点 = 正方形 + `radius = 边长/2`：

```cpp
ui.rect(id + ".line").position(lineX + 3.0f, 50.0f).size(2.0f, 高).radius(1.0f).color(kLine).build();
ui.rect(id + ".dot").position(lineX - 2.0f, y + 8.0f).size(10.0f, 10.0f).radius(5.0f).color(kindColor).build();
```

### ③ 分栏：自己算 x

```cpp
const float columnWidth = (width - gap * 6.0f) / 7.0f;
for (int i = 0; i < 7; ++i) {
    const float x = pad + i * (columnWidth + gap);
    composeDayColumn(ui, columns[i], i, x, top, columnWidth, height);
}
```

**容器要裁剪时分两步**：先 `ui.stack(...)`，再 `.clip()`，子元素在 `.content([&]{...})`
里用**相对坐标**（0,0 是该容器的左上角）。

## 6. 字体与资源（踩过的坑）

- 字体查找顺序（`core/render/text.cpp`，`resolveFontFilePath`）：
  原样 → **`<exe>/assets/<文件名>`** → `<exe>/<路径>` → `assets/<文件名>` →
  `../assets/...` → `../../assets/...`。
- **只取文件名**：`textFont("fonts/a.ttf")` 实际找的是 `assets/a.ttf`，
  所以字体必须**平铺**在 `assets/` 根下。
- 默认正文字体是自带的中文字体 `JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf`，
  想要别的就在 `DslAppConfig{}` 里 `.textFont("xxx.ttf")`。
- `assets/` 是构建时由 `eui_neo_copy_assets` 从 EUI 自己的目录 **拷**过来的；
  自己的资源要放仓库里再在 CMake 加一条 POST_BUILD copy，否则下次构建就没了。

## 7. 重建与性能（写 UI 时最容易犯的错）

- `compose()` 会被**反复调用**（每次状态变化/重绘），所以里面不能做 IO、不能读文件。
  我们的做法：数据只在启动和点「刷新」时读（`reload()`），`compose` 只读内存快照。
- 每帧都新建 builder 对象是正常开销；但**别每帧改 `std::vector` 大小**，会拖慢。
- 元素数量大（几百行）用组件库的 `components::virtualList`（只渲染可见行），
  别自己 for 循环堆 rect。

## 8. 组件库（`components/`，比图元高一层）

图元是"矩形/文字"，组件是"按钮/输入框/列表/表格"，直接拿来用能省很多事：

| 组件 | 文件 | 备注 |
|---|---|---|
| `components::button` | `button.h` | `.text() / .onClick()` |
| `components::input` | `input.h` | 多行、IME、`.onChange() / .onEnter()` |
| `components::virtualList` | `virtuallist.h` | `.itemCount() / .rowHeight() / .row(fn)` |
| `components::scroll` / `scrollView` | `scroll.h` / `scrollview.h` | |
| `components::datatable` | `datatable.h` | |
| `components::markdown` | `markdown.h` | Markdown 渲染 |
| `components::dialog` / `contextmenu` / `toast` / `tooltip` | 同名 | |
| `components::checkbox` / `radio` / `switch` / `slider` / `stepper` | 同名 | |

完整清单：`third_party/EUI-NEO/components/`，文档：`third_party/EUI-NEO/docs/`（中文）。

## 9. 快速自查清单

写完一段 UI，按这个顺序看：

1. id 有没有重复？（撞了状态会串）
2. 需要裁剪的地方开 `.clip()` 了吗？
3. `compose` 里有没有 IO / 分配大数组？
4. 数字是相对 `screen.width/height` 算的吗？（窗口一拉就错位 = 这里）
5. 中文显示不出来 → 检查字体文件是否真的在 `<exe>/assets/` 下。
