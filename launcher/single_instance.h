#pragma once
// 单实例：双击 5 个 .pxc 不该开 5 个启动器窗口。
//
// 三条独立成立的理由（不是"顺便统一一下"）：
//   1. projects.json / launcher.json 都是整文件改写，两个实例同时写必然互相覆盖；
//   2. 双击多个文件会开多个窗口，用户得挨个关；
//   3. 同一个工程被两个 PXC 打开，PXC 自己的 autosave 会互相覆盖 —— 这是用户的文件风险。
//
// 第二实例不建窗：把路径用 WM_COPYDATA 丢给已有实例，然后自己退出。
#include <filesystem>
#include <vector>

namespace pxc_single {

// WM_COPYDATA 的 dwData 标记（写死的四字节，避免和别的程序的消息混）。
constexpr unsigned long kCopyDataMagic = 0x50584331UL;   // 'PXC1'

// true = 本进程是第一个实例（继续建窗并持有互斥体直到退出）。
// false = 已有实例在跑。
bool acquire();

// 把路径转发给已有实例（按"进程 exe 路径与本进程相同"找窗口）。
// 成功（至少一个窗口收下）返回 true。
// 找不到窗口时会重试约 2 秒 —— 已有实例刚启动、窗口还没建出来的那一段（约 1 秒）是真实存在的。
bool notifyExisting(const std::vector<std::filesystem::path>& paths);

// 把已有窗口提到前台（用户只是又点了一次图标，没有文件要转发）。
// 少了这一步，第二实例静默退出的表现就是"点了没反应"，用户会以为程序没启动然后连点。
bool activateExisting();

// 已有实例的进程号（找不到返回 0）。
unsigned long existingPid();

} // namespace pxc_single
