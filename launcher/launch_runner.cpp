#include "launch_runner.h"

#include "launcher.h"
#include "launch_toast.h"
#include "log.h"
#include "pxc_reuse.h"
#include "util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>

namespace pxc_runner {
namespace {

namespace fs = std::filesystem;

// 用户最终没用上这份副本（取消 / 选择不开新进程 / 目标实例还在冷启动）：删掉。
// 与 `pxc_project::applyCancel` 同一口径 —— 删失败就如实写日志，绝不重试、绝不扩大范围。
void dropUnusedCopy(const fs::path& copy) {
    if (copy.empty()) return;
    std::error_code ec;
    if (fs::remove(pxc_util::toExtended(copy), ec)) {
        pxc_log::write("INFO", "Stage", "副本没用上，已删除: " + copy.u8string());
    } else if (ec) {
        pxc_log::write("WARN", "Stage",
                       "副本没用上，但删不掉（多半被别的程序占用）: " + copy.u8string() + " | " +
                           ec.message());
    }
}

// 等主窗的上限。25 秒起给黄警、60 秒判失败（docs/05 §9.5，依据 1.18.5 实测 18.2–18.7 s，
// 且真实发生过 43 s 的一次 —— 所以超时只能说"可能卡住"，不能说"启动失败"）。
constexpr int kWaitWindowMs = pxc_stage::kFailAfterSec * 1000;
// 「复用失败 → 要不要开新进程」这个问句等多久。等不到就当用户走了。
constexpr int kAskTimeoutMs = 5 * 60 * 1000;

struct State {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<Job> queue;
    bool quit = false;
    std::thread worker;

    std::mutex resultMtx;
    // 结果**排队**而不是单格：连开两个工程时两次结果可能先后脚到，单格会被后一个盖掉，
    // 于是"记启动历史 / 弹自动生成结论"就少做一次（用户看到的是"第二个没进历史"）。
    std::deque<Result> results;

    std::atomic<bool> cancel{false};
    std::atomic<bool> busy{false};
    std::atomic<int> stage{(int)pxc_stage::Stage::Resolve};
    // 排队数（不含正在跑的那个）。提示窗每条内容都要带上它。
    std::atomic<int> queueLen{0};

    // 最近一次推给提示窗的**原始**内容。排队数变了要原地重画一次，
    // 否则用户点了第二个工程，要等到下一个阶段 tick（最长 1 秒）才看到"还有 1 个"。
    std::mutex textMtx;
    pxc_stage::Text lastBase;
    bool hasLast = false;

    bool toastEnabled = true;
    void* mainHwnd = nullptr;
    unsigned int doneMsg = 0;
};

// 故意 new 且不释放：worker 可能活到静态析构之后（与 pxc_reuse 的 async 状态同理）。
State& S() {
    static State* s = new State();
    return *s;
}

// 排队数并进次行。为什么要写在**正在跑的那个**任务上，而不是给排队任务单独弹一个窗：
// 提示窗同一时刻只允许一个（launch_toast.h 的第 3 条硬性约束），弹出的第二个会把进度盖掉 ——
// 用户看到的是"启动 A 的进度没了，界面在说 B"，比没有提示更慌。
// 次行本来就有两行余量（preferredHeightPx 里的 textPx(44)），加这一截不用改布局。
pxc_stage::Text withQueue(const pxc_stage::Text& base) {
    const int n = S().queueLen.load();
    if (n <= 0) return base;
    pxc_stage::Text t = base;
    const std::string note = "还有 " + std::to_string(n) + " 个在排队";
    t.line2 = t.line2.empty() ? note : t.line2 + "\u3000·\u3000" + note;
    return t;
}

// 所有推给提示窗的路径都走这里，保证"排队数"永远跟着内容一起更新。
void pushText(const pxc_stage::Text& base) {
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.textMtx);
        s.lastBase = base;
        s.hasLast = true;
    }
    if (s.toastEnabled) pxc_toast::show(withQueue(base));
}

// 排队数变了就原地重画（内容里除了这一截都没变，所以看起来只是"多了一句"）。
void refreshToast() {
    State& s = S();
    pxc_stage::Text base;
    {
        std::lock_guard<std::mutex> lk(s.textMtx);
        if (!s.hasLast) return;
        base = s.lastBase;
    }
    if (s.toastEnabled) pxc_toast::show(withQueue(base));
}

void logStage(const Job& job, pxc_stage::Stage st, int elapsedSec) {
    // 每个阶段一行，带秒数 —— 验收判据④要求"能算出各阶段耗时"。
    pxc_log::write("INFO", "Stage",
                   std::string(pxc_stage::stageName(st)) + " | " + job.fileName +
                       " | " + job.version + (elapsedSec > 0 ? " | " + std::to_string(elapsedSec) + "s" : ""));
}

/// 消费一次用户点击，判断"要不要停"。返回 true = 停。
///
/// 为什么是**轮询**而不是回调：提示窗有自己的线程，回调会把线程归属搞乱
/// （docs/05 §9.4）。轮询点就是所有"还没把决定交给 PXC"的边界 —— 复用链路的每一步、
/// 建副本前后、拉起之前。拉起之后不再调用它，所以「已经拉起进程就不可取消」不是靠
/// 按钮置灰一个人守着的，worker 这边也真的不问。
///
/// 只认 `Cancel`：`NewProcess` 是询问窗的答案，由 `askNewProcessAndWait` 自己读，这里
/// 若一并取走会让询问窗永远等不到答案（它俩不会同时存在，但别留下这种耦合）。
bool pollCancel(State& s) {
    if (s.cancel.load()) return true;   // 关主窗 / shutdown
    if (pxc_toast::takeAction() != pxc_stage::Action::Cancel) return false;
    s.cancel = true;
    pxc_log::write("INFO", "Stage", "用户取消 | 阶段=" +
                                        std::string(pxc_stage::stageName((pxc_stage::Stage)s.stage.load())));
    return true;
}


void showStage(const Job& job, pxc_stage::Stage st, int elapsedSec = 0,
               const std::string& detail = {}) {
    State& s = S();
    s.stage = (int)st;
    logStage(job, st, elapsedSec);
    pushText(pxc_stage::makeText(st, job.fileName, job.version, elapsedSec, detail));
}

// 提示窗上一行放得下的短原因。长版本（含"避免 Ctrl+O 打到别的程序"这类解释）只进日志 ——
// 提示窗折行后会把"吗?"单独甩到第二行，很丑，而且用户要的是"要不要再来一次"。
const char* shortReason(pxc_reuse::Result r) {
    switch (r) {
    case pxc_reuse::Result::ForegroundFailed: return "抢不到前台";
    case pxc_reuse::Result::NoDialog: return "没等到打开对话框";
    // 卡着和"没等到对话框"必须分开说：前者是我们**故意没发键**（发了会排队、恢复后连弹几个窗），
    // 后者是发了没反应。用户据这句话判断该等一等还是另开一个。
    case pxc_reuse::Result::NotResponding: return "PXC 一直没响应（卡在建工程）";
    // 屏幕上开着别的对话框时，按键进不去、也不能把主窗提到它前面（那样 PXC 就点不动了），
    // 只能等用户先把它处理掉。这句话要说得像"原因"而不是"我们失败了"。
    case pxc_reuse::Result::DialogOpen: return "PXC 开着别的对话框，先关掉它再试";
    case pxc_reuse::Result::VerifyFailed: return "点了打开但没生效";
    case pxc_reuse::Result::CallFailed: return "无法发送按键";
    case pxc_reuse::Result::Undetermined: return "有 PXC 在跑但读不到它的位置";
    // Busy 单独给一句：它意味着"目标 PXC 还没启动完"，而这个时刻用户最需要的正是"再试一次"，
    // 不是在"开新进程"和"取消"之间二选一（那两种都不是他的诉求）。
    case pxc_reuse::Result::Busy: return "PXC 还在启动中，窗口还没出来";
    default: return "没能在已开的 PXC 里打开";
    }
}

void showAskNewProcess(const Job& job, const std::string& reason, bool allowNewProcess) {
    State& s = S();
    s.stage = (int)pxc_stage::Stage::AskNewProcess;
    pxc_log::write("WARN", "Stage", "reuse_failed | " + job.fileName + " | " + reason);
    pushText(pxc_stage::makeAskNewProcess(job.fileName, job.version, reason, allowNewProcess));
}

void finish(State& s, Kind kind, const std::string& message, const std::string& path,
            unsigned long pid = 0) {
    {
        std::lock_guard<std::mutex> lk(s.resultMtx);
        Result r;
        r.kind = kind;
        r.message = message;
        r.path = path;
        r.pid = pid;
        s.results.push_back(r);
    }
    s.busy = false;
    if (s.mainHwnd) PostMessageW((HWND)s.mainHwnd, s.doneMsg, 0, 0);
}

// 「复用失败，怎么办？」—— 停下来等用户点。这是用户 2026-11-01 明确要求的：
// 不再像以前那样静默退化成新进程（多开一个 PXC 会抢同一个工程的 autosave）。
//
// 2026-11-04 加了「重试」：复用失败最常见的原因是**目标 PXC 还没启动完 / 正卡在建工程**，
// 那时对话框要等十几秒才响应，一次失败并不能说明"复用不了"。用户要求给一个按钮自己再试，
// 而不是只能在"开新进程"（有害）和"取消"（白点）之间选。
enum class AskAnswer { Cancel, NewProcess, Retry };

AskAnswer askNewProcessAndWait(State& s, const Job& job, const std::string& reason,
                               bool allowNewProcess) {
    showAskNewProcess(job, reason, allowNewProcess);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kAskTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (s.cancel) return AskAnswer::Cancel;
        switch (pxc_toast::takeAction()) {
        case pxc_stage::Action::NewProcess:
            pxc_log::write("INFO", "Stage", "用户选择开新进程 | " + job.fileName);
            return AskAnswer::NewProcess;
        case pxc_stage::Action::Retry:
            pxc_log::write("INFO", "Stage", "用户选择重试 | " + job.fileName);
            return AskAnswer::Retry;
        case pxc_stage::Action::Cancel:
        case pxc_stage::Action::Close:
            return AskAnswer::Cancel;
        default:
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    // 超时：提示窗还挂着，别让它永远留在屏幕上。
    pxc_toast::hide();
    return AskAnswer::Cancel;
}

// 重试之前先把"目标实例的窗口"等到。
//
// 用户 2026-11-04 的复现：首启之后马上再开第二个同版本文件 —— 第二个文件来的时候第一个 PXC
// 的进程已经在了，但主窗要 ≈18 秒才出来（最慢一次 43 秒）。这段时间里复用必然失败
// （`Busy`，或者按键被卡顿吞掉），所以"点了重试立刻再失败一次"是完全可能的。
// 这里就是那时候的等待：等到了再重试，重试才有意义。
//
// @return false = 用户取消了（调用方直接收场，不要再拉起进程）。
bool waitInstanceReady(State& s, const Job& job) {
    // 30 秒，不是首启那 60 秒：走到这里说明这个进程**已经起来了**（用户点过一次失败、
    // 又点了重试），它比"刚 CreateProcess 出来"的时刻晚了十几二十秒。再等满一分钟只会让
    // 用户盯着一个不动的秒数 —— 到点就照常再试一次，让复用链自己给结论（多半又是 Busy），
    // 那时询问窗会再出现，决定权回到用户手里。
    constexpr int kReadyWaitMs = 30 * 1000;
    const auto t0 = std::chrono::steady_clock::now();
    bool wasCanceled = false;
    const bool ready = pxc_reuse::waitInstanceWindow(
        job.exePath, job.altExes, kReadyWaitMs,
        [&](int sec) { showStage(job, pxc_stage::Stage::ReuseWaitWindow, sec); },
        [&] {
            wasCanceled = pollCancel(s);
            return wasCanceled;
        });
    const int waitedSec = (int)std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
    if (wasCanceled) {
        pxc_log::write("INFO", "Reuse", "retry_wait_canceled | 等了 " + std::to_string(waitedSec) + "s");
        return false;
    }
    // 超时不是死局：照样往下再试一次，让真正的复用链去给出结论（它可能这次就成了）。
    pxc_log::write(ready ? "INFO" : "WARN", "Reuse",
                   std::string(ready ? "retry_wait_ok" : "retry_wait_timeout") + " | 等了 " +
                       std::to_string(waitedSec) + "s | file=" + job.fileName);
    return true;
}

// 新进程那条路：拉起 → 等主窗出现。
// `prebuiltCopy` 非空 = 副本已经建好（Copy 模式），startProject 直接用它，不再复制。
Kind spawnAndWait(State& s, const Job& job, const fs::path& prebuiltCopy, unsigned long& outPid,
                  std::string& message) {
    // 拉起之前的最后一道闸：用户在 ReuseXxx（可取消阶段）点的取消，若刚好落在
    // "复用已收场、正要 spawn"这个缝里，绝不能"点了取消还是把进程拉起来"。
    if (pollCancel(s)) {
        message = "已取消（未拉起 PXC）";
        return Kind::Canceled;
    }

    // 阶段由 spawn 自己按真实顺序报（建副本 → 备份 → 拉起），不在这里抢跑：
    // 先报 `Spawn` 会让取消按钮在"其实还能取消"的建副本阶段就置灰。
    if (!job.spawn) {
        message = "内部错误：没有提供启动方式";
        return Kind::Failed;
    }
    // 建副本 / 备份还在这里面：把它们报给提示窗，用户才知道"现在点取消还来得及"。
    SpawnHooks hooks;
    hooks.stage = [&](pxc_stage::Stage st) { showStage(job, st); };
    hooks.canceled = [&] { return pollCancel(s); };

    const SpawnOutcome sp = job.spawn(hooks, prebuiltCopy);
    if (sp.needsPicker) {
        // 本机没登记这个版本：worker 不做这个决策，交回主线程弹协商窗。
        message = "本机没有这个版本";
        return Kind::NeedsPicker;
    }
    if (sp.canceled) {
        // 走到这里说明取消发生在拉起之前（副本已由 startProject 自己清掉），
        // 与"等窗口途中被取消"不是一回事，但终态一样。
        message = sp.detail.empty() ? "已取消（未拉起 PXC）" : sp.detail;
        return Kind::Canceled;
    }
    if (!sp.ok) {
        message = sp.detail.empty() ? "无法启动 PXC" : sp.detail;
        return Kind::Failed;
    }
    outPid = sp.pid;

    // 等窗口：每 250 ms 探一次，秒数一变就刷提示窗。
    // 这里的取消谓词**只认 s.cancel**（关主窗 / shutdown），不读提示窗的点击：
    // 进程已经拉起，取消按钮在同一时刻被置灰，点了也不会进队列。多读一次等于给自己
    // 留一条"取消按钮失效之后还能停"的后门，而那条路只能靠杀进程收场。
    int lastSec = -1;
    bool wasCancelled = false;
    unsigned long exitCode = STILL_ACTIVE;
    const bool got = pxc_launch::waitMainWindow(
        sp.pid, kWaitWindowMs,
        [&](int sec) {
            if (sec == lastSec) return;
            lastSec = sec;
            showStage(job, pxc_stage::waitWindowStage(sec), sec);
        },
        [&] { return s.cancel.load(); }, &wasCancelled, &exitCode);

    if (got) return Kind::OkNew;

    if (wasCancelled) {
        // 只可能是关主窗：进程已经拉起，它自己会继续启动，我们不去杀它（§9.4）。
        message = "已取消（PXC 进程已拉起，它自己会继续启动）";
        return Kind::Canceled;
    }
    if (exitCode != STILL_ACTIVE) {
        message = "PXC 启动后立即退出了（退出码 " + std::to_string(exitCode) + "）";
        return Kind::Failed;
    }
    message = "60 秒仍未出现窗口，可能卡住了（可查看日志）";
    return Kind::Failed;
}

void runJob(State& s, const Job& job) {
    s.cancel = false;
    s.busy = true;
    // 丢掉上一个任务留下的按钮点击：这一轮还没开始，队列里任何点击都属于上一个任务
    // （它的取消，或者询问窗的答案）。不清掉会被本轮第一个阶段当成"刚点的"。
    while (pxc_toast::takeAction() != pxc_stage::Action::None) {
    }
    pxc_log::write("INFO", "Stage", "==== 开始 " + job.fileName + " | " + job.exePath.u8string());

    unsigned long pid = 0;
    std::string message;
    // 结果里的 path 永远是**原工程**：历史记录要记用户点开的那一个，不是临时副本。
    const std::string pathUtf8 = job.pxcPath.u8string();
    // 真正交给 PXC 的文件。Copy 模式 = 副本（先建、再复用）。
    fs::path openPath = job.pxcPath;
    bool copyBuilt = false;

    // 空启动（版本库卡片）= 没有工程可交给别人 → 复用那条链整段跳过。
    if (job.allowReuse && !job.pxcPath.empty()) {
        // 每一轮 = 一次复用尝试。用户在询问窗点「重试」就再来一轮（2026-11-04 加）：
        // 第一次失败常常只是目标 PXC 还没缓过来，让用户自己决定"再来一次"比我们猜要准。
        for (;;) {
            // Copy 模式：**先建副本，再判断复用**。
            //
            // 顺序反了会怎样（用户 2026-11-04 实测）：复用链拿到的只能是原文件，而原文件的
            // 版本与目标 exe 对不上 —— 把一个旧版本工程塞进同版本实例里就是要毁它，于是
            // 只能退化成"再开一个进程"。先建副本，复用打开的就是副本，跨版本也能复用。
            //
            // 重试时**不重建**：copyBuilt 为真说明副本已经存在，重建会多留一份孤儿文件。
            if (job.buildCopy && !copyBuilt) {
                SpawnHooks bh;
                bh.stage = [&](pxc_stage::Stage st) { showStage(job, st); };
                bh.canceled = [&] { return pollCancel(s); };
                std::string detail;
                bool wasCanceled = false;
                if (!job.buildCopy(bh, openPath, detail, wasCanceled)) {
                    pushText(pxc_stage::makeText(
                        wasCanceled ? pxc_stage::Stage::Canceled : pxc_stage::Stage::Failed,
                        job.fileName, job.version, 0, detail));
                    finish(s, wasCanceled ? Kind::Canceled : Kind::Failed,
                           detail.empty() ? "创建副本失败" : detail, pathUtf8);
                    return;
                }
                copyBuilt = true;
            }

            showStage(job, pxc_stage::Stage::ReuseFind);
            const auto o = pxc_reuse::openInExisting(
                job.exePath, openPath, job.recentFile,
                [&](pxc_stage::Stage st) { showStage(job, st); },
                // 复用链路每一步之间都会问一次 —— 这就是「取消」真正生效的地方
                // （3–11 秒的窗口，用户唯一来得及点的时候）。
                [&] { return pollCancel(s); },
                job.altExes,
                // 等 PXC 缓过来的秒数：卡在建工程时可能要十几秒，用户得看见它在等什么。
                [&](int sec) { showStage(job, pxc_stage::Stage::ReuseWaitIdle, sec); });

            // 复用链的**结果**必须落日志。以前只有阶段行（`reuse_find` 之后直接 `spawn`），
            // 于是"没有实例"和"有实例但没连上"在日志里长得一模一样，第三方反馈多开时
            // 根本没法判断卡在哪一环 —— 这一行是下次排查的第一手证据。
            pxc_log::write(o.result == pxc_reuse::Result::Ok ? "INFO" : "WARN", "Reuse",
                           std::string("reuse=") + pxc_reuse::resultName(o.result) + " | " +
                               o.detail + " | pid=" + std::to_string(o.pid) +
                               " | exe=" + (o.usedExe.empty() ? job.exePath : o.usedExe).u8string() +
                               " | file=" + openPath.u8string());

            if (o.result == pxc_reuse::Result::Ok) {
                // 副本已经交给 PXC，保留。
                pushText(pxc_stage::makeText(pxc_stage::Stage::OkReuse, job.fileName, job.version));
                finish(s, Kind::OkReuse, "已在打开的 PXC 里打开（新增了一个文件标签）", pathUtf8,
                       o.pid);
                return;
            }
            if (o.result == pxc_reuse::Result::Canceled) {
                dropUnusedCopy(copyBuilt ? openPath : fs::path());
                pushText(pxc_stage::makeText(pxc_stage::Stage::Canceled, job.fileName, job.version));
                finish(s, Kind::Canceled, "已取消（未交给 PXC）", pathUtf8);
                return;
            }
            if (o.result == pxc_reuse::Result::NoInstance) {
                // 只有**确认**"同版本的每一份都没有进程在跑"才走到这里 —— 那是首次启动，
                // 本来就该开新进程，问一句"要开新进程吗"纯属添乱。
                // 判不准的情况一律不问这条路：读不到进程路径 → `Undetermined`（走询问），
                // 有进程但没连上 → 其余 Result（同样询问）。
                break;
            }

            // 剩下两种都停下来问用户，区别只在给不给「开启新进程」：
            //   · Busy（进程在、主窗还没出来）：这时开新进程正是要避免的事（两个 PXC 抢 autosave），
            //     所以只给「重试 / 取消」；
            //   · 其余（有实例但没连上）：三个都给，让用户自己判断。
            const bool busy = (o.result == pxc_reuse::Result::Busy);
            const AskAnswer ans = askNewProcessAndWait(s, job, shortReason(o.result), !busy);
            if (ans == AskAnswer::Retry) {
                if (!waitInstanceReady(s, job)) {
                    // 等待期间被取消：副本还没交给任何人，删掉。
                    dropUnusedCopy(copyBuilt ? openPath : fs::path());
                    pushText(pxc_stage::makeText(pxc_stage::Stage::Canceled, job.fileName,
                                                 job.version));
                    finish(s, Kind::Canceled, "已取消（未交给 PXC）", pathUtf8);
                    return;
                }
                continue;
            }
            if (ans == AskAnswer::NewProcess) break;

            dropUnusedCopy(copyBuilt ? openPath : fs::path());
            pushText(pxc_stage::makeText(pxc_stage::Stage::Canceled, job.fileName, job.version));
            finish(s, Kind::Canceled, busy ? "已取消（PXC 还在启动，没有再开进程）"
                                           : "已取消（未开新进程）",
                   pathUtf8);
            return;
        }
    }

    const Kind k = spawnAndWait(s, job, copyBuilt ? openPath : fs::path(), pid, message);
    if (k == Kind::NeedsPicker) {
        // 提示窗让位给协商窗：两个浮层叠在右下角，用户只会看见上面那个。
        // 副本也没人用了（协商完会重新走一遍，重新建一份）。
        dropUnusedCopy(copyBuilt ? openPath : fs::path());
        if (s.toastEnabled) pxc_toast::hide();
        finish(s, k, message, pathUtf8);
        return;
    }
    if (k == Kind::OkNew) {
        pushText(pxc_stage::makeText(pxc_stage::Stage::Ok, job.fileName, job.version));
    } else {
        pushText(pxc_stage::makeText(
            k == Kind::Canceled ? pxc_stage::Stage::Canceled : pxc_stage::Stage::Failed,
            job.fileName, job.version, 0, message));
    }
    if (k == Kind::OkNew) message = "已用 " + job.version + " 启动：" + job.fileName;
    finish(s, k, message, pathUtf8, pid);
}

void workerMain() {
    State& s = S();
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(s.mtx);
            s.cv.wait(lk, [&] { return s.quit || !s.queue.empty(); });
            if (s.quit) return;
            job = std::move(s.queue.front());
            s.queue.pop_front();
            // 剩下的才是"在排队"的；本任务已经出队，不再算进排队数。
            s.queueLen.store((int)s.queue.size());
        }
        if (s.queueLen.load() > 0)
            pxc_log::write("INFO", "Stage",
                           "开始下一个 | " + job.fileName + " | 后面还剩 " +
                               std::to_string(s.queueLen.load()) + " 个");
        runJob(s, job);
    }
}

} // namespace

bool init(const std::wstring& fontDir, void* mainHwnd, unsigned int appDoneMsg, bool toastEnabled) {
    State& s = S();
    s.mainHwnd = mainHwnd;
    s.doneMsg = appDoneMsg;
    s.toastEnabled = toastEnabled;
    if (s.worker.joinable()) return true;

    if (toastEnabled) pxc_toast::start(fontDir);
    s.worker = std::thread(workerMain);
    return true;
}

void setToastTextScale(float scale) { pxc_toast::setTextScale(scale); }

void setToastAccent(unsigned int rgb) { pxc_toast::setAccent(rgb); }

bool submit(const Job& job) {
    State& s = S();
    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.queue.push_back(job);
        // 队列里只有**等着的**（正在跑的那个早在出队时被取走了）。
        // 空闲时新提交的这一个马上就会被取走，不算排队。
        const bool busy = s.busy.load();
        queued = busy || s.queue.size() > 1;
        s.queueLen.store(busy ? (int)s.queue.size() : (int)s.queue.size() - 1);
    }
    if (queued) {
        pxc_log::write("INFO", "Stage", "排队 | " + job.fileName + " | 前面还有 " +
                                          std::to_string(s.queueLen.load()) + " 个");
        // 立刻把这句并进当前任务那一行：不然用户要等下一个阶段 tick（最长 1 秒）才有回应。
        refreshToast();
    }
    s.cv.notify_one();
    return queued;
}

bool busy() { return S().busy.load(); }

int queued() { return S().queueLen.load(); }

pxc_stage::Stage stage() { return (pxc_stage::Stage)S().stage.load(); }

void cancel() {
    State& s = S();
    s.cancel = true;
    s.cv.notify_all();
}

bool takeResult(Result& out) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.resultMtx);
    if (s.results.empty()) return false;
    out = s.results.front();
    s.results.pop_front();
    return true;
}

void shutdown() {
    State& s = S();
    if (!s.worker.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.quit = true;
        // 关窗时排队的任务直接丢掉：不然 join 要等它们一个个跑完（每个都能等到 60 秒超时）。
        s.queue.clear();
        s.queueLen.store(0);
    }
    // 让正在跑的那个在下一个取消检查点收手。检查点密度（docs/05 §9.14.4）：
    // 等主窗 250 ms、复用链路一步 80–200 ms、询问窗 120 ms；真正不可打断的只有
    // startProject 里那一次同步 copy（通常 <500 ms）。
    cancel();
    s.worker.join();
    pxc_toast::stop();
}

} // namespace pxc_runner
