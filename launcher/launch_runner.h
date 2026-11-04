#pragma once
// 启动任务的执行者（worker 线程）。
//
// 为什么必须独立线程（docs/05 §9.1）：
//   · 主窗最小化后 `compose()` 一次都不跑 —— 挂在帧上的状态机会静默死掉，
//     进程变成"隐藏僵尸还攥着单实例互斥体"（2026-10-31 踩过）；
//   · 复用链路本身要同步阻塞 3–11 秒，不可能塞进窗口过程；
//   · 第二实例的转发是 3 秒同步消息，主线程一忙就丢单。
// 所以：窗口过程只入队（<50 ms），活全在这里干，结果通过 PostMessage 回投主线程。
//
// 边界纪律：worker **不碰任何 UI 状态**（`g_wbStore` / `g_wb` / 任何 `Signal`）。
// 它只写 `Result` 结构体，由主线程取走后再落库。
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "launch_stage.h"

namespace pxc_runner {

/// spawn 执行期间能向 worker 问的两件事。**只有还没拉起进程的阶段**会用到它们
/// （建副本 / 备份）—— 拉起之后 worker 不再回答"是否取消"，因为那时取消就是杀进程。
struct SpawnHooks {
    /// 上报一个阶段（`Copy` / `Backup`）。上报后提示窗上的「取消」才真正可点。
    std::function<void(pxc_stage::Stage)> stage;
    /// 是否已经被取消（用户在可取消阶段点了取消）。可能在 spawn 内部被调用多次。
    std::function<bool()> canceled;
};

/// 真正执行"拉起"的结果。由 app 层填，worker 只消费。
struct SpawnOutcome {
    bool ok = false;
    unsigned long pid = 0;
    bool needsPicker = false;   // 本机没登记这个版本 → 交回主线程弹协商窗
    /// 在拉起之前被取消。**副本已由 startProject 自己处理**（本次刚建的删掉），
    /// 这里只负责让 worker 把终态写成"已取消"而不是"失败"。
    bool canceled = false;
    std::string detail;         // 失败原因 / 取消说明（一行中文）
};

struct Job {
    /// 真正交给 PXC 的文件（副本模式 = 副本路径）。**留空 = 空启动**（版本库卡片双击那种
    /// "只要一个 PXC 窗口"）：没有工程可交给别人，所以跳过复用，直接走首启。
    std::filesystem::path pxcPath;
    std::filesystem::path exePath;
    /// 同版本里**其它**已登记的 exe（Steam 与便携各装一份时会有多条）。
    /// 复用只认"同一个 exe 路径"，而用户正在用的未必是偏好选中的那份 ——
    /// 只试 `exePath` 会把它判成"没有实例"，然后静默再开一个同版本进程。
    /// 快照在主线程取（worker 不读 `g_cfg`），这里是纯值拷贝。
    std::vector<std::filesystem::path> altExes;
    std::filesystem::path recentFile; // 复用校验用（PXC 的 recent.json）
    std::string version;              // 显示用，可空
    std::string fileName;             // 显示用（只要文件名）
    bool allowReuse = true;           // 关掉"复用已有实例"时就直连首启
    /// Copy 模式：复用之前先把副本建出来。**副本才是真正交给 PXC 的文件** ——
    /// 跨版本时如果先判断复用、再建副本，复用链只能拿到原文件（版本对不上，不能塞给
    /// 同版本实例），于是必然退化成"再开一个进程"。用户 2026-11-04 实测到的就是这个。
    ///
    /// 返回 false 表示建不出来；`canceled` 为真表示用户在建的过程中点了取消。
    /// 建出来的副本由 **worker 负责收尾**：复用成功 / 真开了新进程 → 保留；
    /// 复用失败且用户选择不开 → 删掉（用户从没用过它）。
    std::function<bool(const SpawnHooks&, std::filesystem::path& outCopy, std::string& detail,
                       bool& canceled)>
        buildCopy;
    /// 执行拉起的动作（app 层提供，内部走 pxc_project::startProject —— 那是唯一合法的启动
    /// 入口，跨版本副本与备份都在里面，不能绕过）。在 worker 线程调用，**不得碰 UI 状态**。
    /// @param hooks         建副本 / 备份期间向 worker 上报阶段、查询是否已取消。
    /// @param prebuiltCopy  非空 = 副本已经建好（`buildCopy` 的产物），不要再复制一份。
    std::function<SpawnOutcome(const SpawnHooks&, const std::filesystem::path& prebuiltCopy)> spawn;
};

enum class Kind {
    OkNew,        // 新进程拉起来了，主窗已出现
    OkReuse,      // 在原进程里开了新标签
    Failed,       // 起不来 / 超时 / 进程秒退
    Canceled,     // 用户点了取消
    NeedsPicker,  // 本机没登记该版本，需主线程弹协商窗
};

struct Result {
    Kind kind = Kind::Failed;
    std::string message;   // 一行中文，可直接进状态栏与日志
    /// 这个结果属于哪个工程（utf8，空 = 空启动）。**必须随结果一起回来**：
    /// 连开两个工程时，主线程靠它记启动历史 —— 用"最近一次提交的路径"会张冠李戴。
    std::string path;
    unsigned long pid = 0;
    bool needsPicker = false;   // 调用方需要弹版本协商（worker 不做这个决策）
};

/// 初始化：起 worker 线程 + 提示窗线程。
/// @param fontDir    内置字体目录（`<exe>/data/fonts`），给提示窗用
/// @param mainHwnd   主窗 HWND（结果就绪时 PostMessage 给它），可为空
/// @param appDoneMsg 自定义消息号（主窗窗口过程里识别）
/// @param toastEnabled false 时完全不显示提示窗（调试 / 录屏）
bool init(const std::wstring& fontDir, void* mainHwnd, unsigned int appDoneMsg, bool toastEnabled);

/// 提交一个任务，立即返回。已有任务在跑时排队（FIFO）。
///
/// 排队是**正常路径**，不是异常：连开两个同版本工程时，第二个要等第一个把 PXC 窗口等出来，
/// 之后才能复用同一个实例（否则两个 PXC 抢同一份 autosave）。排队期间右下角提示窗会在
/// 当前任务那一行后面追加"· 还有 N 个在排队"，让用户知道第二个已被收下。
/// @return true = 这次提交进了队列（前面还有任务在跑），调用方据此写状态文字。
bool submit(const Job& job);

/// 提示窗的字号倍率（设置页「文本大小」）。app 层调这一个口子即可 ——
/// 提示窗是原生 GDI，不吃 EUI 的 fontSize 系数，倍率必须显式转交（内部转给 pxc_toast）。
/// 可在 `init()` 之前调用（值先存着），也可运行期随时改。
void setToastTextScale(float scale);

/// 提示窗的描边色（主题色，`COLORREF` 的 0x00BBGGRR）。同理：原生窗拿不到 `uiTheme()`，
/// 主题色由 app 显式转交。不调用则用启动器默认主题色。
void setToastAccent(unsigned int rgb);

bool busy();

/// 正在排队的任务数（不含正在跑的那个）。只用于状态文字，不参与调度。
int queued();

pxc_stage::Stage stage();

/// 请 worker 停下：置标志，worker 在下一个取消检查点收手。
///
/// 注意分工：**提示窗上那个「取消」不走这里** —— 它只把一次点击放进队列，由 worker 自己
/// 在阶段边界 `pollCancel()` 取走（拉起的进程之后不再取，那时取消已无意义）。
/// 这个函数是给"整条链路都要停"的场合用的（关窗收尾），目前只有 `shutdown()` 调用。
void cancel();

/// 主线程轮询：取走已完成的结果。没有就返回 false。
bool takeResult(Result& out);

/// 停线程 + 关提示窗。进程退出前调用。
void shutdown();

} // namespace pxc_runner
