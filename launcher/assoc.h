#pragma once
// `.pxc` 文件关联（只写 HKCU）。
//
// 为什么不抢默认程序：Win8+ 的 UserChoice 带哈希（扩展名 + ProgId + 用户 SID + 时间戳），
// 程序自己写会被系统判为无效并重置；网上"算哈希"的做法依赖未公开算法、随更新失效，
// 而且是在绕过系统的默认程序保护。所以这里只做两件事：
//   1. 把 ProgId 写进 .pxc 的 OpenWithProgids —— 保证"打开方式"里能看到启动器；
//   2. 引导用户点一次「始终使用」—— 那一步由系统自己写 UserChoice 哈希，永久有效。
// 代价是"出现在候选里"能保证，"自动成为默认"不能保证；比一个随时被重置的假成功好。
#include <filesystem>
#include <string>

namespace pxc_assoc {

enum class State {
    None,    // 未关联
    Ours,    // 已关联（默认值是我们的 ProgId）
    Other,   // 被其它程序占用（UserChoice 指向别的 ProgId）
};

struct Info {
    State state = State::None;
    std::string progId;        // .pxc 默认值
    std::string userChoice;    // UserChoice\ProgId（可能为空）
    std::string otherName;     // 占用方的友好名（读不到则为 ProgId）
};

// 我们的 ProgId 固定，避免多版本安装互相打架。
constexpr const char* kProgId = "PXCLauncher.pxc";

Info query();
// 注册：写 HKCU\Software\Classes\.pxc（默认值 + OpenWithProgids）与 ProgId 子树。
// previousProgId 回传"原来占着默认值的 ProgId"（用于取消关联时还原），失败时 err 说明原因。
bool registerSelf(const std::filesystem::path& launcherExe, std::string& previousProgId,
                  std::string& err);
// 取消：仅当默认值仍是我们时才动 .pxc，删 ProgId 子树；previousProgId 非空则写回。
bool unregisterSelf(const std::string& previousProgId, std::string& err);

// 打开 Windows 的「默认应用」设置页，让用户自己点一次「始终使用」。
bool openDefaultAppsSettings();

} // namespace pxc_assoc
