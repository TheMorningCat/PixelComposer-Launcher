# Pixel Composer 启动器

面向 [Pixel Composer](https://pixelcomposer.com/) 的 Windows 桌面启动器：统一管理多个已安装版本、按工程文件快速启动，并把汉化流程做成可视化操作。

C++17 + [EUI-NEO](EUI-NEO-main) 即时模式 UI，产出单个 `pxc_launcher.exe`（静态 CRT，不依赖 VC++ 运行库）。

> 本项目与 Pixel Composer 官方无关，是第三方辅助工具。它不包含、也不分发 Pixel Composer 本体。

## 功能

| 页面 | 能力 |
|---|---|
| **版本库** | 自动发现 Steam / itch / 便携版安装；卡片式列表，显示版本号、渠道与「便携」标记；版本号自动识别（直读 exe 内嵌数据包，不依赖不可靠的 PE 版本资源）；手动添加 exe、设为默认启动版本、删除条目 |
| **工程文件** | 汇总工程库与你最近打开过的工程（`全部 = 工程库 ∪ 启动历史`，按路径去重）；双击即用对应版本打开；PXC 已在运行时复用那个实例而不是再开一个；支持拖放与命令行 |
| **汉化** | 从已安装的 PXC 目录取英文骨架 → 按对照表 + 自定义译文生成中文语言文件 → 写入 PXC 用户目录的 `keys.json`；可在启动前自动生成 |
| **设置** | 文本缩放（75%~150%）、界面字体、主题色；内置汉化包构建器 |

## 构建

**要求**：Windows 10/11 + Visual Studio 2022（含「使用 C++ 的桌面开发」工作负载）+ CMake 3.20+

```bat
python tools\run.py cfg                     :: 配置，生成 tmp\build
python tools\run.py build pxc_launcher      :: 构建产品
python tools\deploy.py                      :: 把 exe 与 data\ 发布到 launcher\bin
```

产物在 `launcher\bin\pxc_launcher.exe`，直接运行即可。

`tools\run.py` 按自身位置推导仓库根目录，并自动探测 `cmake`（环境变量 `CMAKE_EXE` → `PATH` → Visual Studio 自带的那份），不写死本机路径。也可以直接用 CMake：

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DEUI_ENABLE_TRAY=OFF ^
      -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build build --config Release --target pxc_launcher
```

## 测试

```bat
python tools\run.py build pxc_project_test
tmp\build\launcher\Release\pxc_project_test.exe
```

纯逻辑回归（工程索引、版本顾问、登记表去重、路径归一等），不链接 EUI、不依赖任何外部样本，任何人 clone 下来都能跑。当前 174 项。

## 目录结构

```
CMakeLists.txt          顶层：引入 EUI-NEO 与 launcher
launcher/               启动器本体
  *.cpp *.h             29 个模块（版本库/汉化/工程/备份/复用等）
  data/                 随程序分发的资源：字体、图标、对照表 translation_db.json
  res/                  app.rc + launcher.ico（由 tools/mkicon.py 生成）
pxc_detect/             识别内核：从 exe 内嵌数据包探测版本（也可独立编译，见 build.bat）
tests/                  headless 回归测试
tools/                  构建与发布脚本（run.py / deploy.py / mkicon.py）
docs/                   设计文档
素材/                   logo 源文件（mkicon.py 的输入）
EUI-NEO-main/           vendored UI 框架（含其 LICENSE）
```

## 不包含的内容

仓库**不含**下列本机样本，它们只在开发机上存在，不影响构建与运行：

- Pixel Composer 本体与安装目录副本
- 各类汉化包（`汉化文件/`）
- 工程样本、用户目录结构副本、备份与设计参考图
- 依赖上述样本的开发期诊断工具（`pxc_backend_test` / `pxc_gen_test` / `pxc_db_build` / `pxc_gen_cli`，见 `launcher/CMakeLists.txt` 的 `PXC_DEV_TOOLS` 选项，默认关闭）

## 许可

源代码采用 [MIT](LICENSE) 许可。

**图标不在授权范围内。** `素材/` 下的 logo 源文件、`launcher/res/launcher.ico`，以及由
`tools/mkicon.py` 生成的一切图标，保留所有权利。

## 第三方

- [EUI-NEO](EUI-NEO-main) —— vendored UI 框架，Apache License 2.0；本地改动逐项记录在
  [EUI-NEO-main/MODIFICATIONS.md](EUI-NEO-main/MODIFICATIONS.md)
- Noto Sans SC、Fira Code、JetBrains Mono —— SIL Open Font License 1.1
- zlib 1.3.1 —— 见 `EUI-NEO-main/3rd/zlib-1.3.1`
- Steam / itch.io 图标为各平台商标，仅用于标识渠道
