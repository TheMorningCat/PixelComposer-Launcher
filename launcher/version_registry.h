#pragma once
// 版本库：从 exe 读取版本登记、手动登记、按检测版本匹配、删除；v5 起还负责"刷新"与渠道判定。
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "store.h"

namespace pxc_reg {

// 从 exe 读取版本并登记。优先 PE 资源（pxc::readExeVersion）。
// 返回 true 表示自动读出（source=auto）；false 表示读不到（source=manual，需用户补版本）。
// 已存在相同 exe_path 则更新其版本/来源。
bool addExe(std::vector<pxc_store::ExeEntry>& registry, const std::filesystem::path& exePath,
            std::string& outVersion);

// 手动登记（PE 读不到时由用户填写版本）。
void addManual(std::vector<pxc_store::ExeEntry>& registry, const std::filesystem::path& exePath,
               const std::string& version);

// 版本号规范化：把"纯数字 + 点"的串补齐成四段（"1.18.5" -> "1.18.5.0"）。
// 带后缀的（"1.21rc2"）原样返回 —— 猜不出语义就不猜。
//
// 为什么需要：PXC 写进 .pxc 的版本号会**省略尾部的 .0**（1.20.1.0 存成 "1.20.1"），
// 而版本库里登记的是 exe 的完整四段版本。两边按字符串全等比，就会出现
// "本机明明有 1.20.1.0，却弹版本库列表说没有"。
std::string normalizeVersion(const std::string& v);

// 两个版本串是否指同一个版本：数字四段相等即相同（"1.20.1" == "1.20.1.0"）。
bool sameVersion(const std::string& a, const std::string& b);

// 按检测到的产品版本匹配（同名版本可能并列多个）。比较走 sameVersion，不按字符串全等。
std::vector<pxc_store::ExeEntry> findMatching(const std::vector<pxc_store::ExeEntry>& registry,
                                              const std::string& productVersion);

// 删除指定 exe_path 的登记项（samePath 匹配，一次清掉所有同路径重复）。返回删掉的条数。
int removeExe(std::vector<pxc_store::ExeEntry>& registry, const std::string& exePath);

// 当前日期（YYYY-MM-DD），登记时间戳用。
std::string today();

// 两个路径是否指同一个文件：大小写不敏感 + 分隔符归一（Steam 的 vdf 写正斜杠，PE 路径写反斜杠）。
// 串里含 `..` 时再做一次词法归一 —— 老配置存下来的相对路径可能带它，不归一会把同一条判成两条。
bool samePath(const std::string& a, const std::string& b);

// 版本号比较：a>b 返回 1，a<b 返回 -1，相等 0。数字四段优先；猜不出语义的退回字符串比较。
int compareVersion(const std::string& a, const std::string& b);

// ---- v5：刷新（docs/07 §11.3）----

// 一次刷新发现的一处版本变化（Steam 条目上的）。
struct VersionChange {
    std::string exe_path;
    std::string from;   // 用户上次确认过的版本（acked_version）
    std::string to;     // 现在磁盘上的版本
};

struct RefreshResult {
    int steam_added = 0;                    // 新发现的 Steam 条目（含"库里换盘"改路径的那种）
    int reread = 0;                         // 重读 PE 版本成功的条目数
    int channel_filled = 0;                 // 渠道被首次判定 / 改判的条目数
    std::vector<std::string> missing;       // exe 不在的条目（不删，只标记）
    std::vector<VersionChange> changes;     // Steam 版本变化（角标 + 说明，§11.7）
};

// 刷新 = 让登记表追上磁盘事实：
//   ① Steam 重发现（可能新增条目，也可能只是库换了盘 → 改那一条的路径）
//   ② 存在性校验（不在了就记进 missing，绝不删条目）
//   ③ 逐条重读 PE 版本（读不到保留旧值，§11.8）
//   ④ 回填 channel —— 一条规则同时覆盖"自动发现"和"用户手动添加的 Steam exe"
// 只该挂在用户动作上（启动 / 刷新按钮 / 启动工程前），别进帧循环。
RefreshResult refreshAll(pxc_store::AppConfig& cfg);

// ---- 单条刷新（docs/07 §11.3.1）----
// 「添加 exe / 手动登记 / 删除 / 重新读取版本 / 启动工程前」这些场景只关心**一条**的状态，
// 全量刷新会顺带做 Steam 重发现和便携扫描 —— 那不但慢，还会让刚删掉的条目被重新发现回来。
// 单条刷新只做全量的 ②③④（存在性 / 重读 / 渠道），①②一概不碰。
struct EntryRefresh {
    bool found = false;          // 登记表里有这条（changedPath 非空即 true 的等价物）
    std::string changedPath;     // 实际命中的登记路径（大小写/斜杠写法可能与入参不同）
    bool missing = false;        // exe 不在了
    bool channel_filled = false;
    bool reread = false;         // 成功读到了版本（含内嵌数据包探测）
    bool changed = false;        // 版本号真的变了
};
EntryRefresh refreshEntry(pxc_store::AppConfig& cfg, const std::string& exePath);

// 还有哪些 Steam 条目的版本变化没被确认（角标 / 说明弹窗的数据源）。
// 抽出来是因为它必须由**当前状态**推导：单条刷新后没人去维护 refreshAll 里那份列表，
// 两边各算一次必然漂移（角标还在、列表却空了）。
std::vector<VersionChange> pendingChanges(const pxc_store::AppConfig& cfg);

// 便携版自动发现（docs/07 §11.2.6）：扫描启动器目录 + 父级目录（各含一层子目录）下的
// PixelComposer.exe，登记表里没有的就登记（source=auto，channel 留空 → 刷新时按"不在 Steam 库下"
// 归为 itch，与本地手动拖入的 itch 同源）。用途：把启动器和 PXC 摆在一起打包，相对位置不变即有效。
// 返回新增条数。
int discoverPortable(std::vector<pxc_store::ExeEntry>& registry,
                     const std::filesystem::path& launcherDir);

// 这个 exe 是否落在便携扫描范围内（与 discoverPortable 同一套目录集合）。
// 用途：右键菜单据此藏掉「删除」—— 范围内的条目由自动发现托管，删了下次刷新又回来（§11.5 同款口径）。
// 不判文件是否存在：存在性交给调用方，因为「丢失的便携条目」恰恰要允许删（见 app.cpp 的注释）。
bool isPortablePath(const std::string& exePath, const std::filesystem::path& launcherDir);

// exe 属于哪个渠道：落在任一个已知 Steam 库的 steamapps\common 下 = "steam"，否则 "itch"。
std::string detectChannel(const std::filesystem::path& exe);

// 同渠道 + 同版本 + 不同路径的另一条（§11.6.1 情况 A）；没有返回 nullptr。
const pxc_store::ExeEntry* findSameChannelDup(const std::vector<pxc_store::ExeEntry>& registry,
                                             const std::string& exePath, const std::string& version,
                                             const std::string& channel);

// 这个版本是否 itch 与 Steam 各有一张（§11.6.1 情况 B）。
bool hasCrossChannel(const std::vector<pxc_store::ExeEntry>& registry, const std::string& version);

// 同版本多条时挑一条（§11.6.2）：偏好命中的渠道优先 → Steam 优先（地址稳定）→ 第一条。
// 返回下标；matches 为空时返回 0（调用方负责先判空）。
std::size_t choosePreferred(const std::vector<pxc_store::ExeEntry>& matches,
                            const std::map<std::string, std::string>& prefer,
                            const std::string& version);

} // namespace pxc_reg
