// 复用已有 PXC 进程：Ctrl+O → 文件对话框 → 校验 recent.json。
#include "pxc_reuse.h"
#include "launcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "io_util.h"
#include "log.h"
#include "store.h"
#include "util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

namespace fs = std::filesystem;

namespace pxc_reuse {

namespace {

constexpr DWORD kDialogWaitMs = 5000;   // 等 Ctrl+O 弹出对话框
constexpr DWORD kVerifyWaitMs = 6000;   // 等 recent.json 首项变成目标文件

// 第一次 Ctrl+O 偶尔"没反应"：实测再按一次就出来了（根因未明，见 docs/05 §9.14.5）。
// 曾经把病因判为"PXC 启动页握着键盘焦点"，**已证伪** —— 启动页开着也能触发 Ctrl+O。
//
// 做法：先按正常路径等 kDialogFirstWaitMs，没等到才重发一次完整 Ctrl+O。
// 不追求"一次都不多按"：正常情况 0.3 秒就弹，2.5 秒等不到本来就已经不正常了。
constexpr DWORD kDialogFirstWaitMs = 2500;

// 发按键之前，最多等目标 PXC"恢复处理消息"多久。
//
// PXC 在建工程 / 冷启动收尾时，主循环会卡住十几秒。这段时间里它不处理消息，
// 我们发的 Ctrl+O **不会丢**，而是积在它的消息队列里，等它缓过来再被当成一次按下 ——
// 按几次就弹几个「导入工程」窗口（2026-11-04 用户实测：复用明明成功了，随后连跳几个）。
// 所以卡着的时候一个键都不发，等它恢复。20 秒够覆盖实测最长的卡顿。
constexpr DWORD kIdleWaitMs = 20000;

// 复用结束后扫残留对话框的时长。
// 排队的那次按下只会在**前一个对话框关掉之后**才被处理，所以不能只扫一眼，
// 要给一个短窗口持续扫；代价是成功提示晚 1.2 秒出来，可接受。
constexpr DWORD kDrainMs = 1200;

// 已经发出去、但还没有"兑现"（见到对话框）的 Ctrl+O 次数。
//
// 为什么要有这个计数：按键可能积在 PXC 的队列里，而一次启动最多会有两次尝试、
// 每次数发两发。判"要不要扫残留"不能只看当前这次尝试 —— 上一次发的键可能还排着队。
// 兑现一次就减一次；离开复用链时还大于 0，说明可能有键晚到，必须扫一段。
std::atomic<int> g_pendingPress{0};

std::wstring lowerW(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

// 路径归一化：先解析成"真实路径"（弱 canonical），再统一分隔符、转小写。
//
// 为什么必须 canonical：同一个 exe 在磁盘上只有一个文件，但字符串可以有多种写法 ——
// 正斜杠 / 反斜杠、8.3 短名（`STEAML~1`）、符号链接与 junction（Steam 库常见）。
// 只做 absolute + 小写时这些写法互不相等，于是"进程明明在跑"被判成没有实例，
// 直接退化成再开一个 PXC —— 用户反馈的"再按一次变多开"就是这样来的。
//
// canonical 失败（路径不存在 / 权限不足）就退回 absolute：宁可少归一化，
// 也不能因为归一化失败把"有实例"判成"没实例"。
std::wstring normPath(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    std::error_code ec2;
    const fs::path real = fs::weakly_canonical(abs, ec2);
    std::wstring s = (ec2 ? abs : real).wstring();
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return lowerW(s);
}

void sleepMs(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---- 进程 / 窗口 ----
// 一次枚举拿全：pid + 归一化后的 exe 路径（读不到则空）。
//
// 为什么一次列全而不是"每个候选 exe 各扫一遍"：同版本可能有多份 exe，
// 每份都 CreateToolhelp32Snapshot 一遍既慢又容易在两次枚举之间错过进程变化。
struct ProcEntry {
    DWORD pid = 0;
    std::wstring path;      // 空 = 读不到（权限 / UIPI）
    bool looksPxc = false;  // 进程名像 PXC（读不到路径时的唯一线索）
};

std::vector<ProcEntry> listProcesses() {
    std::vector<ProcEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == GetCurrentProcessId()) continue;
            ProcEntry e;
            e.pid = pe.th32ProcessID;
            e.looksPxc = _wcsicmp(pe.szExeFile, L"PixelComposer.exe") == 0;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (h) {
                wchar_t buf[MAX_PATH * 4] = {0};
                DWORD size = (DWORD)(sizeof(buf) / sizeof(buf[0]));
                if (QueryFullProcessImageNameW(h, 0, buf, &size)) {
                    std::wstring p(buf, size);
                    for (auto& c : p)
                        if (c == L'/') c = L'\\';
                    e.path = lowerW(p);
                }
                CloseHandle(h);
            }
            out.push_back(std::move(e));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

// 把一个 exe 路径归一化成可与 ProcEntry::path 直接比较的字符串。
std::wstring normForMatch(const fs::path& p) {
    // 进程报的路径已经是绝对路径，这里只做 canonical + 大小写，不必再 absolute。
    std::error_code ec;
    const fs::path real = fs::weakly_canonical(p, ec);
    std::wstring s = (ec ? p : real).wstring();
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return lowerW(s);
}

// 主窗判据只有一份：`pxc_launch::findMainWindowOfPid`。
// 它和"等 PXC 窗口出现"共用同一套（可见 / 无 owner / rcNormalPosition 宽 > 400），
// 免得两处判据漂移 —— 一边认得另一边不认，表现就是"等窗口 60 秒超时但窗口明明开着"。
HWND mainWindowOf(DWORD pid) {
    return (HWND)pxc_launch::findMainWindowOfPid((unsigned long)pid);
}

// ---- 找"同版本实例" ----
// 同版本可能装了好几份（Steam + 便携 / 手动登记）。**用户正在用的未必是偏好选中的那份**，
// 所以按"主 exe 优先、其余候选顺延"的顺序找：哪份在跑就复用哪份。
// 只看主 exe 的后果（2026-11-04 用户反馈）：偏好指向 A，实际跑的是 B → 判成"没有实例"
// → 静默再开一个 B，用户莫名其妙多了一个同版本进程。
//
// 这段判据现在有两个使用者（复用 / 重试前等窗口），必须只有一份实现 ——
// 两份实现漂移的后果是"等到了窗口但复用找不到进程"，或者反过来。
struct Hit {
    DWORD pid = 0;
    fs::path exe;
};

struct Instances {
    std::vector<Hit> hits;
    int unreadable = 0;      // 名字是 PXC 但读不到路径：不能当成"没有实例"
    int candidateCount = 0;  // 去重后参与匹配的 exe 份数（写文案用）
};

Instances findInstances(const fs::path& exePath, const std::vector<fs::path>& altExes) {
    std::vector<fs::path> candidates{exePath};
    std::vector<std::wstring> want{normForMatch(exePath)};
    for (const auto& alt : altExes) {
        const std::wstring n = normForMatch(alt);
        if (std::find(want.begin(), want.end(), n) != want.end()) continue;
        want.push_back(n);
        candidates.push_back(alt);
    }

    Instances ins;
    ins.candidateCount = (int)candidates.size();
    for (const auto& pe : listProcesses()) {
        if (pe.path.empty()) {
            if (pe.looksPxc) ++ins.unreadable;
            continue;
        }
        for (std::size_t i = 0; i < want.size(); ++i)
            if (pe.path == want[i]) {
                ins.hits.push_back({pe.pid, candidates[i]});
                break;
            }
    }
    return ins;
}

// ---- 置前 ----
// 抢前台在 Windows 上有"前台锁"：不是当前前台线程的进程不能随便抢。
// 常规解法是 AttachThreadInput 把自己的输入队列挂到"当前前台线程"和"目标线程"上，
// 借它们的输入状态把窗口顶上来，完事再摘掉。
// 窗口缩在任务栏时，除了抢前台还得先把它**还原**出来。
// ShowWindow(SW_RESTORE) 是投递给目标线程的异步动作：调完立刻查 IsIconic 往往还是真，
// 查前台也还是别人。所以这里轮询到状态真的变过来为止，而不是sleep 一次就下结论。
bool waitRestored(HWND hwnd, DWORD timeoutMs) {
    if (!IsIconic(hwnd)) return true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (IsIconic(hwnd)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        ShowWindow(hwnd, SW_RESTORE);
        sleepMs(50);
    }
    return true;
}

// 该进程此刻有没有任何可见的对话框（`#32770`）。
//
// 只用来回答一个问题：**能不能碰它的主窗**。所以判据故意放宽 —— 不要求是「打开」窗：
// 只要是对话框，激活主窗、往主窗发键都不对。真正"能不能复用"仍由 findOpenDialog 判。
//
// 为什么不能拿「主窗是否被禁用」当判据（2026-11-04 实测）：PXC 的「打开」窗是**独立顶层窗
// 既没有 parent 也没有 owner**，主窗从头到尾都是启用的。用户看到"PXC 动不了"不是因为父窗被禁用，
// 而是 GameMaker 在对话框打开期间整帧挂起 —— 主窗还在画最后一帧，但不再处理输入。
// 一把它顶到对话框前面，人就点不到「打开/取消」，只能动任务栏。
bool hasVisibleDialog(DWORD pid) {
    struct Ctx {
        DWORD pid;
        bool found;
    } c{pid, false};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* ctx = reinterpret_cast<Ctx*>(lp);
            DWORD p = 0;
            GetWindowThreadProcessId(h, &p);
            if (p != ctx->pid || !IsWindowVisible(h)) return TRUE;
            wchar_t cls[64] = {0};
            GetClassNameW(h, cls, 64);
            if (_wcsicmp(cls, L"#32770") == 0) {
                ctx->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&c));
    return c.found;
}

bool bringToFront(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    // 目标有对话框开着时，**绝不能**把主窗提到前台。
    //
    // 那个对话框是独立顶层窗（无 parent / 无 owner），主窗也没被禁用 —— 一激活主窗，主窗就
    // 跑到对话框前面：对话框在后面够不着，主窗又因为 GameMaker 整帧挂起而点不动，
    // 用户看到的就是"PXC 死在那儿"（2026-11-04 用户遇到的正是这个）。要提的是对话框（raiseToFront）。
    DWORD targetPid = 0;
    GetWindowThreadProcessId(hwnd, &targetPid);
    if (hasVisibleDialog(targetPid)) {
        pxc_log::write("WARN", "Reuse", "bring_blocked | 目标开着对话框，拒绝把主窗提到前台");
        return false;
    }

    const bool wasIconic = IsIconic(hwnd) != 0;
    if (!waitRestored(hwnd, 1500)) return false;   // 还原失败：别继续（免得把键发给别人）
    // 刚还原的窗口要给它几帧把激活状态立起来，否则 GameMaker 下一逻辑帧读到的是旧状态。
    if (wasIconic) sleepMs(120);
    if (GetForegroundWindow() == hwnd) return true;

    const DWORD cur = GetCurrentThreadId();
    const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD tgtThread = GetWindowThreadProcessId(hwnd, nullptr);
    const bool a1 = (fgThread && fgThread != cur) ? AttachThreadInput(cur, fgThread, TRUE) != 0 : false;
    const bool a2 = (tgtThread && tgtThread != cur) ? AttachThreadInput(cur, tgtThread, TRUE) != 0 : false;
    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; ++attempt) {   // 前台锁偶尔要连试
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        SetFocus(hwnd);
        sleepMs(80);
        ok = (GetForegroundWindow() == hwnd);
    }
    if (a1) AttachThreadInput(cur, fgThread, FALSE);
    if (a2) AttachThreadInput(cur, tgtThread, FALSE);
    return ok;
}

// 把一个**对话框**提到前台（不碰主窗）。
//
// 为什么要单独一个函数：那个「打开」窗是无 parent / 无 owner 的独立顶层窗，被主窗盖住之后
// 人根本点不到它；而主窗此刻又被 GameMaker 挂起、点不动。两头都点不动 = "PXC 死在那儿"。
// 复用"屏幕上已有的对话框"时，要抢的是**这个对话框**：抢完填表、点「打开」，模态结束，
// 主窗自然活过来。
bool raiseToFront(HWND w) {
    if (!w || !IsWindow(w)) return false;
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    if (GetForegroundWindow() == w) return true;

    const DWORD cur = GetCurrentThreadId();
    const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD tgtThread = GetWindowThreadProcessId(w, nullptr);
    const bool a1 = (fgThread && fgThread != cur) ? AttachThreadInput(cur, fgThread, TRUE) != 0 : false;
    const bool a2 = (tgtThread && tgtThread != cur) ? AttachThreadInput(cur, tgtThread, TRUE) != 0 : false;
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
        BringWindowToTop(w);
        SetForegroundWindow(w);
        sleepMs(60);
        ok = (GetForegroundWindow() == w);
    }
    if (a1) AttachThreadInput(cur, fgThread, FALSE);
    if (a2) AttachThreadInput(cur, tgtThread, FALSE);
    return ok;
}

// ---- 真实按键 ----
// 必须是真实按键（SendInput）：GameMaker 靠逐帧读键盘状态，PostMessage(WM_KEYDOWN) 不改那个状态。
//
// **必须分帧慢发**。一次 SendInput 把 4 个事件全部落下（彼此相隔 <1ms），等到 PXC 下一个
// 逻辑帧去轮询时 Ctrl 与 O 都已经抬起 —— 按下沿从未被观测到，Ctrl+O 等于没按。
// 实测（PXC 1.20.1，2026-11-01）：一次全发 → SendInput 返回 4（"成功"）但 6 秒内不弹对话框；
// 每步间隔 120ms → 0.3 秒弹出。所以**发送成功 ≠ 按到**，这个坑只在真机上才看得见。
bool sendKey(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool tapCtrlO() {
    if (!sendKey(VK_CONTROL, false)) return false;
    sleepMs(120);
    if (!sendKey('O', false)) return false;
    sleepMs(120);
    if (!sendKey('O', true)) return false;
    sleepMs(120);
    return sendKey(VK_CONTROL, true);
}

// 「发键前是不是 PXC 在前台」这件事不单独判了 —— 一律走 `bringToFront` 的返回值：
// 它自己会 SetForegroundWindow + 校验，抢不到就返回 false。
// （曾经有个 `foregroundIs(pid)` 的独立判据，但它只能"确认"，不能"纠正"，
//   而我们恰恰需要纠正：等卡顿的那一秒里前台随时会跑掉。）

// ---- 文件对话框 ----
struct DlgCtx {
    DWORD pid = 0;
    HWND dlg = nullptr;
};

bool textContainsOpen(const std::wstring& t) {
    return t.find(L"打开") != std::wstring::npos || t.find(L"Open") != std::wstring::npos ||
           t.find(L"open") != std::wstring::npos;
}

struct ChildCtx {
    HWND edit = nullptr;
    HWND button = nullptr;
};

BOOL CALLBACK enumChildProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<ChildCtx*>(lp);
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t cls[64] = {0};
    GetClassNameW(h, cls, 64);
    if (!c->edit && _wcsicmp(cls, L"Edit") == 0) {
        c->edit = h;
        return TRUE;
    }
    if (!c->button && _wcsicmp(cls, L"Button") == 0) {
        wchar_t txt[128] = {0};
        GetWindowTextW(h, txt, 128);
        if (textContainsOpen(txt)) c->button = h;
    }
    return TRUE;
}

// 只认【#32770 + 有可见 Edit + 按钮含「打开」/Open】。
// 这条判据不能放松：弹出来的也可能是"是否保存"，误点一下就把用户的工程存了。
bool inspectDialog(HWND h, ChildCtx& out) {
    if (!IsWindowVisible(h)) return false;
    wchar_t cls[64] = {0};
    GetClassNameW(h, cls, 64);
    if (_wcsicmp(cls, L"#32770") != 0) return false;
    ChildCtx c;
    EnumChildWindows(h, enumChildProc, reinterpret_cast<LPARAM>(&c));
    if (!c.edit || !c.button) return false;
    out = c;
    return true;
}

BOOL CALLBACK enumDlgProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<DlgCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;
    ChildCtx cc;
    if (inspectDialog(h, cc)) {
        c->dlg = h;
        return FALSE;
    }
    return TRUE;
}

HWND waitForOpenDialog(DWORD pid, DWORD timeoutMs, ChildCtx& cc) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        DlgCtx c;
        c.pid = pid;
        EnumWindows(enumDlgProc, reinterpret_cast<LPARAM>(&c));
        if (c.dlg) {
            inspectDialog(c.dlg, cc);
            return c.dlg;
        }
        sleepMs(80);
    }
    return nullptr;
}

// 一眼看现在有没有「打开」对话框开着 —— 不等。
//
// 为什么需要：上一次尝试发的 Ctrl+O 可能在 PXC 恢复之后才落地，于是"这次"还没按，
// 对话框已经在屏幕上了。这时再按一次就是第二个窗。有就**直接用**，绝不再按。
HWND findOpenDialog(DWORD pid, ChildCtx& cc) {
    DlgCtx c;
    c.pid = pid;
    EnumWindows(enumDlgProc, reinterpret_cast<LPARAM>(&c));
    if (c.dlg) inspectDialog(c.dlg, cc);
    return c.dlg;
}

// ---- 目标窗口是否还在处理消息 ----
//
// 这是"按键排队"和"按键没反应"的分界线：卡住的 PXC 不回 WM_NULL，我们就不该发键 ——
// 发了也只是往它的队列里再塞一次，等它恢复后一次性弹出好几个对话框。
bool pumpingMessages(HWND hwnd, DWORD ms) {
    DWORD_PTR res = 0;
    return SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, ms, &res) != 0;
}

enum class Pump { Ready, Timeout, Canceled };

// 等它不再卡着。卡住期间每秒回调一次（提示窗显示"已 N 秒"）。
Pump waitPumping(HWND hwnd, DWORD timeoutMs, const std::function<void(int)>& onTick,
                 const CancelFn& cancelled) {
    const auto t0 = std::chrono::steady_clock::now();
    int lastSec = -1;
    for (;;) {
        if (cancelled && cancelled()) return Pump::Canceled;
        if (pumpingMessages(hwnd, 400)) {
            // 刚恢复的那一下通常还在收尾（正在建工程 / 正在读盘）：再确认一次，
            // 两次都回话才认为真的能收键了。
            sleepMs(200);
            if (pumpingMessages(hwnd, 400)) return Pump::Ready;
        }
        const int sec = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count() /
                        1000;
        if (sec != lastSec) {
            lastSec = sec;
            if (onTick) onTick(sec);
        }
        if (std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(timeoutMs))
            return Pump::Timeout;
        sleepMs(200);
    }
}

// ---- 关掉多余的「打开」对话框 ----
//
// 判据必须和填表那套一致（#32770 + 可见 Edit + 按钮含「打开」）：
// 顺手关掉"是否保存"这类提问框等于替用户做了决定，那种框一个都不能碰。
struct CloseCtx {
    DWORD pid = 0;
    HWND keep = nullptr;
    int closed = 0;
};

BOOL CALLBACK enumCloseProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<CloseCtx*>(lp);
    if (h == c->keep) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;
    ChildCtx cc;
    if (!inspectDialog(h, cc)) return TRUE;
    PostMessageW(h, WM_CLOSE, 0, 0);
    ++c->closed;
    return TRUE;
}

int closeStrayDialogs(DWORD pid, HWND keep) {
    CloseCtx c;
    c.pid = pid;
    c.keep = keep;
    EnumWindows(enumCloseProc, reinterpret_cast<LPARAM>(&c));
    return c.closed;
}

// 收尾扫描：排队的按键只会在前一个对话框关掉之后才被处理，所以要扫一段窗口而不是扫一眼。
void drainStrayDialogs(DWORD pid, DWORD ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    int total = 0;
    while (std::chrono::steady_clock::now() < until) {
        total += closeStrayDialogs(pid, nullptr);
        sleepMs(150);
    }
    if (total > 0)
        pxc_log::write("WARN", "Reuse",
                       "closed_stray_dialogs | 关掉 " + std::to_string(total) +
                           " 个多余的「打开」对话框（按键排队晚到）");
}

// ---- 校验：recent.json 首项 ----
// PXC 不是原子写：mtime 在 1 秒内变过就退 200ms 重读，避免读到半截文件。
std::string recentFirst(const fs::path& recentFile) {
    std::error_code ec;
    auto t1 = fs::last_write_time(recentFile, ec);
    if (ec) return {};
    auto [jv, ok] = pxc_io::readJson(recentFile);
    auto t2 = fs::last_write_time(recentFile, ec);
    if (!ec && t1 != t2) {
        sleepMs(200);
        auto again = pxc_io::readJson(recentFile);
        if (again.second) jv = again.first;
        else return {};
    }
    if (!ok || !jv.isObj()) return {};
    const auto* arr = jv.find("Recents");
    if (!arr || !arr->isArr() || arr->arr.empty()) return {};
    const auto& first = arr->arr.front();
    return first.isStr() ? first.str : std::string();
}

bool samePath(const std::string& a, const fs::path& b) {
    if (a.empty()) return false;
    return lowerW(pxc_util::pathFromUtf8(a).wstring()) == normPath(b);
}

bool recentIsTarget(const fs::path& recentFile, const fs::path& pxcPath) {
    return samePath(recentFirst(recentFile), pxcPath);
}

} // namespace

const char* resultName(Result r) {
    switch (r) {
        case Result::Ok: return "ctrl_o";
        case Result::NoInstance: return "no_instance";
        case Result::Undetermined: return "undetermined";
        case Result::Busy: return "busy";
        case Result::ForegroundFailed: return "no_foreground";
        case Result::NoDialog: return "no_dialog";
        case Result::NotResponding: return "not_responding";
        case Result::DialogOpen: return "dialog_open";
        case Result::VerifyFailed: return "failed";
        case Result::CallFailed: return "call_failed";
        case Result::Canceled: return "canceled";
    }
    return "?";
}

// 冒烟钩子（只有设了环境变量才生效）：`PXC_LAUNCHER_DEBUG_REUSE_FAIL=<n>` 让前 n 次复用
// 尝试直接判失败。
//
// 为什么需要它：验证「重试」按钮的整条链路得先造一次失败，而最省事的造法是"把 PXC 窗口
// 最小化 / 改尺寸，让它判不出来"。2026-11-04 那么测了一次，代价是把 GameMaker 的窗口状态
// 搞坏 —— 还原矩形被撑成接近全屏、最大化后盖住任务栏、点击落点跟着偏移。测试不该改动
// 被测外的东西，所以改成**在启动器自己身上注入失败**：不抢前台、不发按键、不碰 PXC。
bool debugReuseFailNow() {
    static int left = -1;
    if (left < 0) {
        wchar_t b[16] = {0};
        left = GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_REUSE_FAIL", b, 16) > 0 ? _wtoi(b) : 0;
        if (left < 0) left = 0;
    }
    if (left == 0) return false;
    --left;
    return true;
}

Outcome openInExisting(const fs::path& exePath, const fs::path& pxcPath,
                       const fs::path& recentFile, const StageSink& onStage,
                       const CancelFn& cancelled,
                       const std::vector<fs::path>& altExes, const TickSink& onTick) {
    // 阶段边界统一走这里：既报阶段，又给取消一个抓手。
    // 放在每个"会产生外部副作用"的动作之前，而不是循环里 —— 取消的意义是"还没交给 PXC"。
    auto stageIn = [&](pxc_stage::Stage st) {
        if (onStage) onStage(st);
        return !(cancelled && cancelled());
    };
    Outcome out;
    if (exePath.empty() || pxcPath.empty()) {
        out.result = Result::NoInstance;
        out.detail = "参数不完整";
        return out;
    }

    if (!stageIn(pxc_stage::Stage::ReuseFind)) {
        out.result = Result::Canceled;
        out.detail = "已取消";
        return out;
    }

    // 同版本可能装了好几份（Steam + 便携 / 手动登记），哪份在跑就复用哪份 —— 判据见 findInstances。
    const Instances ins = findInstances(exePath, altExes);
    if (ins.hits.empty()) {
        // 读不到路径的 PXC 只可能是权限问题（UIPI：对方以管理员运行）。
        // 这时既不能确认"没有实例"，也复用它不了 —— 交给调用方问用户，
        // 绝不能像 NoInstance 那样直接再开一个。
        if (ins.unreadable > 0) {
            out.result = Result::Undetermined;
            out.detail = "有 " + std::to_string(ins.unreadable) +
                         " 个 PXC 进程读不到所在路径（可能以管理员身份运行），无法判断能否复用";
            return out;
        }
        out.result = Result::NoInstance;
        out.detail = ins.candidateCount <= 1
                         ? "没有同版本的 PXC 在运行"
                         : "同版本的 " + std::to_string(ins.candidateCount) +
                               " 份安装都没有进程在运行";
        return out;
    }
    // 同一版本可能开着好几个实例（复用失败退化成新进程就会积下一个）：
    // 优先用户正看着的那个，否则按枚举顺序取第一个。
    {
        const HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        std::size_t pick = 0;
        for (std::size_t i = 0; i < ins.hits.size(); ++i)
            if (ins.hits[i].pid == fgPid) {
                pick = i;
                break;
            }
        out.pid = ins.hits[pick].pid;
        out.usedExe = ins.hits[pick].exe;
    }

    // 进程在、主窗还没出来 = PXC 正在冷启动（实测 18.5 秒）。
    // 这时候绝不能"再开一个新进程"—— 用户会得到两个 PXC 抢同一个工程。
    HWND hwnd = nullptr;
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20000);
        while (!hwnd && std::chrono::steady_clock::now() < deadline) {
            hwnd = mainWindowOf(out.pid);
            if (!hwnd) sleepMs(150);
        }
    }
    if (!hwnd) {
        out.result = Result::Busy;
        out.detail = "PXC 进程在，但主窗口还没出来（正在启动）";
        return out;
    }

    if (!stageIn(pxc_stage::Stage::ReuseHotkey)) {
        out.result = Result::Canceled;
        out.detail = "已取消（未发送按键）";
        return out;
    }
    // 注入点放在"已确认有实例"之后、"抢前台"之前：既能让调用方走进询问窗，
    // 又保证整个过程没有对外部程序产生任何副作用（见 debugReuseFailNow 的注释）。
    if (debugReuseFailNow()) {
        out.result = Result::NoDialog;
        out.detail = "调试注入：本次尝试判失败（PXC_LAUNCHER_DEBUG_REUSE_FAIL，未发送按键）";
        pxc_log::write("WARN", "Reuse",
                       "debug_fail_injected | pid=" + std::to_string(out.pid) +
                           " | file=" + pxcPath.u8string());
        return out;
    }
    // ---- 发 Ctrl+O 之前的两道闸 ----
    //
    // ① 屏幕**已经有一个「打开」对话框** → 直接用它。上一次尝试的按键可能直到 PXC 恢复
    //    才落地，这时再按一次就是第二个窗。
    // ② 目标窗口**卡着** → 一个键都不发。卡住时按键不丢，而是积在它的消息队列里，
    //    等它缓过来一次性被当成几次按下 —— 用户 2026-11-04 实测："复用成功了，随后连跳
    //    几个导入工程窗口"，就是第 1 次尝试的那两发加上重试那一发一起落地。
    //
    // 两条合起来保证"一次启动最多多出一个对话框"，剩下的由收尾扫描兜底（见 sweeper）。

    // 收尾扫描：`g_pendingPress` 是**跨尝试**的计数 —— 上一轮发的键可能还排在 PXC 队列里，
    // 只扫当前这次尝试是看不到它们的。离开复用链时只要还大于 0，就扫一段（不是扫一眼：
    // 排队的按键要等前一个对话框关掉之后才会被处理）。
    struct PressSweeper {
        DWORD pid;
        ~PressSweeper() {
            if (g_pendingPress.load() > 0) {
                drainStrayDialogs(pid, kDrainMs);
                g_pendingPress.store(0);   // 扫完就归零：留下的只可能是"根本没落地"的按键，不能一直欠着
            }
        }
    } sweeper{out.pid};

    ChildCtx cc;   // 下面两个 lambda 都要用它（填表时拿 edit/button 句柄）

    // 填表 + 校验：adopt（复用屏幕上已有的对话框）和"按出来"两条路都走这一段。
    auto fillAndVerify = [&](HWND dlg) -> Outcome {
        if (!stageIn(pxc_stage::Stage::ReuseFill)) {
            // 对话框已经弹出来了，取消时必须顺手关掉，不能留在用户屏幕上。
            PostMessageW(dlg, WM_CLOSE, 0, 0);
            out.result = Result::Canceled;
            out.detail = "已取消（已关掉打开对话框）";
            return out;
        }
        // 中文路径必须原样过宽字符，不能走窄串（GBK 会把它拆坏）。
        std::wstring wpath = pxcPath.wstring();
        SendMessageW(cc.edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(wpath.c_str()));
        SendMessageW(cc.button, BM_CLICK, 0, 0);

        if (onStage) onStage(pxc_stage::Stage::ReuseVerify);
        // 校验：**以"对话框消失"为准**，recent.json 命中只当额外佐证。
        //
        // 为什么不能只靠原来那两个依据 —— 它们在 PXC 1.20.1 上都是死的（2026-11-01 实测）：
        //   · recent.json 只在 PXC **正常退出**时写：当天整个用户目录零写入，文件停在 9-24；
        //   · 窗口标题不随标签切换变化：多文档里新开一个标签后 GetWindowText 仍是旧工程名。
        // 而"点了「打开」对话框就关掉"是可靠信号 —— 点「取消」得有人手动点，我们没发过 WM_CLOSE。
        const auto vDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kVerifyWaitMs);
        while (std::chrono::steady_clock::now() < vDeadline) {
            if (recentIsTarget(recentFile, pxcPath)) {
                out.result = Result::Ok;
                out.detail = "已在原进程里打开（recent.json 已记录）";
                return out;
            }
            if (!IsWindow(dlg)) {
                out.result = Result::Ok;
                out.detail = "已在原进程里打开（新增文件标签）";
                return out;
            }
            sleepMs(60);
        }

        // 失败：别把文件对话框留在用户屏幕上让他自己收拾。
        if (IsWindow(dlg)) PostMessageW(dlg, WM_CLOSE, 0, 0);
        out.result = Result::VerifyFailed;
        out.detail = "填了路径也点了「打开」，但对话框一直没关，已强制关掉";
        return out;
    };

    // 复用屏幕上已有的「打开」窗：一个键都不按，也绝不把主窗提上来。
    auto adopt = [&](HWND d) -> Outcome {
        if (g_pendingPress.load() > 0) --g_pendingPress;   // 这一发兑现了
        pxc_log::write("INFO", "Reuse", "adopt_dialog | 已有打开对话框，直接用它，未重发 Ctrl+O");
        raiseToFront(d);
        return fillAndVerify(d);
    };

    HWND dlg = findOpenDialog(out.pid, cc);
    bool retried = false;

    // 闸①：屏幕上已经有「打开」窗 —— 直接用它。上一次尝试的按键可能直到 PXC 恢复才落地，
    // 这时再按一次就是第二个窗。
    if (dlg) return adopt(dlg);

    if (!stageIn(pxc_stage::Stage::ReuseHotkey)) {
        out.result = Result::Canceled;
        out.detail = "已取消（未发送按键）";
        return out;
    }

    // 闸②：卡着就一个键都不发。卡住时按键不丢，而是积在它的消息队列里，等它缓过来一次性
    // 被当成几次按下 —— 用户 2026-11-04 实测："复用成功了，随后连跳几个导入工程窗口"。
    {
        const Pump p = waitPumping(hwnd, kIdleWaitMs,
                                   [&](int sec) {
                                       if (onTick) onTick(sec);
                                   },
                                   cancelled);
        if (p == Pump::Canceled) {
            out.result = Result::Canceled;
            out.detail = "已取消（未发送按键）";
            return out;
        }
        if (p == Pump::Timeout) {
            pxc_log::write("WARN", "Reuse", "idle_timeout | PXC 卡着不处理消息，未发送按键");
            out.result = Result::NotResponding;
            out.detail = "PXC 卡在启动/建工程里（一直不响应），没有发送按键";
            return out;
        }
    }

    // 闸① 复查 —— 2026-11-04 那次"PXC 动不了"的缺口就在这里。
    //
    // "等它不卡"最长要等 20 秒，而**之前那几发按键可能恰好在这段时间里落地**，屏幕上于是
    // 已经有了「打开」窗。旧代码只在等待之前查一次，接着就去抢前台，把主窗压到对话框前面：
    // 主窗被模态框禁用（点不动）、对话框在它后面（够不着），整个 PXC 看起来就死了，
    // 后续几次测试也全撞在这上面。
    dlg = findOpenDialog(out.pid, cc);
    if (dlg) {
        pxc_log::write("INFO", "Reuse", "adopt_dialog | 等待期间对话框已落地，直接用它");
        return adopt(dlg);
    }

    // 屏幕上还开着对话框（这里指没被 findOpenDialog 认出来的那些：#32770 但不是「打开」窗，
    // 或者我们判据没覆盖到的变体）→ 不能按，也绝不能把主窗提上来。
    // 主窗**不会**被禁用（那个「打开」窗没有 parent / 没有 owner，2026-11-04 实测），
    // 所以判据只能是"有没有对话框"，不能是 IsWindowEnabled。
    if (hasVisibleDialog(out.pid)) {
        pxc_log::write("WARN", "Reuse", "dialog_blocked | 目标开着对话框，未发送按键");
        out.result = Result::DialogOpen;
        out.detail = "PXC 屏幕上开着对话框（不是「打开」窗），没有发送按键";
        return out;
    }

    // 抢前台必须**紧挨着**发键：抢完再等一秒（等卡顿过去就要等这么久），前台很可能已经
    // 被别的窗口拿走 —— 包括我们自己的提示窗。按键是发给当前前台窗口的，那一下就会打空
    // （2026-11-04 实测：先抢前台再等 0.8 秒，两次 Ctrl+O 都没进 PXC）。
    if (!bringToFront(hwnd)) {
        out.result = Result::ForegroundFailed;
        out.detail = IsIconic(hwnd)
            ? "PXC 窗口缩在任务栏且无法自动还原（可能以管理员身份运行），未发送按键"
            : "抢不到前台，未发送按键（避免 Ctrl+O 打到别的程序）";
        return out;
    }
    // 置前后再压一小段：目标进程要先处理完还原/激活，才轮得到读取键盘状态。
    sleepMs(150);
    // 最后一刻再查：这 150 ms 里排队的按键也可能刚好落地。
    dlg = findOpenDialog(out.pid, cc);
    if (dlg) return adopt(dlg);
    // 发键前把"此刻谁在前台"记下来。SendInput 只看前台，前台不对就整发落空，
    // 而日志里除了"没等到对话框"什么都看不出来 —— 这一行是唯一能复盘的证据。
    {
        const HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        pxc_log::write("INFO", "Reuse",
                       "pre_key | fg=" + std::to_string((unsigned long long)fg) +
                           " fgPid=" + std::to_string(fgPid) +
                           " target=" + std::to_string((unsigned long long)hwnd) +
                           " targetPid=" + std::to_string(out.pid) +
                           " iconic=" + (IsIconic(hwnd) ? "1" : "0") +
                           " match=" + (fg == hwnd ? "1" : "0"));
    }
    if (!tapCtrlO()) {
        out.result = Result::CallFailed;
        out.detail = "SendInput 失败（可能被 UIPI 拦截：PXC 以管理员运行时会发生）";
        return out;
    }
    ++g_pendingPress;

    if (!stageIn(pxc_stage::Stage::ReuseDialog)) {
        out.result = Result::Canceled;
        out.detail = "已取消（已按 Ctrl+O，正在等对话框）";
        return out;
    }
    dlg = waitForOpenDialog(out.pid, kDialogFirstWaitMs, cc);
    if (!dlg) {
        // 第一次没等到：重发一次完整 Ctrl+O。
        //
        // 为什么值得重试 —— 实测"第一次没反应、再按一次就出来"，但**根因未明**
        // （两次误判都已证伪，见 docs/05 §9.14.5：不是提示窗吃点击，也不是 PXC 启动页吃焦点，
        // 启动页开着照样能触发 Ctrl+O）。所以这里只做"重试"，不再附加任何解释性动作。
        if (!stageIn(pxc_stage::Stage::ReuseRetry)) {
            out.result = Result::Canceled;
            out.detail = "已取消（未重发按键）";
            return out;
        }
        if (waitPumping(hwnd, kIdleWaitMs, nullptr, nullptr) != Pump::Ready) {
            // 重发前也要过卡顿这道闸，否则就是往队列里再塞一个。
            pxc_log::write("WARN", "Reuse", "retry_skip | PXC 仍卡着不处理消息，未重发 Ctrl+O");
        } else if ((dlg = findOpenDialog(out.pid, cc)) != nullptr) {
            // 就在等它缓过来的这段里，第一次那发按键落地了 —— 直接用，别再按。
            pxc_log::write("INFO", "Reuse", "adopt_dialog | 重发前对话框已落地，直接用它");
            return adopt(dlg);
        } else if (!bringToFront(hwnd)) {
            // 抢不到前台：这一下按键会落到别人程序上（Ctrl+O 等于替用户开别人的文件对话框）。
            pxc_log::write("WARN", "Reuse", "retry_skip | 抢不到前台，未重发 Ctrl+O");
        } else if (!tapCtrlO()) {
            pxc_log::write("WARN", "Reuse", "retry_failed | SendInput 失败");
        } else {
            retried = true;
            ++g_pendingPress;
            pxc_log::write("INFO", "Reuse", "retry | 首次 Ctrl+O 未等到对话框，已重发一次");
            // 重发的是完整组合键，不是只补一个 O：单独补 O，主窗那边没有 Ctrl 按住的状态。
            dlg = waitForOpenDialog(out.pid, kDialogWaitMs, cc);
        }
        if (!dlg) {
            out.result = Result::NoDialog;
            out.detail = retried ? "没等到打开对话框（已重发一次 Ctrl+O）"
                                 : "没等到打开对话框";
            return out;
        }
    }
    // 等到对话框 = 有一发按键兑现了。
    if (g_pendingPress.load() > 0) --g_pendingPress;
    return fillAndVerify(dlg);
}

// ---------------- 重试前等实例窗口 ----------------

bool waitInstanceWindow(const fs::path& exePath, const std::vector<fs::path>& altExes,
                        int timeoutMs, const std::function<void(int)>& onTick,
                        const CancelFn& cancelled) {
    if (exePath.empty()) return true;
    Instances ins = findInstances(exePath, altExes);
    if (ins.hits.empty()) return true;   // 没有实例：重试会走首启，没什么可等

    const int budget = timeoutMs > 0 ? timeoutMs : 20000;
    const auto t0 = std::chrono::steady_clock::now();
    int lastSec = -1;
    for (;;) {
        if (cancelled && cancelled()) return false;
        for (const auto& h : ins.hits)
            if (mainWindowOf(h.pid)) return true;

        const int sec = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count() /
                        1000;
        if (sec != lastSec) {
            lastSec = sec;
            if (onTick) onTick(sec);
            // 一秒重扫一次进程表：等的这段时间里目标可能已经退出（崩溃 / 用户手动关掉），
            // 那就别再干等了 —— 当成"没有实例"，交给调用方走首启。
            ins = findInstances(exePath, altExes);
            if (ins.hits.empty()) return true;
        }
        if (sec * 1000 >= budget) return false;
        sleepMs(150);
    }
}

// ---------------- 异步 ----------------
namespace {

struct AsyncState {
    std::mutex mtx;
    bool busy = false;
    bool done = false;
    Outcome out;
};

// 故意用 new 且不释放：后台线程可能在静态析构之后才写结果，
// 让这块内存活到进程结束，比在退出时踩悬垂指针安全。
AsyncState& state() {
    static AsyncState* s = new AsyncState();
    return *s;
}

} // namespace

bool beginAsync(const fs::path& exePath, const fs::path& pxcPath, const fs::path& recentFile) {
    AsyncState& s = state();
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (s.busy) return false;
        s.busy = true;
        s.done = false;
    }
    std::thread([exePath, pxcPath, recentFile] {
        Outcome o = openInExisting(exePath, pxcPath, recentFile);
        AsyncState& st = state();
        std::lock_guard<std::mutex> lk(st.mtx);
        st.out = o;
        st.done = true;
        st.busy = false;
        pxc_log::write(o.result == Result::Ok ? "INFO" : "WARN", "Reuse",
                       std::string("reuse=") + resultName(o.result) + " | " + o.detail +
                           " | pid=" + std::to_string(o.pid) + " | " + pxcPath.u8string());
    }).detach();
    return true;
}

bool poll(Outcome& out) {
    AsyncState& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (!s.done) return false;
    s.done = false;
    out = s.out;
    return true;
}

bool busy() {
    AsyncState& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    return s.busy;
}

} // namespace pxc_reuse
