# 内置字体与许可

本目录的字体随启动器一起分发（`tools/deploy.py` 会把整个 `launcher/data` 复制到
`launcher/bin/data`）。全部为 SIL Open Font License 1.1，全文见同目录 `OFL-1.1.txt`。

| 文件 | 名称 | 版权 | 用途 |
|---|---|---|---|
| `JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf` | 荆南俊俊体 | © 荆南字坊（武汉光谷字造创意有限公司） | **启动器界面主字体**（EUI 的默认值就是这个字体，`assets/` 移除后由 `dslAppConfig()` 显式指定） |
| `Font Awesome 7 Free-Solid-900.otf` | Font Awesome 7 Free | © Fonticons, Inc. | **图标字体**（`ui.text().icon(码位)`）。字体文件为 OFL 1.1；其图标本身另有 CC BY 4.0 |
| `NotoSansSC-Medium.ttf`<br>`NotoSansSC-SemiBold.ttf`<br>`NotoSansSC-Bold.ttf` | Noto Sans SC | © The Noto Project Authors | 生成汉化包的默认界面字体、正文兜底 |
| `FiraCode-Medium.ttf` | Fira Code | © The Fira Code Project Authors | 汉化包等宽槽位 |
| `JetBrainsMono-Medium.ttf` | JetBrains Mono | © The JetBrains Mono Project Authors | 版本号等纯 ASCII 文本（`wbMonoFont()`） |

## 分发时的义务

OFL §2 要求每个副本带上版权声明与本许可全文。`tools/deploy.py` 会把本文件与
`OFL-1.1.txt` 一并复制到发行包的 `licenses/` 下。

## 注意

`JingNanJunJunTi-…` 与 `Font Awesome 7 Free-Solid-900.otf` 原属 EUI-NEO 的 `assets/`
目录（框架 demo 资源）。该目录已整份移除，这两个字体改由启动器自带并在
`dslAppConfig()` 里显式指定 —— 不指定的话 EUI 会静默退回系统字体，界面字形与图标
全变样且**不报错**。详见 `EUI-NEO-main/MODIFICATIONS.md`。
