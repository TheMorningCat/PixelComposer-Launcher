#pragma once
// 启动器：拉起对应 PXC exe 并传入工程文件路径；以及"等它的主窗出来"。
//
// 等窗口这件事以前没有：`launchExe()` 只 `CreateProcessW` 就返回，连 pid 都丢掉了，
// 所以"PXC 启动成功"从来没被真正观测过（只观测到"进程被拉起了"）。
// docs/05 §9.3.1 要求首启的成功判据是"PXC 主窗出现"，所以这里补上 pid 与轮询。
#include <filesystem>
#include <functional>
#include <string>

namespace pxc_launch {

struct SpawnResult {
    bool ok = false;
    unsigned long pid = 0;
    unsigned long error = 0;   // CreateProcessW 失败时的 GetLastError()
    std::string detail;        // 一行中文说明（写日志 + 提示窗）
};

// 以 "<exe>" "<pxc>" 形式启动 PXC；成功返回 pid。
SpawnResult launchExe(const std::filesystem::path& exePath, const std::filesystem::path& pxcPath);

// 找该 pid 的主窗：可见、无 owner、还原矩形宽 > 400（跳过启动画面）、面积最大者胜。
// 用 `GetWindowPlacement().rcNormalPosition` 而不是 `GetClientRect` ——
// 最小化时客户区是 0×0，只有前者还留着 1056×759（2026-11-01 踩过）。
// 未找到返回 nullptr。
void* findMainWindowOfPid(unsigned long pid);

// 等主窗出现，250 ms 探一次，最长 timeoutMs。
// onTick 每次探测前回调（参数是已等秒数），提示窗靠它刷新"已 N 秒"；可为空。
// cancelled 返回 true 时立刻结束，*wasCancelled 置 true（可为空指针）。
/// @param exitCode 输出：若因进程退出而返回 false，这里是真实退出码；
///                 超时/取消时为 STILL_ACTIVE，调用方据此区分"卡住"和"秒退"。
bool waitMainWindow(unsigned long pid, int timeoutMs,
                    const std::function<void(int)>& onTick,
                    const std::function<bool()>& cancelled,
                    bool* wasCancelled,
                    unsigned long* exitCode = nullptr);

// 进程是否还活着。活着时 exitCode = STILL_ACTIVE；已退出时给真实退出码。
// pid 无效 / 打不开进程也返回 false（对调用方来说等价于"没了"）。
bool processAlive(unsigned long pid, unsigned long* exitCode);

} // namespace pxc_launch
