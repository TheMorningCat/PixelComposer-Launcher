#pragma once
// PXC 自己的偏好设置（`<用户目录>/Preferences/<构建号>/keys.json`）。
//
// 我们只关心一个键：`"local"` —— 它指向 `<用户目录>/Locale/<名字>` 这个文件夹。
// 这解决了一直悬着的问题：生成语言文件之后，**用户的 PXC 并不会自动切过去**，
// 因为"用哪个语言目录"记在 PXC 自己的偏好里，而不是靠扫目录里有什么。
//
// 注意两个现实约束：
//   1. 文件名是 `local`（不是 locale）。拼错一个字母就静默无效，所以这里集中写一次。
//   2. PXC 退出时会把内存里的整份设置写回这个文件 —— 所以**必须在 PXC 没运行时改**，
//      否则它一关就把我们写的值覆盖掉。调用方负责先判断进程在不在。
#include <filesystem>
#include <string>
#include <vector>

namespace pxc_prefs {

// 一个候选偏好文件（Preferences 下可能有多份，按构建号分目录）。
struct PrefsFile {
    std::filesystem::path path;
    std::string local;   // 该文件里的 "local" 值（空 = 没有这个键）
};

struct LocaleState {
    std::filesystem::path userDir;        // 用户目录（含 Actions/Locale/Preferences 那一层）
    std::filesystem::path prefsFile;      // 实际生效的那份 keys.json（可能为空）
    std::string current;                  // 当前 local 值（可能为空 = 没设置过）
    std::vector<std::string> available;   // Locale/ 下真实存在的语言目录名（排序）
    std::vector<PrefsFile> all;           // 找到的全部 keys.json
    bool readable = false;                // 至少读到一个 keys.json
};

// 探测用户目录下的语言设置现状。userDir 为空或不可读时返回 readable=false。
LocaleState probe(const std::filesystem::path& userDir);

// 把 local 改写成 lang（lang 为空 = 不写）。**保留文件里其它所有字段**。
// PXC 正在运行时请勿调用：它退出时会用内存值覆盖整个文件。
bool writeLocal(const std::filesystem::path& prefsFile, const std::string& lang, std::string& err);

// 判断 PXC 是否在运行（按进程名 PixelComposer.exe 匹配）。写偏好前必须查一次。
bool pxcRunning();

} // namespace pxc_prefs
