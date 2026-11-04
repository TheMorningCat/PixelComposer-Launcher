#pragma once
// 同版本实例复用：让"已经开着的 PXC"追加一个文件标签，而不是再开一个进程。
//
// 为什么只能这么做：PXC 是多文档 + 文件标签，但**官方没有实例转发** ——
// 命令行只认一个文件、多参数只留最后一个、全仓无"检测已有实例并转发"的代码。
// 外部入口只有两个：`Ctrl+O` 文件对话框与拖放（源码见 docs/03 §7.3.5）。
// 这里选 `Ctrl+O`：不依赖面板布局、不动鼠标、失败只是留个对话框（我们主动关掉）。
//
// 两条硬约束（实测）：
//   · `Ctrl+O` 那一发必须是真实按键（`SendInput`），`PostMessage(WM_KEYDOWN)` 不行 ——
//     GameMaker 靠 `GetKeyState` 读线程键盘状态，消息不会改那个状态；
//   · 因此发键前必须校验 `GetForegroundWindow()` 就是 PXC，抢不到前台就**不发键**，
//     否则 Ctrl+O 会打到别的程序上。
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "launch_stage.h"

namespace pxc_reuse {

enum class Result {
    Ok,               // 已在原进程里打开（recent.json 首项已变成目标文件）
    NoInstance,       // 确认没有同版本实例在跑 → 调用方走新进程（不必打扰用户）
    Undetermined,     // 有 PXC 进程但读不到 exe 路径（UIPI / 权限）→ 不能确认，必须问用户
    Busy,             // 进程在、但主窗还没出来（PXC 冷启动要 18 秒）→ 别开第二个
    ForegroundFailed, // 抢不到前台，未发按键
    NoDialog,         // 按键发了但 5 秒内没等到打开对话框
    NotResponding,    // 目标 PXC 卡着不处理消息（按键会排队，所以一个键都没发）
    DialogOpen,       // 屏幕上有别的模态框（不是「打开」窗）→ 不能按，也不能抢前台
    VerifyFailed,     // 填了路径也点了「打开」，但 recent.json 没变
    CallFailed,       // SendInput / SendMessage 失败
    Canceled,         // 调用方在某个阶段边界喊停（用户点了「取消」）
};

struct Outcome {
    Result result = Result::NoInstance;
    std::string detail;      // 一行中文说明（写日志 + 状态栏）
    unsigned long pid = 0;
    /// 实际复用的那个 exe。同版本装了多份时（Steam + 便携），正在跑的可能不是
    /// 偏好选中的那份 —— 调用方靠它写日志、并在"要开新进程"时知道该报哪一份。
    std::filesystem::path usedExe;
};

const char* resultName(Result r);

// 阶段回调：每进入一个子步骤前调一次（提示窗靠它滚动文案）。
using StageSink = std::function<void(pxc_stage::Stage)>;
// 秒数回调：只有"等"类的阶段用（等窗口 / 等 PXC 缓过来），提示窗靠它显示"已 N 秒"。
using TickSink = std::function<void(int elapsedSec)>;
// 取消判据：每个阶段边界问一次，返回 true 就停下（不发键、不点对话框）。
using CancelFn = std::function<bool()>;

// 同步执行（供命令行/测试）。recentFile 是 PXC 的 recent.json，用来判定"真的打开了"。
// onStage / cancelled 可为空 —— 不给就是原来的行为。
//
// altExes：同版本里**其它**已登记的 exe（Steam 与便携各装一份时会有多条）。
// 复用只认"同一个 exe 路径"，但用户正在用的未必是偏好选中的那份 —— 所以这里按
// "主 exe 优先、其余候选顺延"的顺序找进程，谁在跑就复用谁。全都不在跑才算 NoInstance。
Outcome openInExisting(const std::filesystem::path& exePath,
                       const std::filesystem::path& pxcPath,
                       const std::filesystem::path& recentFile,
                       const StageSink& onStage = {},
                       const CancelFn& cancelled = {},
                       const std::vector<std::filesystem::path>& altExes = {},
                       const TickSink& onTick = {});

// 等"同版本实例的主窗"出现 —— **只给重试用**。
//
// 为什么需要：首启之后立刻再开第二个文件时，目标 PXC 的进程已经在了，但主窗要 ≈18 秒才出来
// （实测 18.2–18.7 s，最慢一次 43 s）。这段时间里复用必然失败（`Busy` / 直接按键被卡顿吞掉），
// 所以"立刻重试"等于"立刻再失败一次"。重试前先在这里把窗口等到，重试才有意义。
//
// @param onTick    每变一秒回调一次（提示窗靠它显示"已 N 秒"），可为空
// @param cancelled 取消判据，可为空
// @return true  = 可以继续（窗口已出现，或**压根没有实例** —— 那就该走首启）
//         false = 超时（返回时窗口仍没出来）或被取消
bool waitInstanceWindow(const std::filesystem::path& exePath,
                        const std::vector<std::filesystem::path>& altExes, int timeoutMs,
                        const std::function<void(int elapsedSec)>& onTick = {},
                        const CancelFn& cancelled = {});

// 异步版本：UI 线程不能被这 5–11 秒的轮询冻住（列表还在滚、窗口还要重绘）。
// begin 返回 false 表示已有一次在跑；poll 在有结果时返回 true 并把结果取走。
bool beginAsync(const std::filesystem::path& exePath, const std::filesystem::path& pxcPath,
                const std::filesystem::path& recentFile);
bool poll(Outcome& out);
bool busy();

} // namespace pxc_reuse
