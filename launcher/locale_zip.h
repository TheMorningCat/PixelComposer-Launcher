#pragma once
// 最小 zip 读取器：只为从 Steam 版 PXC 的 `pack/locale.zip` 里取出英文骨架（docs/08 §8.1.7）。
//
// 为什么手写而不引第三方：工程已经链了 `ZLIB::ZLIB`（EUI 自带 3rd/zlib-1.3.1），
// 缺的只是「读中央目录 + 找到数据起点」这一层（约 100 行）。不用 Shell.Application
// （COM + 落盘临时文件 + 杀软扫描），也不整包解压（25 条目里 2.7 MB 是 sample locale 的字体）。
//
// 只支持 zip 的两种存储方式：0 = stored、8 = deflate。locale.zip 里全是这两种。
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pxc_zip {

struct Entry {
    std::string name;      // zip 内路径，正斜杠分隔，**大小写敏感**（比较不做折叠）
    uint32_t compSize = 0;
    uint32_t rawSize = 0;
    uint32_t localOffset = 0;
    uint16_t method = 0;   // 0 stored / 8 deflate
};

// 读中央目录。失败时 err 里是一行中文原因（调用方只记日志，不弹窗）。
bool list(const std::filesystem::path& zip, std::vector<Entry>& out, std::string& err);

// 把 zip 里 `prefix`（如 "en/"）下的所有文件解到 destDir（保持相对层级）。
//   includeFonts=false 时跳过 `<prefix>fonts/`（sample locale 的 4 个 ttf，1.86 MB，我们不用）；
//   同时若存在 `<prefix>fonts/fonts.json` 也一并写出（它是槽位表，不是字体本体）。
// 返回写出的文件数（0 视为失败）。
int extractDir(const std::filesystem::path& zip, const std::string& prefix,
               const std::filesystem::path& destDir, bool includeFonts, std::string& err);

// 只取一个条目（用于 `sample locale/fonts/fonts.json` 这种单个槽位表，不整目录解压）。
bool extractOne(const std::filesystem::path& zip, const std::string& name,
                const std::filesystem::path& dst, std::string& err);

} // namespace pxc_zip
