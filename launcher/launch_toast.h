#pragma once
// 右下角启动提示窗（原生 Win32，独立线程，独立消息泵）。
//
// 为什么不用 EUI 画：
//   · 主窗最小化后框架**不再调用 compose**（`glfw_app_main.cpp` 里遇 `GLFW_ICONIFIED` 直接 continue），
//     而启动过程恰恰发生在主窗最小化的时候；
//   · EUI 的二次窗口跟着主循环走，主窗最小化时它们一起停；
//   · `DslWindowConfig` 没有"无边框 / 置顶 / 指定位置 / 不激活"任何一个参数，改了也拿不到 `WS_EX_NOACTIVATE`。
// 结论：提示窗只能自己 CreateWindowEx + 自己的消息泵。
//
// 三条硬性约束（违反任何一条，复用链路当场失效）：
//   1. **绝不激活自己** —— `WS_EX_NOACTIVATE` + `SW_SHOWNOACTIVATE` + 不 `SetFocus`。
//      复用要靠 `Ctrl+O`，那一步要求 PXC 是前台窗口；提示窗一抢焦点就判 `ForegroundFailed`。
//   2. **绝不挡住打开对话框** —— 右下角 + 避开任务栏（`SPI_GETWORKAREA`）。
//   3. **同一时刻只允许一个** —— 连续双击时后来的覆盖前一个，不叠罗汉。
#include <string>

#include "launch_stage.h"

namespace pxc_toast {

/// 初始化（幂等）：起线程、建窗。`fontDir` 是内置字体目录（`<exe>/data/fonts`）。
/// 窗口暂无内容时不显示 —— 只有 `show()` 才会让它露脸。
bool start(const std::wstring& fontDir);

/// 关线程、销毁窗口。只能在没有别的事情依赖它时调用（进程退出前）。
void stop();

/// 更新内容并显示（可从 worker 线程调用；内部抢锁 + PostMessage，立即返回）。
/// 内容没变时不重绘。
void show(const pxc_stage::Text& text);

/// 隐藏（带 120 ms 淡出则留给 P2；当前直接隐藏）。
void hide();

/// 文本缩放倍率（设置页「文本大小」，0.85–1.50）。提示窗是原生 GDI，不在 EUI 的 text
/// pipeline 里，`fontSize()` 的系数对它无效 —— 倍率必须从这里显式传进来。
/// 任意线程可调，随时生效（下一次 `show()` 重建字体与窗口尺寸）；限幅 0.7–2.0。
void setTextScale(float scale);

/// 描边色（主题色，`COLORREF` 的 0x00BBGGRR 形式）。提示窗不在 EUI 的 UI 树里，
/// 主题色只能显式传进来；不传则用启动器默认主题色。任意线程可调，窗口在屏时立即重绘。
void setAccent(unsigned int rgb);

/// 直接摆出一份内容但不进消息循环（冒烟钩子 / 单测用）。
void setPendingForTest(const pxc_stage::Text& text);

/// 取走用户最近一次点击（取走即清空；没点过返回 `Action::None`）。
/// worker 每个阶段边界轮询一次即可，不需要回调 —— 回调会把线程归属搞乱。
pxc_stage::Action takeAction();

/// 提示窗当前是否可见（没启动线程时恒 false）。
bool visible();

} // namespace pxc_toast
