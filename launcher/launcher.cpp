// 启动器实现（Windows CreateProcessW，宽路径，规避 ANSI 编码坑）。
#include "launcher.h"
#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace pxc_launch {
namespace {

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// 还原矩形：最小化时 GetClientRect 是 0×0、GetWindowRect 只有标题栏那条，都不作数；
// GetWindowPlacement 的 rcNormalPosition 是"还原后会是多大"，最小化时依然有效。
//
// 但**两个来源都可能失真**，所以取面积大的那个：
//   · 最小化：GetWindowRect 只剩图标那条（160×28 量级），rcNormalPosition 才是真的；
//   · 最大化的窗口被"先最小化再还原"揉过之后，rcNormalPosition 可能被写成图标尺寸
//     （实测 2026-11-04：窗口明明 1920×1080 最大化着，rcNormalPosition = {0,0,160,28}），
//     这时只有 GetWindowRect 是真的。
// 以前只看 rcNormalPosition，后一种情况会让**一个正在运行的 PXC 被判成"主窗还没出来"**：
// 复用报 Busy（"还在启动中"）、首启干等 60 秒超时，日志里看不出任何异常。
bool normalSize(HWND h, LONG& w, LONG& hh) {
    LONG pw = 0, ph = 0, rw = 0, rh = 0;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(h, &wp)) {
        pw = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        ph = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        if (pw <= 0 || ph <= 0) pw = ph = 0;
    }
    RECT r{};
    if (GetWindowRect(h, &r)) {          // 普通/最大化态用这个
        rw = r.right - r.left;
        rh = r.bottom - r.top;
        if (rw <= 0 || rh <= 0) rw = rh = 0;
    }
    if ((long long)pw * ph >= (long long)rw * rh) {
        w = pw;
        hh = ph;
    } else {
        w = rw;
        hh = rh;
    }
    return w > 0 && hh > 0;
}

struct WinCtx {
    DWORD pid = 0;
    HWND best = nullptr;
    LONG bestArea = 0;
};

BOOL CALLBACK enumWinProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<WinCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER) != nullptr) return TRUE;
    LONG w = 0, hh = 0;
    if (!normalSize(h, w, hh)) return TRUE;
    if (w <= 400) return TRUE;             // 跳过启动画面 / 小工具窗
    const LONG area = w * hh;
    if (area > c->bestArea) {
        c->bestArea = area;
        c->best = h;
    }
    return TRUE;
}

} // namespace

SpawnResult launchExe(const std::filesystem::path& exePath, const std::filesystem::path& pxcPath) {
    SpawnResult out;
    if (!std::filesystem::exists(exePath)) {
        out.detail = "PXC 程序不存在";
        out.error = ERROR_FILE_NOT_FOUND;
        pxc_log::write("ERROR", "Launch", "exe 不存在: " + exePath.u8string());
        return out;
    }
    // 命令行需含可执行文件自身（带引号），参数为带引号的工程文件路径。
    std::wstring cmd = L"\"" + exePath.wstring() + L"\"";
    if (!pxcPath.empty()) cmd += L" \"" + pxcPath.wstring() + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CreateProcessW 会修改命令行缓冲区，故复制一份可写副本。
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    pxc_log::write("INFO", "Launch", "启动命令: " + exePath.u8string() +
                   (pxcPath.empty() ? "" : " 工程=" + pxcPath.u8string()));

    // cwd 必须显式钉在 PXC 安装目录：传 nullptr 会让子进程继承启动器自己的 cwd，
    // 而启动器可能被 .pxc 关联从工程目录拉起（Explorer 把 cwd 设为文件所在目录）→
    // PXC 的 working_directory 变成工程目录，而它所有库路径默认是 %APP%/<lib>/，
    // 于是「No FFmpeg/gifski/webp/ImageMagick detected」四条误报，导出与非 8bit 导入全废。
    const std::wstring wd = exePath.parent_path().wstring();

    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, wd.c_str(), &si, &pi)) {
        const DWORD err = GetLastError();
        char hex[32] = {0};
        std::snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)err);
        out.error = err;
        out.detail = std::string("无法启动 PXC（错误码 ") + hex + "）";
        pxc_log::write("ERROR", "Launch", "CreateProcessW 失败，错误码=" + std::string(hex));
        return out;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    out.ok = true;
    out.pid = (unsigned long)pi.dwProcessId;
    pxc_log::write("INFO", "Launch", "已拉起进程 (PID=" + std::to_string(out.pid) + ")");
    return out;
}

void* findMainWindowOfPid(unsigned long pid) {
    if (pid == 0) return nullptr;
    WinCtx c;
    c.pid = (DWORD)pid;
    EnumWindows(enumWinProc, reinterpret_cast<LPARAM>(&c));
    return c.best;
}

bool processAlive(unsigned long pid, unsigned long* exitCode) {
    if (exitCode) *exitCode = STILL_ACTIVE;
    if (pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return false;
    DWORD code = STILL_ACTIVE;
    const bool got = GetExitCodeProcess(h, &code) != 0;
    CloseHandle(h);
    if (!got) return false;
    if (exitCode) *exitCode = (unsigned long)code;
    return code == STILL_ACTIVE;
}

bool waitMainWindow(unsigned long pid, int timeoutMs,
                    const std::function<void(int)>& onTick,
                    const std::function<bool()>& cancelled,
                    bool* wasCancelled, unsigned long* exitCode) {
    if (wasCancelled) *wasCancelled = false;
    if (exitCode) *exitCode = STILL_ACTIVE;
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        if (cancelled && cancelled()) {
            if (wasCancelled) *wasCancelled = true;
            return false;
        }
        if (findMainWindowOfPid(pid) != nullptr) return true;

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        if (onTick) onTick((int)(elapsed / 1000));
        if (elapsed >= timeoutMs) return false;

        // 进程已经退出就没什么好等的了 —— 别耗满 60 秒才报失败。
        unsigned long code = 0;
        if (!processAlive(pid, &code)) {
            if (exitCode) *exitCode = code;
            pxc_log::write("WARN", "Launch",
                           "PXC 进程已退出，退出码=" + std::to_string(code));
            // 退出前最后一瞬可能刚好建好窗口，再探一次再下结论。
            if (findMainWindowOfPid(pid) != nullptr) return true;
            return false;
        }
        sleepMs(250);
    }
}

} // namespace pxc_launch
