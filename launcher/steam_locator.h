#pragma once
// Steam 版 PXC 定位（docs/07 §11.2）：注册表 -> libraryfolders.vdf（多库）-> appmanifest -> exe。
// 只读，不动 Steam 任何文件。找不到就返回空 —— 调用方据此认为"这台机器没有 Steam 版"。
//
// 为什么不扫 steamapps/common 猜目录名：那是几百个目录的猜测，遇到重命名/多语言/同名 demo 就错。
// 库的位置能精确问出来（vdf），目录名能精确读出来（acf 的 installdir），两级都是权威来源。
#include <filesystem>
#include <string>
#include <vector>

namespace pxc_steam {

struct Hit {
    std::filesystem::path exe;      // <lib>/steamapps/common/<installdir>/PixelComposer.exe
    std::filesystem::path library;  // 库根
    std::string installdir;         // acf 里的目录名；空 = 没有 acf、靠固定名探测到的
    unsigned state_flags = 0;       // acf 的 StateFlags（4 = 完整安装）；0 = 无 acf
    long long last_updated = 0;     // acf 的 LastUpdated（unix 秒）；0 = 无
};

// 从 vdf 文本里取出所有 "path" 值（纯函数，便于单测）。
// 必须处理转义：Steam 写的是 `"path"  "E:\\steam"`，用引号切分会把反斜杠留在值里。
std::vector<std::string> parseLibraryPaths(const std::string& vdf);

// 从 acf（VDF 格式）里取一个标量值；没有返回空串。**不用转义** —— acf 里的路径是单反斜杠。
std::string acfValue(const std::string& text, const char* key);

// Steam 根目录：HKCU SteamPath -> SteamExe 的父目录 -> HKLM InstallPath。空 = 没装 Steam。
std::filesystem::path steamRoot();

// 本机所有 Steam 库（含 Steam 根自身），规范化后按大小写不敏感去重。
// 顺序 = vdf 里的声明顺序；主库排在最前。
std::vector<std::filesystem::path> libraries();

// 找 PXC。正常返回 1 个；返回多个 = 用户在不同库里留了副本。没有则空。
std::vector<Hit> find();

// exe 是否落在该库的 steamapps\common 下（判渠道用，大小写不敏感）。
bool isUnderCommon(const std::filesystem::path& exe, const std::filesystem::path& library);

// 该 exe 落在任一个库里就返回那个库，否则返回空。刷新时用它回填 channel。
std::filesystem::path libraryOf(const std::filesystem::path& exe,
                                const std::vector<std::filesystem::path>& libs);

} // namespace pxc_steam
