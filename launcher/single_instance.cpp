// 单实例实现：Local 命名互斥体 + WM_COPYDATA 转发。
#include "single_instance.h"

#include "log.h"
#include "util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace pxc_single {

namespace {

HANDLE g_mutex = nullptr;
unsigned long g_existingPid = 0;

std::wstring lowerW(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

std::wstring selfExePath() {
    wchar_t buf[MAX_PATH * 4] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return {};
    return lowerW(std::wstring(buf, n));
}

// 进程 exe 路径（小写）；拿不到返回空。
std::wstring processExePath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 4] = {0};
    DWORD size = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &size)) out = lowerW(std::wstring(buf, size));
    CloseHandle(h);
    return out;
}

struct FindCtx {
    std::wstring self;
    std::vector<HWND> windows;
};

BOOL CALLBACK enumProc(HWND h, LPARAM lp);

// 找本程序其它实例的可见顶层窗。waitMs > 0 时在找不到的情况下重试
// （已有实例正在启动的那一秒，窗口确实还不存在 —— 直接放弃会把路径丢掉）。
std::vector<HWND> findExistingWindows(int waitMs) {
    FindCtx ctx;
    ctx.self = selfExePath();
    if (ctx.self.empty()) return {};
    const int step = 100;
    for (int waited = 0;; waited += step) {
        ctx.windows.clear();
        EnumWindows(enumProc, reinterpret_cast<LPARAM>(&ctx));
        if (!ctx.windows.empty() || waited >= waitMs) break;
        Sleep((DWORD)step);
    }
    return ctx.windows;
}

BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER) != nullptr) return TRUE;
    // 跳过工具窗（启动进度提示窗就是 WS_EX_TOOLWINDOW + TOPMOST）：
    // 它同属本进程、可见、无 owner，不筛掉的话
    //   ① 转发会先打到它身上（它不处理 WM_COPYDATA，白白多一次跨线程同步等待）；
    //   ② `activateExisting()` 取 `windows.front()`，topmost 的提示窗排在 Z 序最前 ——
    //      于是"把已有窗口提到前台"提的是那个不激活的浮窗，主窗仍然不见天日。
    if ((GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return TRUE;
    if (processExePath(pid) != ctx->self) return TRUE;
    g_existingPid = pid;
    ctx->windows.push_back(h);
    return TRUE;
}

} // namespace

bool acquire() {
    if (g_mutex) return true;
    // Local\ 前缀：同一会话内唯一；跨用户会话（快速切换用户）互不影响，这正是我们要的。
    g_mutex = CreateMutexW(nullptr, FALSE, L"Local\\PXCLauncher.Instance");
    if (!g_mutex) {
        pxc_log::write("WARN", "Single", "CreateMutex 失败，退化为多实例");
        return true;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) {
        // 互斥体已存在：别人先起了。句柄留着不关也无所谓 —— 本进程马上就退。
        return false;
    }
    pxc_log::write("INFO", "Single", "取得唯一实例身份");
    return true;
}

unsigned long existingPid() { return g_existingPid; }

bool notifyExisting(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) return false;
    const std::vector<HWND> windows = findExistingWindows(2000);
    if (windows.empty()) {
        pxc_log::write("WARN", "Single", "找到互斥体但 2 秒内没找到已有窗口，放弃转发");
        return false;
    }

    // 负载：UTF-8 多行路径（和拖放那边的语义一致：一行一个）。
    std::string payload;
    for (const auto& p : paths) {
        payload += p.u8string();
        payload.push_back('\n');
    }
    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataMagic;
    cds.cbData = (DWORD)payload.size();
    cds.lpData = (void*)payload.data();

    bool any = false;
    for (HWND h : windows) {
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(h, WM_COPYDATA, (WPARAM)GetCurrentProcessId(), (LPARAM)&cds,
                                SMTO_ABORTIFHUNG, 3000, &res) &&
            res != 0) {
            any = true;
            break;
        }
    }
    pxc_log::write(any ? "INFO" : "WARN", "Single",
                   std::string(any ? "已转发给已有实例，进程 " : "转发失败，进程 ") +
                       std::to_string(g_existingPid) + "，共 " +
                       std::to_string(paths.size()) + " 项");
    return any;
}

bool activateExisting() {
    const std::vector<HWND> windows = findExistingWindows(1500);
    if (windows.empty()) {
        pxc_log::write("WARN", "Single", "要提到前台，但没找到已有窗口");
        return false;
    }
    HWND h = windows.front();
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    if (GetForegroundWindow() == h) return true;

    // SetForegroundWindow 受前台锁限制：先把本线程附到当前前台线程上，调完再解除。
    const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD myThread = GetCurrentThreadId();
    bool ok = false;
    if (fgThread != 0 && fgThread != myThread && AttachThreadInput(myThread, fgThread, TRUE)) {
        ok = SetForegroundWindow(h) != 0;
        AttachThreadInput(myThread, fgThread, FALSE);
    } else {
        ok = SetForegroundWindow(h) != 0;
    }
    if (!ok) {
        ShowWindow(h, SW_SHOW);
        ok = SetForegroundWindow(h) != 0;
    }
    pxc_log::write(ok ? "INFO" : "WARN", "Single", ok ? "已把已有窗口提到前台" : "提到前台失败");
    return ok;
}

} // namespace pxc_single
