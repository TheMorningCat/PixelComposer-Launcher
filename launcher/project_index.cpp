// 工程索引实现：只读别人的数据（recent.json / Autosave），自己写的只有 projects.json 与识别缓存。
#include "project_index.h"
#include "version_registry.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <tuple>

#include "io_util.h"
#include "log.h"
#include "search_fold.h"
#include "store.h"
#include "util.h"
#include "version_advisor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace pxc_index {

namespace {

// FILETIME -> unix 秒。FILETIME 从 1601 起算，100ns 一格；差值常量是 1970-01-01 的偏移。
constexpr std::uint64_t kEpochDiff100ns = 116444736000000000ULL;

std::int64_t unixFromFileTime(const FILETIME& ft) {
    std::uint64_t t = ((std::uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    if (t < kEpochDiff100ns) return 0;
    return (std::int64_t)((t - kEpochDiff100ns) / 10000000ULL);
}

std::string u8(const fs::path& p) { return p.u8string(); }

bool endsWithPxc(const std::wstring& name) {
    if (name.size() < 4) return false;
    std::wstring ext = name.substr(name.size() - 4);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return ext == L".pxc";
}

std::string stemOf(const std::wstring& name) {   // 去掉扩展名
    size_t dot = name.rfind(L'.');
    std::wstring s = (dot == std::wstring::npos) ? name : name.substr(0, dot);
    return pxc_util::toUtf8(s.c_str());
}

// 列目录（FindFirstFileW）。不用 std::filesystem 遍历：配 \\?\ 前缀时它会卡死，
// 而这里要处理 4000 个文件的长路径。
struct FindEnum {
    HANDLE h = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW d{};
    bool next() {
        if (h == INVALID_HANDLE_VALUE) return false;
        return FindNextFileW(h, &d) != 0;
    }
    ~FindEnum() {
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
    }
};

void scanInto(std::vector<Item>& out, const fs::path& dir, bool recursive, int depth) {
    if (depth > 12) return;
    FindEnum fe;
    std::wstring pat = dir.wstring();
    if (!pat.empty() && pat.back() != L'\\' && pat.back() != L'/') pat += L'\\';
    pat += L"*";
    fe.h = FindFirstFileW(pat.c_str(), &fe.d);
    if (fe.h == INVALID_HANDLE_VALUE) return;

    do {
        const std::wstring name = fe.d.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fe.d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!recursive) continue;
            if (fe.d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // 不跟符号链接，防环
            scanInto(out, dir / fs::path(name), true, depth + 1);
            continue;
        }
        if (!endsWithPxc(name)) continue;
        Item it;
        it.path = u8(dir / fs::path(name));
        it.name = stemOf(name);
        it.parentDir = u8(dir);
        it.size = ((std::uint64_t)fe.d.nFileSizeHigh << 32) | fe.d.nFileSizeLow;
        it.mtime = unixFromFileTime(fe.d.ftLastWriteTime);
        out.push_back(std::move(it));
    } while (fe.next());
}

std::string getStr(const pxc_store::JsonValue& o, const char* k) {
    const pxc_store::JsonValue* v = o.find(k);
    return (v && v->isStr()) ? v->str : std::string();
}
int getInt(const pxc_store::JsonValue& o, const char* k, int def = 0) {
    const pxc_store::JsonValue* v = o.find(k);
    return (v && v->type == pxc_store::JsonValue::Num) ? (int)v->num : def;
}
bool getBool(const pxc_store::JsonValue& o, const char* k, bool def = false) {
    const pxc_store::JsonValue* v = o.find(k);
    return (v && v->type == pxc_store::JsonValue::Bool) ? v->b : def;
}

pxc_store::JsonValue jstr(const std::string& s) {
    pxc_store::JsonValue v;
    v.type = pxc_store::JsonValue::Str;
    v.str = s;
    return v;
}
pxc_store::JsonValue jnum(double d, bool asFloat = false) {
    pxc_store::JsonValue v;
    v.type = pxc_store::JsonValue::Num;
    v.num = d;
    v.numFloat = asFloat;
    return v;
}
pxc_store::JsonValue jbool(bool b) {
    pxc_store::JsonValue v;
    v.type = pxc_store::JsonValue::Bool;
    v.b = b;
    return v;
}
pxc_store::JsonValue jarr() {
    pxc_store::JsonValue v;
    v.type = pxc_store::JsonValue::Arr;
    return v;
}
pxc_store::JsonValue jobj() {
    pxc_store::JsonValue v;
    v.type = pxc_store::JsonValue::Obj;
    return v;
}
void put(pxc_store::JsonValue& o, const char* k, pxc_store::JsonValue v) {
    o.obj.emplace_back(k, std::move(v));
}

} // namespace

const char* tabName(int tab) {
    switch (tab) {
        case 0: return "近期文件";
        case 1: return "全部";
        case 2: return "工程库";
        case 3: return "启动历史";
        case 4: return "收藏";
        case 5: return "自动恢复";
        default: return "?";
    }
}

int confOf(pxc::Confidence c) {
    switch (c) {
        case pxc::Confidence::HIGH: return 0;
        case pxc::Confidence::MEDIUM: return 1;
        case pxc::Confidence::LOW: return 2;
        default: return 3;
    }
}

const char* confLabel(int conf) {
    switch (conf) {
        case 0: return "高";
        case 1: return "中";
        case 2: return "低";
        default: return "未知";
    }
}

// ---------------- 用户目录 ----------------
std::vector<fs::path> userDirCandidates() {
    std::vector<fs::path> out;
    auto push = [&](const fs::path& p) {
        if (p.empty()) return;
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    auto envWide = [](const wchar_t* name) -> fs::path {
        wchar_t buf[1024] = {0};
        DWORD n = GetEnvironmentVariableW(name, buf, 1024);
        if (n == 0 || n >= 1024) return {};
        return fs::path(std::wstring(buf, n));
    };
    const fs::path localAppData = envWide(L"LOCALAPPDATA");
    const fs::path appData = envWide(L"APPDATA");

    // 1) PXC 自己的重定向记录（本机就指向 D 盘，硬编码 %LOCALAPPDATA% 会读到几个月前的旧列表）
    for (const fs::path& root : {localAppData, appData}) {
        if (root.empty()) continue;
        auto [txt, ok] = pxc_io::readText(root / "PixelComposer" / "persistPreference.json");
        if (!ok) continue;
        auto [jv, okj] = pxc_store::parse(txt);
        if (!okj || !jv.isObj()) continue;
        if (const auto* p = jv.find("path"); p && p->isStr() && !p->str.empty())
            push(pxc_util::pathFromUtf8(p->str));
    }
    // 2) 默认位置
    if (!localAppData.empty()) push(localAppData / "PixelComposer");
    if (!appData.empty()) push(appData / "PixelComposer");
    return out;
}

fs::path resolveUserDir() {
    const auto cands = userDirCandidates();
    auto has = [](const fs::path& d, const wchar_t* name) {
        std::error_code ec;
        return fs::exists(d / name, ec);
    };
    for (const auto& d : cands)
        if (has(d, L"recent.json")) return d;
    for (const auto& d : cands)
        if (has(d, L"Autosave")) return d;
    return cands.empty() ? fs::path() : cands.front();
}

// ---------------- 扫描 ----------------
std::vector<Item> readRecent(const fs::path& userDir, std::vector<std::string>* missing) {
    std::vector<Item> out;
    if (userDir.empty()) return out;
    auto [jv, ok] = pxc_io::readJson(userDir / "recent.json");
    if (!ok || !jv.isObj()) return out;
    const auto* arr = jv.find("Recents");
    if (!arr || !arr->isArr()) return out;

    for (const auto& e : arr->arr) {
        if (!e.isStr() || e.str.empty()) continue;
        const fs::path p = pxc_util::pathFromUtf8(e.str);
        Item it;
        if (!statPath(p, it)) {           // 失效路径：按 §7.8 直接不显示
            if (missing) missing->push_back(e.str);
            continue;
        }
        out.push_back(std::move(it));
    }
    return out;
}

std::vector<Item> scanLayer(const fs::path& dir) {
    std::vector<Item> out;
    if (dir.empty()) return out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    FindEnum fe;
    std::wstring pat = dir.wstring();
    if (!pat.empty() && pat.back() != L'\\' && pat.back() != L'/') pat += L'\\';
    pat += L"*";
    fe.h = FindFirstFileW(pat.c_str(), &fe.d);
    if (fe.h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring name = fe.d.cFileName;
        if (name == L"." || name == L"..") continue;
        const fs::path p = dir / fs::path(name);
        if (fe.d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fe.d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // 不跟符号链接，防环
            Item it;
            it.folder = true;
            it.path = u8(p);
            it.name = pxc_util::toUtf8(name.c_str());
            it.parentDir = u8(dir);
            it.mtime = unixFromFileTime(fe.d.ftLastWriteTime);
            out.push_back(std::move(it));
            continue;
        }
        if (!endsWithPxc(name)) continue;
        Item it;
        it.path = u8(p);
        it.name = stemOf(name);
        it.parentDir = u8(dir);
        it.size = ((std::uint64_t)fe.d.nFileSizeHigh << 32) | fe.d.nFileSizeLow;
        it.mtime = unixFromFileTime(fe.d.ftLastWriteTime);
        out.push_back(std::move(it));
    } while (fe.next());
    return out;
}

std::vector<Item> scanPxcDir(const fs::path& dir, bool recursive) {
    std::vector<Item> out;
    if (dir.empty()) return out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    scanInto(out, dir, recursive, 0);
    return out;
}

// file_time_type -> unix 秒。纪元在 MSVC 上是 1601（同 FILETIME）。
// **必须先按秒取差再减偏移**：1601 到现在的纳秒数约 1.3e19，已经越过 int64 上限
// 9.2e18，先转纳秒会静默溢出成负数（2026 年的文件算出来是 -16653320967，
// 界面「修改时间」一列会全变成「—」，排序也跟着乱）。秒级 1.3e10 完全够用。
static std::int64_t unixFromFileClock(const fs::file_time_type& ft) {
    const auto sec =
        std::chrono::duration_cast<std::chrono::seconds>(ft.time_since_epoch()).count();
    return (std::int64_t)sec - 11644473600LL;
}

bool statPath(const fs::path& p, Item& out) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) {
        out.folder = false;
        out.name = pxc_util::toUtf8(p.stem().c_str());
        out.size = (std::uint64_t)fs::file_size(p, ec);
        if (ec) out.size = 0;
        const auto ft = fs::last_write_time(p, ec);
        if (!ec) out.mtime = unixFromFileClock(ft);
        out.path = u8(p);
        out.parentDir = u8(p.parent_path());
        return true;
    }
    if (fs::is_directory(p, ec)) {
        out.folder = true;
        out.name = pxc_util::toUtf8(p.filename().c_str());
        if (out.name.empty()) out.name = u8(p);
        out.path = u8(p);
        out.parentDir = u8(p.parent_path());
        const auto ft = fs::last_write_time(p, ec);
        if (!ec) out.mtime = unixFromFileClock(ft);
        return true;
    }
    return false;
}

void applyDetect(Item& it, const pxc::DetectionResult& r) {
    it.detected = true;
    // 文件里存的版本可能省了尾部 .0（1.20.1.0 -> "1.20.1"）：这里补成四段，
    // 后面显示、筛选、匹配都用同一份值，免得三处各写一次规范化。
    it.version = pxc_reg::normalizeVersion(r.version);
    switch (r.format) {
        case pxc::Format::JSON: it.format = "JSON"; break;
        case pxc::Format::ZLIB: it.format = "zlib"; break;
        case pxc::Format::PXCX: it.format = "PXCX"; break;
        default: it.format = "未知"; break;
    }
    it.conf = confOf(r.confidence);
}

// ---------------- 排序 ----------------
SortSpec defaultSort(int tab) {
    // 近期文件与启动历史都**保持数组原顺序**：前者是 PXC 自己写的最新在前，
    // 后者是 noteLaunch 插到表头的顺序 —— 都是"最近在前"，再排一次反而会把语义排丢。
    if (tab == (int)Tab::Recent || tab == (int)Tab::History) return {SortKey::None, true};
    return {SortKey::Mtime, false};   // 自动恢复 / 全部 / 工程库 / 收藏 都是"最近在前"
}

SortSpec nextSort(SortSpec cur) {
    switch (cur.key) {
        case SortKey::None:
        case SortKey::Version: return {SortKey::Name, true};
        case SortKey::Name: return cur.asc ? SortSpec{SortKey::Name, false} : SortSpec{SortKey::Mtime, false};
        case SortKey::Mtime: return cur.asc ? SortSpec{SortKey::Mtime, false} : SortSpec{SortKey::Version, true};
    }
    return {SortKey::Name, true};
}

const char* sortLabel(SortSpec spec) {
    switch (spec.key) {
        case SortKey::Name: return spec.asc ? "文件名 ↑" : "文件名 ↓";
        case SortKey::Mtime: return spec.asc ? "修改时间 ↑" : "修改时间 ↓";
        case SortKey::Version: return spec.asc ? "版本 ↑" : "版本 ↓";
        default: return "默认顺序";
    }
}

std::string foldQuery(const std::string& q) { return pxc_search::trim(pxc_search::fold(q)); }

bool matchQuery(const Item& it, const std::string& foldedQuery) {
    if (foldedQuery.empty()) return true;
    const std::string n = pxc_search::fold(it.name);
    const std::string p = pxc_search::fold(it.path);
    const std::string* fields[2] = {&n, &p};
    return pxc_search::hit(fields, 2, foldedQuery);
}

void sortIndices(std::vector<int>& idx, const std::vector<Item>& items, SortSpec spec) {
    if (spec.key == SortKey::None || idx.size() < 2) return;
    // 预折叠名字，避免比较函数里反复 fold（4000 条 × O(n log n) 次 fold 会很慢）。
    std::map<int, std::string> folded;
    if (spec.key == SortKey::Name)
        for (int i : idx) folded.emplace(i, pxc_search::fold(items[(size_t)i].name));

    auto less = [&](int ia, int ib) {
        const Item& a = items[(size_t)ia];
        const Item& b = items[(size_t)ib];
        switch (spec.key) {
            case SortKey::Name: {
                const std::string& fa = folded[ia];
                const std::string& fb = folded[ib];
                if (fa != fb) return spec.asc ? (fa < fb) : (fa > fb);
                return a.path < b.path;
            }
            case SortKey::Mtime:
                if (a.mtime != b.mtime) return spec.asc ? (a.mtime < b.mtime) : (a.mtime > b.mtime);
                return a.path < b.path;
            case SortKey::Version: {
                // 按数字段比较（1.9 < 1.18 不能用字典序）；缺失/解析不出的排最后。
                const pxc_adv::VKey ka = pxc_adv::parseVersion(a.version);
                const pxc_adv::VKey kb = pxc_adv::parseVersion(b.version);
                // 能解析的在前；解析不出的（空版本/未知）一律排最后，升降序都一样。
                // 注意这里是 ka.ok —— 写反会让空版本排到表头，最需要关注的"未知"反而最显眼。
                if (ka.ok != kb.ok) return ka.ok;
                if (ka.ok && kb.ok) {
                    auto key = [](const pxc_adv::VKey& k) {
                        return std::make_tuple(k.major, k.minor, k.patch, k.build);
                    };
                    auto ta = key(ka), tb = key(kb);
                    if (ta != tb) return spec.asc ? (ta < tb) : (ta > tb);
                }
                return a.path < b.path;
            }
            default: return false;
        }
    };
    std::stable_sort(idx.begin(), idx.end(), less);
}

void sortItems(std::vector<Item>& items, SortSpec spec) {
    if (spec.key == SortKey::None) return;
    std::vector<int> idx(items.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int)i;
    sortIndices(idx, items, spec);
    std::vector<Item> out;
    out.reserve(items.size());
    for (int i : idx) out.push_back(std::move(items[(size_t)i]));
    items = std::move(out);
}

const char* verStateName(int st) {
    switch (st) {
        case (int)VerState::Ok: return "本机可启动";
        case (int)VerState::Missing: return "版本缺失";
        case (int)VerState::Unreadable: return "识别不出";
        default: return "识别中";
    }
}

// 四档的显示顺序 = 用户关心的顺序（能开的 → 差版本的 → 坏文件 → 还没轮到的）。
VerState stateChipAt(int idx) {
    switch (idx) {
        case 0: return VerState::Ok;
        case 1: return VerState::Missing;
        case 2: return VerState::Unreadable;
        default: return VerState::Pending;
    }
}

int stateChipBit(int idx) { return 1 << (int)stateChipAt(idx); }

const char* stateChipName(int idx) { return verStateName((int)stateChipAt(idx)); }

const char* timeRangeName(int r) {
    switch (r) {
        case 1: return "今天";
        case 2: return "近 7 天";
        case 3: return "近 30 天";
        case 4: return "今年";
        case 5: return "自定义";
        default: return "不限";
    }
}

void timeRangeBounds(int r, std::int64_t now, std::int64_t& from, std::int64_t& to) {
    from = 0;
    to = now > 0 ? now : nowUnix();
    auto localMidnight = [](std::int64_t t, int month, int mday) {
        time_t tt = (time_t)t;
        std::tm tm{};
        localtime_s(&tm, &tt);
        if (month >= 0) tm.tm_mon = month;
        if (mday > 0) tm.tm_mday = mday;
        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = 0;
        tm.tm_isdst = -1;
        return (std::int64_t)mktime(&tm);
    };
    switch (r) {
        case 1: from = localMidnight(to, -1, -1); break;          // 今天 00:00
        case 2: from = to - 7LL * 86400; break;
        case 3: from = to - 30LL * 86400; break;
        case 4: from = localMidnight(to, 0, 1); break;            // 今年 1 月 1 日
        default: break;                                           // 0 不限 / 5 自定义（调用方给 from）
    }
}

VerState stateOf(const Item& it, const std::vector<std::string>& registered) {
    if (it.folder) return VerState::Pending;      // 文件夹不画徽章，调用方不看这个值
    if (!it.detected) return VerState::Pending;   // 还没轮到 => 识别中（灰）
    if (it.version.empty()) return VerState::Unreadable;   // 读过了，读不出 => 红
    for (const auto& v : registered)
        if (pxc_reg::sameVersion(v, it.version)) return VerState::Ok;
    return VerState::Missing;
}

bool Filter::active() const {
    return !versions.empty() || states != 0 || timeRange != 0 || onlyFav;
}

int Filter::activeGroups() const {
    int n = 0;
    if (!versions.empty()) ++n;
    if (states != 0) ++n;
    if (timeRange != 0) ++n;
    if (onlyFav) ++n;
    return n;
}

void Filter::reset() {
    versions.clear();
    states = 0;
    timeRange = 0;
    from = 0;
    to = 0;
    onlyFav = false;
}

bool passFilter(const Item& it, const Filter& f, const std::vector<std::string>& registered,
                bool isFav) {
    if (!f.active()) return true;

    // 版本 / 状态这两维描述的是"文件里存的是什么版本"，文件夹没有这个属性，
    // 一旦启用就把文件夹排掉 —— 否则「只看 1.20」下面还站着一排文件夹，用户会以为筛选没生效。
    if (!f.versions.empty()) {
        if (it.folder) return false;
        bool hit = false;
        for (const auto& v : f.versions) {
            if (v.empty()) {                                  // 空串 = 「未知版本」档
                if (it.detected && it.version.empty()) { hit = true; break; }
            } else if (!it.version.empty() && pxc_reg::sameVersion(v, it.version)) {
                hit = true;
                break;
            }
        }
        if (!hit) return false;
    }
    if (f.states != 0) {
        if (it.folder) return false;
        if ((f.states & (1 << (int)stateOf(it, registered))) == 0) return false;
    }
    if (f.timeRange != 0) {
        std::int64_t from = f.from, to = f.to;
        if (f.timeRange != 5) timeRangeBounds(f.timeRange, nowUnix(), from, to);
        if (it.mtime < from || it.mtime > to) return false;
    }
    if (f.onlyFav && !isFav) return false;
    return true;
}

// v3 的四档漏斗（旧调用方/测试）：口径已经统一到 stateOf，不再各写一套。
const char* funnelName(int f) {
    switch (f) {
        case 1: return "本机可启动";
        case 2: return "版本缺失";
        case 3: return "识别不出";
        default: return "全部";
    }
}

bool passFunnel(const Item& it, int funnel, const std::vector<std::string>& registered) {
    if (funnel == 0) return true;
    if (it.folder) return false;                    // 文件夹没有版本
    if (!it.detected) return true;                  // 还没识别：先别把行藏起来，否则列表会跳
    const VerState st = stateOf(it, registered);
    if (funnel == 3) return st == VerState::Unreadable;
    if (funnel == 1) return st == VerState::Ok;
    return st == VerState::Missing;                 // funnel == 2 版本缺失
}

// ---------------- projects.json ----------------
std::int64_t nowUnix() { return (std::int64_t)time(nullptr); }

std::string isoOf(std::int64_t unixSec) {
    std::tm tm{};
    time_t t = (time_t)unixSec;
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &tm);
    return b;
}

std::string nowIso() { return isoOf(nowUnix()); }

std::int64_t parseIso(const std::string& s) {
    if (s.size() < 19) return 0;
    std::tm tm{};
    if (sscanf_s(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                 &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    return (std::int64_t)mktime(&tm);
}

Store loadStore(const fs::path& file, bool* ok) {
    Store s;
    if (ok) *ok = false;
    std::error_code ec;
    if (!fs::exists(file, ec)) {   // 首次运行：不是错误
        if (ok) *ok = true;
        return s;
    }
    auto [jv, okj] = pxc_io::readJson(file);
    if (!okj || !jv.isObj()) {
        // 坏文件：留一份 .bad，别让用户的数据无声消失
        fs::path bad = file;
        bad += ".bad";
        fs::copy_file(file, bad, fs::copy_options::overwrite_existing, ec);
        pxc_log::write("ERROR", "Index", "projects.json 解析失败，已另存 .bad：" + u8(file));
        return s;
    }
    if (const auto* a = jv.find("history"); a && a->isArr()) {
        for (const auto& e : a->arr) {
            if (!e.isObj()) continue;
            HistoryEntry h;
            h.path = getStr(e, "path");
            if (h.path.empty()) continue;
            h.version = getStr(e, "version");
            h.exeVersion = getStr(e, "exe_version");
            h.format = getStr(e, "format");
            h.conf = getStr(e, "conf");
            h.launchedAt = getStr(e, "launched_at");
            h.count = getInt(e, "count", 1);
            s.history.push_back(std::move(h));
        }
    }
    if (const auto* a = jv.find("imported"); a && a->isArr()) {
        for (const auto& e : a->arr) {
            if (!e.isObj()) continue;
            ImportEntry im;
            im.path = getStr(e, "path");
            if (im.path.empty()) continue;
            im.type = getStr(e, "type");
            im.addedAt = getStr(e, "added_at");
            s.imported.push_back(std::move(im));
        }
    }
    if (const auto* a = jv.find("favorites"); a && a->isArr()) {
        for (const auto& e : a->arr) {
            if (!e.isObj()) continue;
            FavoriteEntry f;
            f.path = getStr(e, "path");
            if (f.path.empty()) continue;
            f.addedAt = getStr(e, "added_at");
            s.favorites.push_back(std::move(f));
        }
    }
    if (const auto* a = jv.find("recent_hidden"); a && a->isArr()) {
        for (const auto& e : a->arr)
            if (e.isStr()) s.recentHidden.push_back(e.str);
    }
    if (const auto* a = jv.find("running"); a && a->isArr()) {
        for (const auto& e : a->arr) {
            if (!e.isObj()) continue;
            RunningEntry r;
            r.path = getStr(e, "path");
            r.exe = getStr(e, "exe");
            r.startedAt = getStr(e, "started_at");
            r.pid = getInt(e, "pid", 0);
            if (!r.path.empty()) s.running.push_back(std::move(r));
        }
    }
    if (const auto* a = jv.find("assoc"); a && a->isObj()) {
        s.assoc.registered = getBool(*a, "registered");
        s.assoc.silentLaunch = getBool(*a, "silent_launch", true);
        s.assoc.reuseInstance = getBool(*a, "reuse_instance", true);
        s.assoc.previousProgid = getStr(*a, "previous_progid");
        s.assoc.registeredAt = getStr(*a, "registered_at");
    }
    if (ok) *ok = true;
    return s;
}

bool saveStore(const fs::path& file, const Store& s) {
    pxc_store::JsonValue root = jobj();
    put(root, "version", jnum(1));

    pxc_store::JsonValue hist = jarr();
    for (const auto& h : s.history) {
        pxc_store::JsonValue o = jobj();
        put(o, "path", jstr(h.path));
        put(o, "version", jstr(h.version));
        put(o, "exe_version", jstr(h.exeVersion));
        put(o, "format", jstr(h.format));
        put(o, "conf", jstr(h.conf));
        put(o, "launched_at", jstr(h.launchedAt));
        put(o, "count", jnum((double)h.count));
        hist.arr.push_back(std::move(o));
    }
    put(root, "history", std::move(hist));

    pxc_store::JsonValue imp = jarr();
    for (const auto& i : s.imported) {
        pxc_store::JsonValue o = jobj();
        put(o, "path", jstr(i.path));
        put(o, "type", jstr(i.type));
        put(o, "added_at", jstr(i.addedAt));
        imp.arr.push_back(std::move(o));
    }
    put(root, "imported", std::move(imp));

    pxc_store::JsonValue fav = jarr();
    for (const auto& f : s.favorites) {
        pxc_store::JsonValue o = jobj();
        put(o, "path", jstr(f.path));
        put(o, "added_at", jstr(f.addedAt));
        fav.arr.push_back(std::move(o));
    }
    put(root, "favorites", std::move(fav));

    pxc_store::JsonValue hid = jarr();
    for (const auto& p : s.recentHidden) hid.arr.push_back(jstr(p));
    put(root, "recent_hidden", std::move(hid));

    pxc_store::JsonValue run = jarr();
    for (const auto& r : s.running) {
        pxc_store::JsonValue o = jobj();
        put(o, "path", jstr(r.path));
        put(o, "exe", jstr(r.exe));
        put(o, "pid", jnum((double)r.pid));
        put(o, "started_at", jstr(r.startedAt));
        run.arr.push_back(std::move(o));
    }
    put(root, "running", std::move(run));

    pxc_store::JsonValue a = jobj();
    put(a, "registered", jbool(s.assoc.registered));
    put(a, "previous_progid", jstr(s.assoc.previousProgid));
    put(a, "silent_launch", jbool(s.assoc.silentLaunch));
    put(a, "reuse_instance", jbool(s.assoc.reuseInstance));
    put(a, "registered_at", jstr(s.assoc.registeredAt));
    put(root, "assoc", std::move(a));

    // 原子写：先写 .tmp 再替换，避免写到一半断电留下半份 JSON。
    fs::path tmp = file;
    tmp += ".tmp";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (!pxc_io::writeText(tmp, pxc_store::serialize(root))) {
        pxc_log::write("ERROR", "Index", "写临时文件失败：" + u8(tmp));
        return false;
    }
    if (!MoveFileExW(tmp.wstring().c_str(), file.wstring().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        pxc_log::write("ERROR", "Index", "替换失败：" + u8(file));
        return false;
    }
    return true;
}

void noteLaunch(Store& s, const std::string& path, const std::string& fileVersion,
                const std::string& exeVersion, const std::string& format, const std::string& conf) {
    if (path.empty()) return;
    const std::string now = nowIso();
    for (auto& h : s.history) {
        if (h.path != path) continue;
        h.version = fileVersion;
        h.exeVersion = exeVersion;
        h.format = format;
        h.conf = conf;
        h.launchedAt = now;
        ++h.count;
        // 挪到最前（数组序即"最近使用"序）
        HistoryEntry copy = h;
        s.history.erase(std::remove_if(s.history.begin(), s.history.end(),
                                       [&](const HistoryEntry& x) { return x.path == path; }),
                        s.history.end());
        s.history.insert(s.history.begin(), std::move(copy));
        pruneStore(s);
        return;
    }
    HistoryEntry h;
    h.path = path;
    h.version = fileVersion;
    h.exeVersion = exeVersion;
    h.format = format;
    h.conf = conf;
    h.launchedAt = now;
    h.count = 1;
    s.history.insert(s.history.begin(), std::move(h));
    pruneStore(s);
}

void addImported(Store& s, const std::string& path, bool folder) {
    if (path.empty()) return;
    if (std::any_of(s.imported.begin(), s.imported.end(),
                    [&](const ImportEntry& e) { return e.path == path; }))
        return;
    ImportEntry e;
    e.path = path;
    e.type = folder ? "folder" : "file";
    e.addedAt = nowIso();
    s.imported.insert(s.imported.begin(), std::move(e));
    pruneStore(s);
}

void removeImported(Store& s, const std::string& path) {
    s.imported.erase(std::remove_if(s.imported.begin(), s.imported.end(),
                                    [&](const ImportEntry& e) { return e.path == path; }),
                     s.imported.end());
}

bool isFavorite(const Store& s, const std::string& path) {
    return std::any_of(s.favorites.begin(), s.favorites.end(),
                       [&](const FavoriteEntry& e) { return e.path == path; });
}

bool toggleFavorite(Store& s, const std::string& path) {
    if (isFavorite(s, path)) {
        removeFavorite(s, path);
        return false;
    }
    FavoriteEntry e;
    e.path = path;
    e.addedAt = nowIso();
    s.favorites.insert(s.favorites.begin(), std::move(e));
    return true;
}

void removeFavorite(Store& s, const std::string& path) {
    s.favorites.erase(std::remove_if(s.favorites.begin(), s.favorites.end(),
                                     [&](const FavoriteEntry& e) { return e.path == path; }),
                      s.favorites.end());
}

void hideRecent(Store& s, const std::string& path) {
    if (path.empty()) return;
    if (std::find(s.recentHidden.begin(), s.recentHidden.end(), path) == s.recentHidden.end())
        s.recentHidden.push_back(path);
}

void pruneStore(Store& s) {
    if (s.history.size() > kMaxHistory) s.history.resize(kMaxHistory);
    if (s.imported.size() > kMaxImported) s.imported.resize(kMaxImported);
}

// ---------------- 识别缓存 ----------------
void DetectCache::load(const fs::path& file) {
    map_.clear();
    lru_.clear();
    auto [jv, ok] = pxc_io::readJson(file);
    if (!ok || !jv.isObj()) return;
    const auto* ents = jv.find("entries");
    if (!ents || !ents->isObj()) return;
    for (const auto& kv : ents->obj) {
        const auto& o = kv.second;
        if (!o.isObj()) continue;
        CacheEntry e;
        const auto* sz = o.find("size");
        const auto* mt = o.find("mtime");
        e.size = (sz && sz->type == pxc_store::JsonValue::Num) ? (std::uint64_t)sz->num : 0;
        e.mtime = (mt && mt->type == pxc_store::JsonValue::Num) ? (std::int64_t)mt->num : 0;
        e.version = getStr(o, "version");
        e.format = getStr(o, "format");
        e.conf = getInt(o, "conf", 3);
        map_[kv.first] = e;
        lru_.push_back(kv.first);
    }
}

bool DetectCache::get(const std::string& path, std::uint64_t size, std::int64_t mtime,
                      CacheEntry& out) const {
    auto it = map_.find(path);
    if (it == map_.end()) return false;
    if (it->second.size != size || it->second.mtime != mtime) return false;  // 文件变了：重识别
    out = it->second;
    return true;
}

void DetectCache::set(const std::string& path, const CacheEntry& e) {
    map_[path] = e;
    if (std::find(lru_.begin(), lru_.end(), path) == lru_.end()) lru_.push_back(path);
    dirty_ = true;
    constexpr std::size_t kLimit = 5000;
    while (lru_.size() > kLimit) {
        const std::string victim = lru_.front();
        lru_.erase(lru_.begin());
        map_.erase(victim);
    }
}

bool DetectCache::save(const fs::path& file) {
    pxc_store::JsonValue root = jobj();
    put(root, "version", jnum(1));
    pxc_store::JsonValue ents = jobj();
    for (const auto& kv : map_) {
        pxc_store::JsonValue o = jobj();
        put(o, "size", jnum((double)kv.second.size));
        put(o, "mtime", jnum((double)kv.second.mtime));
        put(o, "version", jstr(kv.second.version));
        put(o, "format", jstr(kv.second.format));
        put(o, "conf", jnum((double)kv.second.conf));
        ents.obj.emplace_back(kv.first, std::move(o));
    }
    put(root, "entries", std::move(ents));
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (!pxc_io::writeText(file, pxc_store::serialize(root))) return false;
    dirty_ = false;
    return true;
}

} // namespace pxc_index
