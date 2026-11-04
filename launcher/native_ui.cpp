// Windows 原生窗口集成：拖放 + IFileDialog。
// 本文件不含 EUI 头文件，可自由包含 windows.h。
#include "native_ui.h"
#include "log.h"
#include "util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <cstring>

namespace pxc_nui {
namespace {

HWND g_hwnd = nullptr;
WNDPROC g_oldProc = nullptr;
bool g_dropInstalled = false;
std::function<void(std::vector<std::filesystem::path>)> g_onDrop;
std::function<void(std::vector<std::filesystem::path>)> g_onCopyData;
std::function<void()> g_onLaunchDone;
unsigned int g_launchDoneMsg = 0;
bool g_iconApplied = false;

// COM 初始化作用域（只在本次调用负责初始化时才 CoUninitialize）。
struct ComScope {
    bool owner = false;
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        owner = SUCCEEDED(hr);
    }
    ~ComScope() { if (owner) CoUninitialize(); }
};

BOOL CALLBACK enumThreadWindow(HWND h, LPARAM) {
    if (GetWindow(h, GW_OWNER) != nullptr) return TRUE; // 跳过 owned 工具窗口
    wchar_t cls[256] = {0};
    if (!GetClassNameW(h, cls, 256)) return TRUE;
    // 类名优先、且**不看可见性**：静默启动时窗口建出来就是不可见的（GLFW_VISIBLE=false，
    // 为了不闪），如果这里还要求 IsWindowVisible，就会找不到自己的主窗 ——
    // 表现是"最小化/隐藏都失败"，而且失败得很安静。
    if (std::wcscmp(cls, L"GLFW30") == 0) { g_hwnd = h; return FALSE; } // 命中即停
    if (IsWindowVisible(h) && !g_hwnd) g_hwnd = h;                       // 兜底：首个顶层可见窗
    return TRUE;
}

// 定位本进程主线程上的顶层可见窗口（GLFW 主窗口）。
HWND findMainWindow() {
    if (g_hwnd && IsWindow(g_hwnd)) return g_hwnd;
    g_hwnd = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), enumThreadWindow, 0);
    return g_hwnd;
}

std::vector<std::filesystem::path> pathsFromDrop(HDROP hd) {
    std::vector<std::filesystem::path> out;
    UINT count = DragQueryFileW(hd, 0xFFFFFFFFu, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        UINT need = DragQueryFileW(hd, i, nullptr, 0);
        if (need == 0) continue;
        std::wstring buf((size_t)need + 1, L'\0');
        if (DragQueryFileW(hd, i, buf.data(), need + 1) == 0) continue;
        buf.resize(std::wcslen(buf.c_str()));
        if (!buf.empty()) out.emplace_back(buf); // 宽路径，避免 GBK 解码
    }
    return out;
}

LRESULT CALLBACK dropWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (g_onLaunchDone && g_launchDoneMsg != 0 && msg == g_launchDoneMsg) {
        g_onLaunchDone();
        return 0;
    }
    if (msg == WM_DROPFILES) {
        HDROP hd = reinterpret_cast<HDROP>(w);
        std::vector<std::filesystem::path> paths = pathsFromDrop(hd);
        DragFinish(hd);
        pxc_log::write("INFO", "Drop",
                       "收到拖放 " + std::to_string(paths.size()) + " 项" +
                       (paths.empty() ? std::string() : ("，首项: " + paths[0].u8string())));
        if (g_onDrop && !paths.empty()) g_onDrop(std::move(paths));
        return 0;
    }
    if (msg == WM_COPYDATA) {
        // 单实例转发：第二个实例把路径丢过来就退出，这里只负责把路径交给应用层。
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(l);
        if (!cds || cds->dwData != 0x50584331UL || !cds->lpData || cds->cbData == 0) return 0;
        const std::string payload((const char*)cds->lpData, (size_t)cds->cbData);
        std::vector<std::filesystem::path> paths;
        size_t a = 0;
        while (a < payload.size()) {
            size_t b = payload.find('\n', a);
            if (b == std::string::npos) b = payload.size();
            if (b > a) paths.push_back(pxc_util::pathFromUtf8(payload.substr(a, b - a)));
            a = b + 1;
        }
        pxc_log::write("INFO", "Single",
                       "收到第二实例转发 " + std::to_string(paths.size()) + " 项");
        if (g_onCopyData && !paths.empty()) g_onCopyData(std::move(paths));
        return 1;   // 非 0 = 已处理（发送方据此判断转发成功）
    }
    if (msg == WM_NCDESTROY) {
        // 窗口销毁：还原原过程，避免回调到已释放的窗口
        if (g_oldProc) {
            WNDPROC old = g_oldProc;
            g_oldProc = nullptr;
            g_dropInstalled = false;
            SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(old));
        }
    }
    if (g_oldProc) return CallWindowProcW(g_oldProc, h, msg, w, l);
    return DefWindowProcW(h, msg, w, l);
}

// 统一的文件对话框骨架（mode: folders=选文件夹, save=另存为, 其余=打开文件）。
std::filesystem::path commonDialog(const wchar_t* title,
                                   const std::vector<std::pair<std::wstring, std::wstring>>& filters,
                                   bool folders, bool save = false,
                                   const std::wstring& defaultName = L"",
                                   const wchar_t* defaultExt = nullptr) {
    ComScope com;
    HWND owner = findMainWindow();
    IFileDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) {
        pxc_log::write("ERROR", "Pick", "IFileDialog 创建失败: 0x" + std::to_string((unsigned long)hr));
        return {};
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM;
    if (folders) opts |= FOS_PICKFOLDERS;
    else if (save) opts |= FOS_OVERWRITEPROMPT;
    else opts |= FOS_FILEMUSTEXIST;
    dlg->SetOptions(opts);
    if (title) dlg->SetTitle(title);
    if (save && !defaultName.empty()) dlg->SetFileName(defaultName.c_str());

    std::vector<COMDLG_FILTERSPEC> spec;
    if (!folders && !filters.empty()) {
        spec.reserve(filters.size());
        for (const auto& f : filters) spec.push_back({f.first.c_str(), f.second.c_str()});
        dlg->SetFileTypes((UINT)spec.size(), spec.data());
        dlg->SetFileTypeIndex(1);
    }
    if (save && defaultExt) dlg->SetDefaultExtension(defaultExt);

    std::filesystem::path out;
    hr = dlg->Show(owner);
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR psz = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
                out = std::filesystem::path(psz); // 宽路径
                CoTaskMemFree(psz);
            }
            item->Release();
        }
    } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        pxc_log::write("ERROR", "Pick", "对话框失败: 0x" + std::to_string((unsigned long)hr));
    }
    dlg->Release();
    return out;
}

} // namespace

// 子类化只做一次：拖放与单实例转发共用同一个窗口过程，
// 谁先来谁装，后来者只更新自己的回调（不能因为"已装过"就把回调丢掉）。
static bool ensureSubclass() {
    if (g_dropInstalled) return true;
    HWND h = findMainWindow();
    if (!h) return false; // 窗口尚未创建：调用方（后续帧）会重试
    DragAcceptFiles(h, TRUE);
    // 允许来自较低完整性级别进程（如资源管理器）的拖放消息，否则以管理员运行时拖放会被 UIPI 拦截。
    ChangeWindowMessageFilterEx(h, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(h, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    g_oldProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(dropWndProc)));
    g_dropInstalled = true;
    pxc_log::write("INFO", "Native", "已注册窗口消息（拖放 + 单实例转发，HWND=" +
                                         std::to_string((size_t)h) + "）");
    return true;
}

bool installDrop(std::function<void(std::vector<std::filesystem::path>)> onDrop) {
    g_onDrop = std::move(onDrop);
    return ensureSubclass();
}

bool installCopyData(std::function<void(std::vector<std::filesystem::path>)> onCopyData) {
    g_onCopyData = std::move(onCopyData);
    return ensureSubclass();
}

bool installLaunchDone(unsigned int msg, std::function<void()> onDone) {
    g_launchDoneMsg = msg;
    g_onLaunchDone = std::move(onDone);
    return ensureSubclass();
}

void* mainWindow() { return (void*)findMainWindow(); }

bool applyWindowIcon() {
    HWND h = findMainWindow();
    if (!h) return false;
    if (g_iconApplied) return true;
    // 资源名见 launcher/res/app.rc。GLFW 建窗类时已经按 LR_DEFAULTSIZE(32) 取了一份当**类图标**，
    // 但 WNDCLASSEX.hIconSm 是空的 —— 标题栏 / Alt+Tab 的 16px 图标于是由 32 那张硬缩，
    // 边缘必然糊。这里按当前 DPI 的小图标尺寸重新取一次，正好命中 ico 里手调的 16px 帧。
    HINSTANCE inst = GetModuleHandleW(nullptr);
    // 注意别把变量叫 small —— rpcndr.h 里 `#define small char`，`HICON small` 会被展开成
    // `HICON char` 然后报一堆语法错（踩过）。
    HICON hBig = (HICON)LoadImageW(inst, L"GLFW_ICON", IMAGE_ICON,
                                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON hSmall = (HICON)LoadImageW(inst, L"GLFW_ICON", IMAGE_ICON,
                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (hBig) SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)hBig);
    if (hSmall) SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
    if (!hBig && !hSmall) {
        // 没资源（例如有人删了 res/app.rc）不是错误：窗口就是系统的默认图标，别刷日志。
        return false;
    }
    g_iconApplied = true;   // 两张 HICON 随进程存活，不 DestroyIcon，也别每帧重设
    pxc_log::write("INFO", "Native", std::string("已设置窗口图标（") +
                                     (hBig ? "big" : "-") + "/" + (hSmall ? "small" : "-") + "）");
    return true;
}

void postToMainWindow(unsigned int msg) {
    if (HWND h = findMainWindow()) PostMessageW(h, msg, 0, 0);
}

bool hideMainWindow() {
    HWND h = findMainWindow();
    if (!h) return false;
    ShowWindow(h, SW_HIDE);
    return true;
}

bool minimizeToTaskbar() {
    HWND h = findMainWindow();
    if (!h) return false;
    // SW_SHOWMINNOACTIVE：显示到任务栏但不激活。用户点任务栏按钮才恢复 —— 标准行为。
    ShowWindow(h, SW_SHOWMINNOACTIVE);
    return true;
}

// 把窗口顶到前台。
//
// 单独调 `SetForegroundWindow` 是不够的：Windows 有"前台锁" —— 调用者不是当前前台
// 进程/线程时，这一句会被静默拒绝（没有错误码，窗口就是不上来）。典型场景正是静默启动：
// 用户双击 `.pxc`，启动器由 Explorer 拉起（或转发给已在运行的实例，而那个实例此刻
// 多半在后台），于是"缺版本、要选版本"的协商窗弹出来时窗口还在后面 —— 用户根本
// 不知道有个必须处理的东西（2026-11-04 用户反馈）。
//
// 解法是老办法：`AttachThreadInput` 把自己挂到"当前前台线程"和"目标线程"的输入队列上，
// 借它们的输入状态把窗口顶上来，完事再摘掉。顶不上就闪任务栏 —— 至少给个信号。
static bool forceForeground(HWND h) {
    if (!h || !IsWindow(h)) return false;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    ShowWindow(h, SW_SHOW);
    if (GetForegroundWindow() == h) return true;

    const DWORD cur = GetCurrentThreadId();
    const HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD tgtThread = GetWindowThreadProcessId(h, nullptr);
    const bool a1 =
        (fgThread && fgThread != cur) ? AttachThreadInput(cur, fgThread, TRUE) != 0 : false;
    const bool a2 =
        (tgtThread && tgtThread != cur) ? AttachThreadInput(cur, tgtThread, TRUE) != 0 : false;

    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {   // 前台锁偶尔要连试
        BringWindowToTop(h);
        SetForegroundWindow(h);
        SetFocus(h);
        ok = (GetForegroundWindow() == h);
        if (!ok) Sleep(60);
    }
    if (a1) AttachThreadInput(cur, fgThread, FALSE);
    if (a2) AttachThreadInput(cur, tgtThread, FALSE);

    if (!ok) {
        // 还是抢不到（前台是全屏程序 / UAC 提示 / 用户正在别的虚拟桌面）：
        // 闪任务栏，别一声不响地留在后台。
        FLASHWINFO fi{};
        fi.cbSize = sizeof(fi);
        fi.hwnd = h;
        fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
        fi.uCount = 3;
        FlashWindowEx(&fi);
        pxc_log::write("WARN", "UI", "窗口抢不到前台，已改为闪烁任务栏");
    }
    return ok;
}

bool showMainWindow() {
    HWND h = findMainWindow();
    if (!h) return false;
    return forceForeground(h);
}

bool getMainWindowRect(int& x, int& y, int& w, int& h) {
    HWND hwnd = findMainWindow();
    if (!hwnd) return false;
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return false;
    x = r.left; y = r.top;
    w = r.right - r.left;
    h = r.bottom - r.top;
    return true;
}

bool setMainWindowRect(int x, int y, int w, int h) {
    HWND hwnd = findMainWindow();
    if (!hwnd || w <= 0 || h <= 0) return false;
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

bool setMainWindowClientSize(int cw, int ch, int& outW, int& outH) {
    HWND hwnd = findMainWindow();
    if (!hwnd || cw <= 0 || ch <= 0) return false;

    // 最大化时 SetWindowPos 只是挪位置、尺寸不生效；最小化时改尺寸会被忽略。
    // SW_RESTORE 是投递给目标线程的异步动作 —— 调完立刻查还是旧状态，必须轮询（同 pxc_reuse 的坑）。
    if (IsZoomed(hwnd) || IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
        const ULONGLONG t0 = GetTickCount64();
        while ((IsZoomed(hwnd) || IsIconic(hwnd)) && GetTickCount64() - t0 < 1500) Sleep(20);
    }

    // 客户区 -> 外框：SetWindowPos 的宽高是外框尺寸，两者差一个标题栏/边框。
    RECT r{0, 0, cw, ch};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&r, style, FALSE, exStyle);
    const int ow = r.right - r.left;
    const int oh = r.bottom - r.top;

    // 夹到当前显示器的工作区（已扣任务栏）：窗口比屏幕大 = 用户想恢复也点不到右下角。
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    int waX = 0, waY = 0, waW = ow, waH = oh;
    if (mon && GetMonitorInfoW(mon, &mi)) {
        waX = mi.rcWork.left; waY = mi.rcWork.top;
        waW = mi.rcWork.right - mi.rcWork.left;
        waH = mi.rcWork.bottom - mi.rcWork.top;
    }
    const int fw = ow < waW ? ow : waW;
    const int fh = oh < waH ? oh : waH;

    // 居中：从 2080 宽缩回 1040 时保留左上角会贴在屏幕角落，两个方向居中才稳。
    const int fx = waX + (waW - fw) / 2;
    const int fy = waY + (waH - fh) / 2;
    SetWindowPos(hwnd, nullptr, fx, fy, fw, fh, SWP_NOZORDER | SWP_NOACTIVATE);

    RECT cr{};
    GetClientRect(hwnd, &cr);
    outW = cr.right - cr.left;
    outH = cr.bottom - cr.top;
    return true;
}

bool revealInExplorer(const std::filesystem::path& path, bool selectFile) {
    if (path.empty()) return false;
    std::error_code ec;
    std::wstring args;
    if (selectFile && std::filesystem::exists(path, ec))
        args = L"/select,\"" + path.wstring() + L"\"";   // 选中该文件
    else {
        // 打开所在文件夹：文件取父目录，文件夹直接用自己
        const std::filesystem::path dir =
            std::filesystem::is_directory(path, ec) ? path : path.parent_path();
        if (dir.empty()) return false;
        args = L"\"" + dir.wstring() + L"\"";
    }
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                                SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

std::filesystem::path userDataDir() {
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;

    // 自己取 exe 所在目录（不依赖别处的 exeDir()）。
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir(buf, n);
    auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);

    auto tryDir = [&](const std::wstring& d) -> bool {
        std::error_code e;
        std::filesystem::create_directories(d, e);
        if (e) return false;
        const std::wstring probe = d + L"\\.write_test";
        FILE* fp = _wfopen(probe.c_str(), L"wb");
        if (!fp) return false;
        fclose(fp);
        std::filesystem::remove(probe, e);
        return true;
    };

    // 默认：exe 同级的 userdata/。写不进（Program Files 等）才退 LOCALAPPDATA。
    std::wstring p = dir + L"\\userdata";
    if (!tryDir(p)) {
        wchar_t la[512] = {0};
        DWORD m = GetEnvironmentVariableW(L"LOCALAPPDATA", la, 512);
        if (m > 0 && m < 512)
            p = std::wstring(la, m) + L"\\PixelComposer-Launcher";
    }
    cached = p;
    return cached;
}

std::filesystem::path pickFolder(const wchar_t* title) {
    return commonDialog(title, {}, true);
}

std::filesystem::path pickFile(const wchar_t* title,
                               const std::vector<std::pair<std::wstring, std::wstring>>& filters) {
    return commonDialog(title, filters, false);
}

std::filesystem::path pickSave(const wchar_t* title, const std::wstring& defaultName,
                               const std::vector<std::pair<std::wstring, std::wstring>>& filters,
                               const wchar_t* defaultExt) {
    return commonDialog(title, filters, false, true, defaultName, defaultExt);
}

} // namespace pxc_nui
