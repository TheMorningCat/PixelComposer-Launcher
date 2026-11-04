#pragma once
// 汉化管理：定位语言目录、递归部署到安装目录、字体列表、覆盖层合并、字体替换。
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace pxc_locale {

// 在 sourceDir 中定位目标语言目录。sourceDir 可能是：
//   - 语言目录本身（含 nodes.json/words.json），直接返回；
//   - 汉化包根目录（含 zh/ce-zh/ec-zh 子目录），返回 <sourceDir>/<mode>。
// 若 mode 不存在，依次回退 zh -> 首个含 locale 文件的子目录。找不到返回空路径。
std::filesystem::path locateLangDir(const std::filesystem::path& sourceDir, const std::string& mode);

// 递归拷贝 langDir 到 <installDir>/data/locale/<mode>。成功返回 true。
bool deploy(const std::filesystem::path& langDir, const std::filesystem::path& installDir,
            const std::string& mode);

// 列出 langDir/fonts 下的字体文件名（含扩展名）。
std::vector<std::string> listFonts(const std::filesystem::path& langDir);

// 把覆盖层合并进 words.json：仅替换已存在的键；解析失败则跳过并告警。best-effort。
bool applyOverrides(const std::filesystem::path& wordsJsonPath,
                    const std::map<std::string, std::string>& overrides);

// 把 fonts.json 中所有槽位的 path 替换为 "./<ttfName>"。best-effort。
bool setFont(const std::filesystem::path& fontsJsonPath, const std::string& ttfName);

// 递归复制目录到 to（覆盖已存在文件）。内部走 Win32 + 扩展长度路径，长路径安全。
// 用于把模板包的 fonts/notes 等「非翻译文件」带到生成的语言目录。
bool copyDir(const std::filesystem::path& from, const std::filesystem::path& to);

} // namespace pxc_locale
