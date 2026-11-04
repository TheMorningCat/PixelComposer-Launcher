// 日志实现：路径基于 exe 所在目录（程序相关文件夹），不写 C 盘系统目录。
//
// 只记录「用户操作」与「关键执行逻辑」，不记录周期性 / 每帧事件。
// 两道保险，防止再次出现 11 MB 全是同一行的日志：
//   1. 连续完全相同的条目只落盘一次，后面的事后补一行「重复 N 次」；
//   2. 单个日志文件超过上限就滚成 .1（只保留一份旧档）。
#include "log.h"

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include <windows.h>

static std::mutex g_mutex;
static std::string g_logPath;
// 宽字符版本：**所有实际打开都走它**。
// 原来 emit() 用 fopen_s(g_logPath.c_str())，CRT 按 ANSI(GBK) 解释这段 UTF-8 字节，
// 中文目录（如「PXC 启.动器」）必然打开失败 → 日志静默消失，连崩溃记录都写不下。
static std::wstring g_logPathW;

// 重复抑制状态（连续相同条目）
static std::string g_lastLevel, g_lastTag, g_lastMsg;
static int g_repeat = 0;

static const long long kMaxLogBytes = 8LL * 1024 * 1024;

namespace pxc_log {

static std::string wideToUtf8(const wchar_t* ws) {
    if (!ws) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void init() {
    if (!g_logPath.empty()) return;

    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exePath(buf, n);

    // 取 exe 所在目录
    std::wstring dir = exePath;
    auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);

    std::wstring logDir = dir + L"\\userdata\\logs";
    CreateDirectoryW(logDir.c_str(), nullptr);

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"\\pxc_launcher_%04d-%02d-%02d.log",
               st.wYear, st.wMonth, st.wDay);
    g_logPath = wideToUtf8((logDir + name).c_str());
    g_logPathW = logDir + name;

    // 超限则滚成 .1（覆盖旧的 .1），保证日志不会无限增长
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    std::wstring wpath = g_logPathW;
    if (GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad)) {
        long long sz = ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (sz > kMaxLogBytes) {
            std::wstring bak = wpath + L".1";
            DeleteFileW(bak.c_str());
            MoveFileW(wpath.c_str(), bak.c_str());
        }
    }
}

// 真正落盘
static void emit(const std::string& level, const std::string& tag, const std::string& msg,
                 int repeat) {
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[40];
    sprintf_s(ts, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
              st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    FILE* f = _wfopen(g_logPathW.c_str(), L"ab");
    if (!f) return;
    fprintf(f, "[%s] [%s] [%s] %s\n", ts, level.c_str(), tag.c_str(), msg.c_str());
    if (repeat > 0)
        fprintf(f, "[%s] [%s] [%s] （上一条重复 %d 次，已折叠）\n",
                ts, level.c_str(), tag.c_str(), repeat);
    fclose(f);
}

void write(const char* level, const char* tag, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mutex);
    init();
    if (g_logPath.empty()) return;

    std::string lv = level ? level : "";
    std::string tg = tag ? tag : "";

    if (lv == g_lastLevel && tg == g_lastTag && msg == g_lastMsg) {
        ++g_repeat;          // 连续重复：不落盘，只计数
        return;
    }
    emit(lv, tg, msg, g_repeat);
    g_lastLevel = lv;
    g_lastTag = tg;
    g_lastMsg = msg;
    g_repeat = 0;
}

// 崩溃处理：不拿锁、不分配大对象，尽力写一行就退出。
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    char buf[320];
    const char* what = "未知";
    if (ep && ep->ExceptionRecord) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_ACCESS_VIOLATION) what = "访问冲突(空指针/越界)";
        else if (code == EXCEPTION_STACK_OVERFLOW) what = "栈溢出";
        else if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) what = "整数除零";
        else if (code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED) what = "数组越界";
        sprintf_s(buf, "未捕获异常 code=0x%08X（%s）addr=%p —— 进程即将终止",
                  code, what, ep->ExceptionRecord->ExceptionAddress);
    } else {
        sprintf_s(buf, "未捕获异常（无异常记录）—— 进程即将终止");
    }
    if (!g_logPathW.empty()) {
        FILE* f = _wfopen(g_logPathW.c_str(), L"ab");
        if (f) {
            SYSTEMTIME st; GetLocalTime(&st);
            fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.000] [ERROR] [Crash] %s\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, buf);
            fclose(f);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;  // 直接终止，不再弹系统错误框
}

void installCrashHandler() {
    init();
    SetUnhandledExceptionFilter(crashFilter);
}

} // namespace pxc_log
