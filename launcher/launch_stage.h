#pragma once
// 启动阶段与提示文案（纯逻辑，无 Win32 / 无 EUI 依赖，可 headless 测）。
//
// 为什么要单独一个文件：文案是用户唯一能读到的东西，它必须能脱离界面单独核对。
// 这里只做一件事：给「阶段 + 文件 + 版本 + 已用秒数」出两行字，
// 至于这两行字画在原生窗还是将来的别处，不归它管。
//
// 文案句式是用户 2026-11-01 定的，不要自创：
//   首启  正在启动 <文件> / 版本：<ver>　PXC 启动中
//   复用  正在启动 <文件> / 版本：<ver>　PXC 已启动，正在 <操作>
#include <string>

namespace pxc_stage {

/// 启动阶段。顺序即典型执行顺序：首启 0–4，复用 5–10，终态 11+。
enum class Stage {
    Resolve = 0,      // 识别版本 + 定 exe（<50 ms，通常一闪而过）
    Copy,             // 跨版本协商 → 建副本
    Backup,           // 备份原文件
    Spawn,            // CreateProcess 已发出（此后不可取消）
    WaitWindow,       // 等 PXC 主窗出现（≈18 s，唯一在跑的证据是"已 N 秒"）

    ReuseFind,        // 找同版本实例的主窗
    ReuseWaitWindow,  // 重试前：等已开实例的主窗出来（PXC 冷启动 ≈18 s，此时重试必然再失败）
    ReuseRestore,     // 最小化 → 还原
    ReuseWaitIdle,    // 发按键前：等目标 PXC 不再卡着（卡着时按键会排队，见 §9.14.8）
    ReuseHotkey,      // 发 Ctrl+O
    ReuseRetry,       // 首次 Ctrl+O 没反应 → 重发一次（根因未明，见 docs/05 §9.14.5）
    ReuseDialog,      // 等文件对话框
    ReuseFill,        // 填路径 + 点「打开」
    ReuseVerify,      // 等对话框消失

    // 终态
    Ok = 20,          // 首启成功：PXC 窗口已出现
    OkReuse,          // 复用成功：已新增一个文件标签
    Warn,             // 慢，但还在跑（非终态）
    Failed,
    Canceled,
    AskNewProcess,    // 复用失败：停下来问用户要不要开新进程
};

/// 语气 → 决定图标与主行颜色。
enum class Tone { Busy, Warn, Ok, Fail, Muted };

/// 提示窗上可点的动作。`None` 表示只有关闭按钮。
enum class Action { None, Cancel, NewProcess, Retry, Close };

struct Text {
    std::string line1;   // 主行 16 px
    std::string line2;   // 次行 12.5 px
    Tone tone = Tone::Busy;
    bool cancelable = false;   // 取消按钮是否可点（false = 置灰，不是隐藏）
    Action primary = Action::Cancel;     // 主按钮（最右）—— 全流程恒定是「取消」，除 AskNewProcess
    Action secondary = Action::None;     // 次按钮（主按钮左边）
    Action tertiary = Action::None;      // 第三按钮（最左）—— 只有三选一的询问窗用
    const char* primaryLabel = "取消";
    const char* secondaryLabel = "";
    const char* tertiaryLabel = "";
    /// 自动关闭时间（毫秒）。0 = 不自动关，等用户点。成功态给 3000。
    int autoCloseMs = 0;
};

/// 取文件名（含扩展名）。路径分隔符 / 与 \ 都认，末尾分隔符会被忽略。
std::string fileNameOf(const std::string& path);

/// 阶段标识（写日志用，永远是 ASCII，不随界面语言变）。
const char* stageName(Stage s);

/// 该阶段是否属于"复用已有实例"那条链路（决定用哪套句式）。
bool isReuse(Stage s);

/// 该阶段能否取消。
///
/// 原则（docs/05 §9.4）：只有"还没把决定交给 PXC"的阶段能取消。
/// `Spawn` 之后取消不是"撤销"，而是"杀掉一个正在写文件的进程"，不做。
/// 首启在进程出现后不可取消 —— 这正是用户原话的要求。
///
/// 这个函数只管**按钮**长什么样；真正"停下来"靠 `pxc_runner::pollCancel()` 在阶段边界轮询，
/// 两边的阶段集合必须一致（docs/05 §9.14.3）。可取消阶段取消后，本次刚建的副本由
/// `pxc_project::startProject` 自己删掉。
bool cancelable(Stage s);

/// 生成两行字。
/// @param fileName  工程文件名（`fileNameOf()` 的结果，不是全路径）
/// @param version   版本字符串，空则次行不写"版本："前缀
/// @param elapsedSec 该阶段已用秒数（仅 WaitWindow / Warn 用于"已 N 秒"）
/// @param detail    附加说明（失败原因 / 慢的原因），可空
Text makeText(Stage s, const std::string& fileName, const std::string& version,
              int elapsedSec = 0, const std::string& detail = {});

/// 「复用失败 → 询问是否开新进程」的文案。
/// 退化原因由 `pxc_reuse::Result` 给出一行中文，这里只负责拼句式。
///
/// 三个按钮（左→右）：取消 / 开启新进程 / 重试。**重试是最右的主按钮** ——
/// 复用失败最常见的原因是目标 PXC 还在冷启动（窗口在主窗判据之外、或按键被卡顿吞掉），
/// 等它起来再试一次就能成；而"开启新进程"会真正多开一个 PXC 去抢同一份 autosave，
/// 所以它不该是默认动作（2026-11-04 用户要求加重试）。
///
/// @param allowNewProcess false 时不给「开启新进程」，只剩 重试 / 取消 —— 用于
///        `Busy`（进程在、主窗还没出来）：这种时刻开新进程正是要避免的事。
Text makeAskNewProcess(const std::string& fileName, const std::string& version,
                       const std::string& reason, bool allowNewProcess = true);

/// 「等主窗」的预警阈值（docs/05 §9.5 实测：1.18.5 冷启 18.2–18.7 s，曾 43 s）。
constexpr int kWarnAfterSec = 25;
constexpr int kFailAfterSec = 60;

/// 给等窗口阶段挑一个档位：正常 / 预警 / 失败。
Stage waitWindowStage(int elapsedSec);

} // namespace pxc_stage
