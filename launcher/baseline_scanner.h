#pragma once
// 英文骨架扫描器：决定「目标 PXC 版本需要什么语言文件结构」（docs/08 §8.1）。
//
// 骨架是唯一的结构权威 —— 汉化包翻不全，只有目标版本自带的英文语言文件才能保证 key 与
// **端口顺序**同时正确。PXC 读端口名是按数组下标查的（`locale_data.gml:137`），
// 所以"一份包只能对一个版本正确"，骨架必须来自**用户选定的目标版本**本身。
//
// 候选链（只在目标版本自己的安装目录里找，docs/08 §8.1.3）：
//   1  <安装>/data/locale/en                    itch 安装（明文目录）
//   1b <安装>/data/locale/sample locale         该版本没发 en
//   1c <安装>/pack/locale.zip 里的 en/           Steam 安装（无 data/ 明文）
//   1d 同一 zip 里的 sample locale/             该版本 zip 只带 sample
// 全落空 → 降级链（§8.1.5）：≥目标的最低 → <目标的最高 → 用户目录三份 → 放弃。
//
// **不做多源合并**：以前 `resolve()` 会把后续来源里"手上没有的节点"补进来，等于运行期偷偷做
// 全键并集（实测目标 1.18.5 会从 530 涨到 867）。端口按下标查，混过版本的骨架无处对位，
// 所以只取第一份就返回（§8.1.8.1）。
#include <filesystem>
#include <string>
#include <vector>

#include "store.h"

namespace pxc_base {

struct Baseline {
    std::filesystem::path dir;         // 真正读的目录（Steam 时是 zip 解压落点）
    std::string origin;                // install-en / install-sample / install-zip /
                                       // install-zip-sample / user-en / user-sample / user-missing
    std::string version;               // 版本标签（取不到写 "?"）
    std::filesystem::path sourcePath;  // 展示用：zip 时是 zip 路径，明文时留空

    pxc_store::JsonValue nodes, words, ui, junctions, config;
    bool hasNodes = false, hasWords = false, hasUi = false, hasJunctions = false, hasConfig = false;
    bool hasNotes = false;  // notes/ 目录
    bool hasFonts = false;  // fonts/ 目录（含 fonts.json 作为槽位表）
    std::filesystem::path fontsDir;

    int nodeCount = 0;
    std::string note;  // 人类可读结论（供 UI 与 toast 展示）
};

// 降级来源（用不上目标版本本身）——§8.1.5 情况 A，toast 行首要加 ⚠
bool isDegradedOrigin(const std::string& origin);

struct Candidate {
    std::filesystem::path dir;
    std::string origin;
    std::string version;
    std::filesystem::path source;  // zip 路径或明文目录（展示用）
    int nodeCount = -1;            // -1 = 未读
};

// 已装版本（「目标版本」下拉的一行）。同一版本多渠道合并成一行（itch 优先）。
struct InstalledVersion {
    std::string version;   // 四段
    std::string exePath;
    std::filesystem::path installDir;
    std::string channel;   // itch / steam / ""
    bool hasSkeleton = false;
    std::string origin;
    int nodeCount = -1;
};

// 已装版本清单（版本降序）。withCounts=true 会读/解一次骨架来数节点 —— 只该挂在
// 用户动作或首次进页上，**绝不能进帧循环**。
std::vector<InstalledVersion> scanInstalled(const std::vector<pxc_store::ExeEntry>& registry,
                                            const std::filesystem::path& cacheRoot,
                                            bool withCounts);

// 某个版本自己的候选链（只含真实存在的项，按优先级）。
std::vector<Candidate> candidatesFor(const std::filesystem::path& installDir,
                                     const std::string& version,
                                     const std::filesystem::path& cacheRoot);

// 加载指定候选（含目录内 notes/fonts 探测）。
bool load(const Candidate& c, Baseline& out);

struct Request {
    std::vector<pxc_store::ExeEntry> registry;
    std::filesystem::path userDir;
    std::filesystem::path cacheRoot;   // Steam zip 解压的缓存根（启动器目录下 skeleton/）
    std::string targetVersion;         // 空 = 用已装版本里最高的
};

// 解析出骨架（主链 → 降级链）。找不到时返回空 Baseline，note 里写明原因。
Baseline resolve(const Request& r);

// 自动生成用：只认这个 exe 自己的版本；它取不到就走降级链。
Baseline resolveForExe(const std::filesystem::path& exe, const std::string& version,
                       const std::vector<pxc_store::ExeEntry>& registry,
                       const std::filesystem::path& userDir,
                       const std::filesystem::path& cacheRoot);

// 自动定位 PXC 用户目录（判据放宽到「目录自身存在」，docs/08 §8.7.2）。
// 返回候选列表（优先级从高到低），第一个即采用值。
std::vector<std::filesystem::path> findUserDirs();

// 目录看起来是否像 PXC 用户目录（有 Locale/ 或 Preferences/）。§8.7.2 的 weak 标记用。
bool looksLikeUserDir(const std::filesystem::path& dir);

// 从 exe 路径推安装目录（父目录）。
std::filesystem::path installDirOfExe(const std::filesystem::path& exe);

} // namespace pxc_base
