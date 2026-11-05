# EUI-NEO 本地修改记录

本目录是 [EUI-NEO](https://github.com/sudoevolve/EUI-NEO) 的 vendored 副本，按
**Apache License 2.0** 使用（全文见本目录 `LICENSE`）。

本文件记录 PXCLauncher 对框架源码所作的修改。它**不能替代** Apache-2.0 §4(b) 要求的
文件内声明 —— 每个被改过的文件顶部都有一行
`// Modified from EUI-NEO by PXCLauncher (2026-11). See MODIFICATIONS.md in this directory.`
本文件的作用是解释这些声明背后到底改了什么。

## 基线与同步策略

| 项 | 值 |
|---|---|
| 上游仓库 | https://github.com/sudoevolve/EUI-NEO |
| 获取方式 | GitHub 源码压缩包（目录名带 `-main`，**无 git 历史**） |
| 获取时间 | 2026-09-22 |
| CMake 声明版本 | `project(EUI-NEO VERSION 0.6.0)` |
| 实际快照 | 晚于 `v0.6.0` tag（README、`site/`、`CONTRIBUTING.md` 等已含 tag 之后的上游更新） |
| 同步策略 | **不跟进上游更新**，长期固定在本快照 |

因为不打算跟进上游，本目录可以按需裁剪，不必保持与上游逐字节一致。

### 已移除的 `assets/`

整份删除（5.7 MB）。其中含 Les Gordon 的商业音乐与 Shadertoy 着色器（多为 CC BY-NC-SA），
公开仓库分发有版权风险，而启动器一个都不使用。

**注意连带影响**：框架的默认字体常量指向该目录 ——
`kDefaultUiFontFile = "JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf"`（界面字体）与
`kDefaultIconFontFile = "Font Awesome 7 Free-Solid-900.otf"`（`ui.text().icon()` 的图标字体），
见 `core/render/text.cpp:43`。目录删掉后 `resolveDefaultUiFontPath()` 会静默退回系统字体：
不报错、不进日志，只是汉字换了字形、图标全变缺字方框。

因此**宿主应用必须显式指定字体**：PXC 启动器把这两个字体收到自己的 `launcher/data/fonts/`，
在 `dslAppConfig()` 里用 `cfg.textFont(绝对路径)` / `cfg.iconFont(绝对路径)` 覆盖
（绝对路径是必须的 —— `resolveFontFilePath()` 只会去 `<exe>/assets` 下按文件名找）。
其他接收这份副本的应用若用 `ui.text().icon()`，同样要做这一步。

## 修改清单（13 个文件）

### A. 全局文本缩放

框架原本只有「整个界面一起缩放」，缺一个「只放大文字、控件框不动」的档位。PXC 启动器需要在
不改变布局的前提下让用户调大字号，因此在框架内核里加了一个独立于 `uiScale` 的字号倍率。

| 文件 | 改动 |
|---|---|
| `core/render/text.h` | 声明 `globalTextScale()` / `setGlobalTextScale()` |
| `core/render/text.cpp` | 实现上述接口；系数钳在 `[0.5, 3.0]` |
| `core/dsl.h` | `fontSize()` / `lineHeight()` 乘该系数；`lineHeight` 传 0（自动）时不乘，避免 0 变 0 |
| `include/eui/app.h` | 暴露 `app::setTextScale()` |
| `include/eui/dsl_app.h` | `AppConfig` 加字号倍率初值字段 |
| `include/eui/detail/dsl_app_impl.h` | 初值只在此处进内核；改系数后强制重排一次（字号写在已建元素上，不重排不生效） |

### B. 静默启动（首帧前不显示窗口）

GLFW 默认建窗即显示，而启动器双击 `.pxc` 时是先建窗、首帧才决定要不要露脸 —— 中间会闪一块空窗。

| 文件 | 改动 |
|---|---|
| `core/window/window_types.h` | `WindowRequest` 加 `visible` 字段 |
| `core/window/window_backend.cpp` | 按 `request.visible` 设置 `GLFW_VISIBLE` |
| `include/eui/app.h` | 暴露 `initialWindowVisible()` |
| `include/eui/dsl_app.h` | `AppConfig` 加对应字段 |
| `core/app/glfw_app_main.cpp` | 建窗请求里填入该字段 |

### C. 滑块双击复位

| 文件 | 改动 |
|---|---|
| `components/slider.h` | 新增内置「双击恢复默认值」：检测状态按 slider id 存在一张共享 map，拖拽一次=一次释放，无法伪造双击；需调用 `.default(norm)` 显式开启，不调用则无此行为 |

### D. 组件细项

| 文件 | 改动 | 动机 |
|---|---|---|
| `components/colorpicker.h` | 标题 `"Color"` 与确认按钮 `"Done"` 可通过可选 setter 替换 | 接中文界面 |
| `components/input.h` | 量宽与渲染统一使用「已乘缩放」的真实字号（`renderFont` / `renderLineH`），光标、选区、滚动边界全部按它算 | 不同文本缩放下光标位置会偏移 |
| `components/toast.h` | 字号可覆盖（`msgFont_`，0 = 沿用主题） | 汉化页的生成结果提示需要更紧凑 |

## 已知但未声明的差异

下列文件与上游 `v0.6.0` 存在差异，但差异来自**上游在 `v0.6.0` 之后的更新**，不是本项目改动，
因此没有加修改声明：

- `README.md`、`README.zh-CN.md`、`CONTRIBUTING.md`、`site/*`
- `core/runtime/runtime_input.h`、`core/runtime/runtime_instances.h`（与 `v0.6.0` 逐字节一致）

## 许可以及与项目自身协议的关系

Apache-2.0 不是 copyleft：PXCLauncher 自身的代码（`launcher/`、`pxc_detect/`、`tests/`、`tools/`）
不受本协议传染，可以采用任意协议，也不必开源。但本目录的**修改**仍受 Apache-2.0 约束，
分发时需满足 §4：随附许可副本、保留版权声明、标注修改。

`3rd/` 下的依赖各自独立授权（zlib / libpng / freetype / glfw / md4c / tray / yyjson），
许可证原文在各自的 `LICENSE*` 文件中，`tools/deploy.py` 会把它们一并复制到发行包。
