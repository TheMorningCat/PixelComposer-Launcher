// 版本库实现。
#include "version_registry.h"
#include "pxc_detect.h"   // pxc::readExeVersion
#include "steam_locator.h"
#include "log.h"
#include "version_probe.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cwctype>

namespace pxc_reg {

namespace {
// 版本号的最终裁定：PE 资源读到的值，除非内嵌数据包给出了**同主次版本且更大**的值。
//
// 为什么不能直接用探测值：探测是字节扫描，它只保证"这个串长得像版本号、主次版本对得上"，
// 不保证它就是产品版本。所以规则收得很紧 —— 只在"更大"时采纳（1.21.0.0 → 1.21.0.2），
// 绝不因为扫到个小值就把版本号改小。版本号是骨架目录的键，改错比不准更糟（§11.8.2）。
// 末段是 0 才值得怀疑：PXC 打包时把 patch 段写丢，结果就是 "x.y.z.0"（1.21.0.2 → 1.21.0.0）。
// 末段非 0 的那几版（1.19.0.11 / 1.22.0.5）PE 值都对，去扫它们是白拿几十 MB 的磁盘读换零收益。
bool lastSegIsZero(const std::string& v) {
    const std::size_t dot = v.rfind('.');
    if (dot == std::string::npos || dot + 1 >= v.size()) return false;
    for (std::size_t i = dot + 1; i < v.size(); ++i)
        if (v[i] != '0') return false;
    return true;
}

std::string effectiveVersion(const std::filesystem::path& exe, const std::string& pe) {
    if (pe.empty() || !lastSegIsZero(pe)) return pe;
    const std::string emb = pxc_probe::embeddedVersion(exe, pe);
    if (emb.empty()) return pe;
    return compareVersion(emb, pe) > 0 ? emb : pe;
}
} // namespace

std::string today() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

static void upsert(std::vector<pxc_store::ExeEntry>& registry, pxc_store::ExeEntry e) {
    for (auto& x : registry) {
        // 不能用 == ：同一份 exe 的路径写法可能不同（相对/绝对、大小写、..），
        // 判错就变成"加一条"而不是"改这一条"。
        if (samePath(x.exe_path, e.exe_path)) { x = e; return; }
    }
    registry.push_back(std::move(e));
}

bool addExe(std::vector<pxc_store::ExeEntry>& registry, const std::filesystem::path& exePath,
            std::string& outVersion) {
    std::string ver = effectiveVersion(exePath, pxc::readExeVersion(exePath));
    pxc_store::ExeEntry e;
    e.exe_path = exePath.u8string(); // 宽路径 -> UTF-8 存储（JSON 友好）
    e.display_name = exePath.filename().u8string();
    e.registered_at = today();

    if (!ver.empty()) {
        e.product_version = ver;
        e.source = "auto";
        upsert(registry, e);
        outVersion = ver;
        pxc_log::write("INFO", "Registry", "自动登记 exe: " + e.exe_path + " | 版本=" + ver);
        return true;
    }
    // 读不到：留待用户手动填版本
    e.product_version = "";
    e.source = "manual";
    outVersion.clear();
    pxc_log::write("WARN", "Registry", "PE 版本读取失败，需要手动指定: " + e.exe_path);
    return false;
}

void addManual(std::vector<pxc_store::ExeEntry>& registry, const std::filesystem::path& exePath,
               const std::string& version) {
    const std::string u8 = exePath.u8string();
    // 已登记过：只改版本与来源，别的字段**一律保留**。
    // 「设置版本号」是对已有条目的修改，不是重新登记 —— 走整条覆盖会把 registered_at 冲成今天
    // （同版本卡片按登记时间倒序，位置会莫名跳走），还会把 steam 条目的 acked_version 清掉。
    for (auto& x : registry) {
        if (!samePath(x.exe_path, u8)) continue;
        x.product_version = version;
        x.source = "manual";       // 锁定：刷新不再用 PE 值覆盖（见 refreshAll）
        pxc_log::write("INFO", "Registry", "手动登记 exe: " + x.exe_path + " | 版本=" + version);
        return;
    }
    pxc_store::ExeEntry e;
    e.exe_path = u8;
    e.display_name = exePath.filename().u8string();
    e.product_version = version;
    e.source = "manual";
    e.registered_at = today();
    upsert(registry, e);
    pxc_log::write("INFO", "Registry", "手动登记 exe: " + e.exe_path + " | 版本=" + version);
}

namespace {

// 只认"纯数字 + 点"。任意别的字符（rc/alpha/-beta/空格）都判为不认识，返回 false：
// 带后缀的版本串规范成四段等于凭空编数字，宁可退回字符串比较。
bool splitNumeric(const std::string& v, int out[4]) {
    if (v.empty()) return false;
    std::vector<int> parts;
    int cur = 0;
    bool hasDigit = false;
    for (char ch : v) {
        if (ch >= '0' && ch <= '9') {
            cur = cur * 10 + (ch - '0');
            if (cur > 999999) return false;      // 段值大得不像版本号
            hasDigit = true;
        } else if (ch == '.') {
            if (!hasDigit) return false;          // "1..2" / ".1"
            parts.push_back(cur);
            cur = 0;
            hasDigit = false;
            if (parts.size() > 4) return false;   // 超过四段
        } else {
            return false;
        }
    }
    if (!hasDigit) return false;                  // 末尾是点
    parts.push_back(cur);
    if (parts.size() > 4) return false;
    for (std::size_t i = 0; i < parts.size(); ++i) out[i] = parts[i];
    for (std::size_t i = parts.size(); i < 4; ++i) out[i] = 0;
    return true;
}

} // namespace

std::string normalizeVersion(const std::string& v) {
    int p[4] = {0, 0, 0, 0};
    if (!splitNumeric(v, p)) return v;
    char b[48];
    std::snprintf(b, sizeof(b), "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
    return b;
}

bool sameVersion(const std::string& a, const std::string& b) {
    if (a == b) return true;
    int x[4] = {0, 0, 0, 0}, y[4] = {0, 0, 0, 0};
    if (!splitNumeric(a, x) || !splitNumeric(b, y)) return false;
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
}

std::vector<pxc_store::ExeEntry> findMatching(const std::vector<pxc_store::ExeEntry>& registry,
                                              const std::string& productVersion) {
    std::vector<pxc_store::ExeEntry> out;
    if (productVersion.empty()) return out;
    for (const auto& e : registry) {
        if (sameVersion(e.product_version, productVersion)) out.push_back(e);
    }
    return out;
}

int removeExe(std::vector<pxc_store::ExeEntry>& registry, const std::string& exePath) {
    // 两处都必须松：
    // ① 用 samePath 而不是 == —— 同一个 exe 的写法可能差斜杠/大小写，精确比较会"点了没反应"；
    // ② 删**全部**同路径条目 —— 历史脏数据可能在磁盘上躺着好几条一模一样的，
    //    只删第一条的话用户点一次只少一张卡、剩下几张长得完全一样，看起来就是"删不掉"。
    const std::size_t before = registry.size();
    registry.erase(std::remove_if(registry.begin(), registry.end(),
                                  [&](const pxc_store::ExeEntry& e) {
                                      return samePath(e.exe_path, exePath);
                                  }),
                   registry.end());
    const int removed = (int)(before - registry.size());
    if (removed > 0)
        pxc_log::write("INFO", "Registry",
                       "删除登记: " + exePath + (removed > 1 ? "（一并清掉 " +
                                                                 std::to_string(removed - 1) +
                                                                 " 条同路径重复）"
                                                             : ""));
    return removed;
}

// ---------- v5：渠道 / 刷新 / 同版本（docs/07） ----------

// 逐字符比较：斜杠统一、忽略大小写。
static bool samePathRaw(const std::string& a, const std::string& b) {
    if (a == b) return true;
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (std::tolower((unsigned char)x) != std::tolower((unsigned char)y)) return false;
    }
    return true;
}

bool samePath(const std::string& a, const std::string& b) {
    if (samePathRaw(a, b)) return true;
    // 兜一层词法归一：`..` 会让同一份 exe 出现两种写法（老配置里存的是相对路径，
    // 或是外部工具写进来的），只比字符串必然判成两条 → 每次刷新都"重新发现"一次。
    // 纯词法、不碰磁盘，可以留在扫描热路径上；没有 `..` 时这一步直接跳过。
    if (a.find("..") == std::string::npos && b.find("..") == std::string::npos) return false;
    std::error_code ec;
    return samePathRaw(pxc_util::pathFromUtf8(a).lexically_normal().u8string(),
                       pxc_util::pathFromUtf8(b).lexically_normal().u8string());
}

int compareVersion(const std::string& a, const std::string& b) {
    int x[4] = {0, 0, 0, 0}, y[4] = {0, 0, 0, 0};
    if (splitNumeric(a, x) && splitNumeric(b, y)) {
        for (int i = 0; i < 4; ++i) {
            if (x[i] != y[i]) return x[i] > y[i] ? 1 : -1;
        }
        return 0;
    }
    // 猜不出语义（"1.21rc2"）就退回字符串序 —— 排序难看一点好过瞎编数字。
    if (a == b) return 0;
    return a > b ? 1 : -1;
}

namespace {

bool fileExistsAt(const std::string& utf8Path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pxc_util::pathFromUtf8(utf8Path), ec)) return false;
    return true;
}

// 给"登记表里有没有这条路径"用的小工具。
const pxc_store::ExeEntry* findEntry(const std::vector<pxc_store::ExeEntry>& reg, const std::string& path) {
    for (const auto& e : reg)
        if (samePath(e.exe_path, path)) return &e;
    return nullptr;
}

// 单条刷新的主体：② 存在性 → ④ 渠道回填 → ③ 重读版本。全量和单条都走这里，只有一份逻辑。
EntryRefresh refreshOne(pxc_store::ExeEntry& e, const std::vector<std::filesystem::path>& libs) {
    EntryRefresh r;
    r.found = true;
    r.changedPath = e.exe_path;
    if (!fileExistsAt(e.exe_path)) {
        r.missing = true;
        return r;
    }
    const auto p = pxc_util::pathFromUtf8(e.exe_path);
    const std::string ch = pxc_steam::libraryOf(p, libs).empty() ? "itch" : "steam";
    if (e.channel != ch) {
        e.channel = ch;
        r.channel_filled = true;
    }
    const std::string pe = pxc::readExeVersion(p);
    if (pe.empty()) return r;      // 读不到保留旧值，不清空、不弹窗（§11.8「不弹的情况」①）
    const std::string now = effectiveVersion(p, pe);
    r.reread = true;
    // source=manual 的条目：版本号是用户显式裁定的，刷新**不许**用读到的值盖回去（§11.8.1）。
    if (e.source == "manual") {
        if (!sameVersion(now, e.product_version))
            pxc_log::write("INFO", "Registry",
                           "版本号已手动指定，忽略读到的值：" + e.exe_path + " 手动=" + e.product_version +
                               " 读到=" + now);
        return r;
    }
    if (!sameVersion(now, e.product_version)) {
        std::string extra;
        if (!sameVersion(now, pe)) extra = "（PE 资源写的是 " + pe + "，采用内嵌数据包的值）";
        pxc_log::write("INFO", "Registry",
                       "版本变化：" + e.exe_path + " " + e.product_version + " -> " + now + extra);
        e.product_version = now;
        r.changed = true;
    }
    return r;
}

} // namespace

std::string detectChannel(const std::filesystem::path& exe) {
    return pxc_steam::libraryOf(exe, pxc_steam::libraries()).empty() ? "itch" : "steam";
}

const pxc_store::ExeEntry* findSameChannelDup(const std::vector<pxc_store::ExeEntry>& registry,
                                             const std::string& exePath, const std::string& version,
                                             const std::string& channel) {
    if (version.empty()) return nullptr;
    for (const auto& e : registry) {
        if (samePath(e.exe_path, exePath)) continue;      // 自己不算重复
        if (!sameVersion(e.product_version, version)) continue;
        // 渠道空（还没判定）按 itch 算：用户手动拖进来的就是 itch，等刷新回填。
        const std::string ch = e.channel.empty() ? "itch" : e.channel;
        if (ch == channel) return &e;
    }
    return nullptr;
}

bool hasCrossChannel(const std::vector<pxc_store::ExeEntry>& registry, const std::string& version) {
    if (version.empty()) return false;
    bool itch = false, steam = false;
    for (const auto& e : registry) {
        if (!sameVersion(e.product_version, version)) continue;
        if (e.channel == "steam") steam = true;
        else itch = true;
    }
    return itch && steam;
}

std::size_t choosePreferred(const std::vector<pxc_store::ExeEntry>& matches,
                            const std::map<std::string, std::string>& prefer,
                            const std::string& version) {
    if (matches.empty()) return 0;
    std::string want;
    auto it = prefer.find(normalizeVersion(version));
    if (it == prefer.end()) it = prefer.find(version);
    if (it != prefer.end()) want = it->second;   // 空串 = 问过没表态 → 走兜底
    if (!want.empty()) {
        for (std::size_t i = 0; i < matches.size(); ++i)
            if (matches[i].channel == want) return i;
    }
    for (std::size_t i = 0; i < matches.size(); ++i)
        if (matches[i].channel == "steam") return i;
    return 0;
}

RefreshResult refreshAll(pxc_store::AppConfig& cfg) {
    RefreshResult r;
    const std::vector<std::filesystem::path> libs = pxc_steam::libraries();   // 一次读注册表 + vdf
    const std::vector<pxc_steam::Hit> hits = pxc_steam::find();

    // ---- ① Steam 重发现 ----
    // 先算出"磁盘上有、登记表里没有"的命中。
    std::vector<pxc_steam::Hit> unmatched;
    for (const auto& h : hits) {
        if (!findEntry(cfg.registry, h.exe.u8string())) unmatched.push_back(h);
    }
    // 库换盘：旧 Steam 条目的文件已经不在了，而恰好有一个还没登记的新命中 →
    // 当成同一条搬家，改路径而不是"删一条 + 加一条"（后者会把用户的 acked_version 丢掉）。
    std::vector<std::size_t> staleSteam;
    for (std::size_t i = 0; i < cfg.registry.size(); ++i) {
        const auto& e = cfg.registry[i];
        if (e.channel == "steam" && !fileExistsAt(e.exe_path)) staleSteam.push_back(i);
    }
    if (unmatched.size() == 1 && staleSteam.size() == 1) {
        auto& e = cfg.registry[staleSteam.front()];
        pxc_log::write("INFO", "Registry",
                       "Steam 库位置变更：" + e.exe_path + " -> " + unmatched.front().exe.u8string());
        e.exe_path = unmatched.front().exe.u8string();
        e.display_name = unmatched.front().exe.filename().u8string();
        unmatched.clear();
    }
    for (const auto& h : unmatched) {
        pxc_store::ExeEntry e;
        e.exe_path = h.exe.u8string();
        e.display_name = h.exe.filename().u8string();
        e.product_version = effectiveVersion(h.exe, pxc::readExeVersion(h.exe));
        e.source = e.product_version.empty() ? "manual" : "auto";
        e.registered_at = today();
        e.channel = "steam";
        // 首次发现 = 已确认：否则第一次出现就带「!」角标（§11.6 的初值规则）。
        e.acked_version = e.product_version;
        pxc_log::write("INFO", "Registry", "Steam 自动发现：" + e.exe_path + " | 版本=" + e.product_version);
        cfg.registry.push_back(std::move(e));
        ++r.steam_added;
    }

    // ---- ② 存在性 + ③ 重读 + ④ 渠道回填 ----
    for (auto& e : cfg.registry) {
        const EntryRefresh er = refreshOne(e, libs);
        if (er.missing) {
            r.missing.push_back(e.exe_path);
            continue;      // 盘可能只是没插：不删、不改渠道
        }
        if (er.channel_filled) ++r.channel_filled;
        if (er.reread) ++r.reread;
        // 老配置补初值：首次发现 = 已确认（否则第一张卡就带「!」）
        if (e.channel == "steam" && e.acked_version.empty()) e.acked_version = e.product_version;
    }
    r.changes = pendingChanges(cfg);
    return r;
}

std::vector<VersionChange> pendingChanges(const pxc_store::AppConfig& cfg) {
    std::vector<VersionChange> out;
    for (const auto& e : cfg.registry) {
        if (e.channel != "steam") continue;
        if (e.acked_version.empty()) continue;   // 初值还没补，不当成变化
        if (sameVersion(e.acked_version, e.product_version)) continue;
        out.push_back({e.exe_path, e.acked_version, e.product_version});
    }
    return out;
}

EntryRefresh refreshEntry(pxc_store::AppConfig& cfg, const std::string& exePath) {
    EntryRefresh r;
    const std::vector<std::filesystem::path> libs = pxc_steam::libraries();
    for (auto& e : cfg.registry) {
        if (!samePath(e.exe_path, exePath)) continue;
        r = refreshOne(e, libs);
        if (e.channel == "steam" && e.acked_version.empty()) e.acked_version = e.product_version;
        return r;
    }
    return r;   // found=false：登记表里没这条（比如刚删掉）
}

namespace {

// 便携扫描的候选目录集合（docs/07 §11.2.6）。
// 抽出来是为了让「自动发现」和「这条是不是便携条目」共用同一套范围 —— 两处各写一份必然漂移，
// 漂移的结果就是 UI 判成便携、扫描却不认（或反之），删除项该藏不藏。
//
// **范围只保留两种布局**：
//   ① 启动器目录（含其子目录）：PXC 与启动器同放一个文件夹，或放在启动器的子文件夹里；
//   ② 父目录**本身**：启动器放在 PXC 安装目录的子文件夹里（`PixelComposer\launcher\`）。
//
// ② 只取父目录这一层，**绝不遍历父目录的子目录**：启动器装在盘符顶层下一层时
// （`D:\PXC Launcher`），parent 就是 `D:\`，遍历它的子目录等于把整盘一级目录
// （`D:\我的文档`、`D:\Games`…）全扫一遍 —— 扫描范围随安装位置膨胀，既慢又会误收。
std::vector<std::filesystem::path> portableDirs(const std::filesystem::path& launcherDir) {
    std::vector<std::filesystem::path> dirs;
    if (launcherDir.empty()) return dirs;

    // **必须去重**：`launcherDir` 与 `parent` 在特定布局下可能指向同一处
    // （启动器位于根目录时 parent 就是自己）。平时 findEntry 会挡住第二次，看不出问题；
    // 但只要 findEntry 因路径写法不同而失配，**一次启动就会把同一个 exe 加两条**。
    // 扫描范围本身就不该有重复项。
    std::vector<std::wstring> seen;
    auto add = [&](const std::filesystem::path& p) {
        std::error_code ec;
        if (!std::filesystem::is_directory(p, ec)) return;
        std::wstring key = p.lexically_normal().native();
        for (auto& c : key) c = (wchar_t)std::towlower(c);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
        seen.push_back(std::move(key));
        dirs.push_back(p);
    };

    // ① 启动器目录 + 它的子目录
    add(launcherDir);
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(launcherDir, ec))
        if (de.is_directory(ec)) add(de.path());

    // ② 父目录本身（不展开它的子目录，理由见函数头）
    add(launcherDir.parent_path());
    return dirs;
}

} // namespace

bool isPortablePath(const std::string& exePath, const std::filesystem::path& launcherDir) {
    if (exePath.empty() || launcherDir.empty()) return false;
    // 只看「落在不在扫描范围内」，不判文件在不在 —— 存在性由调用方按场景决定。
    for (const auto& dir : portableDirs(launcherDir)) {
        if (samePath((dir / pxc_util::pathFromUtf8(exePath).filename()).u8string(), exePath))
            return true;
    }
    return false;
}

int discoverPortable(std::vector<pxc_store::ExeEntry>& registry,
                     const std::filesystem::path& launcherDir) {
    if (launcherDir.empty()) return 0;
    const std::vector<std::filesystem::path> dirs = portableDirs(launcherDir);

    int added = 0;
    for (const auto& dir : dirs) {
        const std::filesystem::path exe = dir / L"PixelComposer.exe";
        std::error_code ec;
        if (!std::filesystem::is_regular_file(exe, ec)) continue;
        const std::string u8 = exe.u8string();
        if (findEntry(registry, u8)) continue;   // 已登记（含同 exe 的不同写法）跳过
        pxc_store::ExeEntry e;
        e.exe_path = u8;
        e.display_name = exe.filename().u8string();
        e.product_version = effectiveVersion(exe, pxc::readExeVersion(exe));
        e.source = e.product_version.empty() ? "manual" : "auto";
        e.registered_at = today();
        // channel 留空：刷新时 detectChannel 按"不在 Steam 库下"归为 itch，
        // 与本地手动拖入的 itch 同源 —— 便携打包带走的本来就是 itch 版。
        e.channel = "";
        if (e.product_version.empty())
            pxc_log::write("WARN", "Registry", "便携版 PE 版本读取失败，需手动指定: " + u8);
        else
            pxc_log::write("INFO", "Registry", "便携版自动发现：" + u8 + " | 版本=" + e.product_version);
        registry.push_back(std::move(e));
        ++added;
    }
    return added;
}

} // namespace pxc_reg
