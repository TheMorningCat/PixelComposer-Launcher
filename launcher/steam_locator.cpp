// Steam 版 PXC 定位实现（docs/07 §11.2）。全部只读。
#include "steam_locator.h"
#include "util.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace pxc_steam {
namespace {

// Pixel Composer 的 Steam AppID。硬编码是刻意的：它就是"这个产品"的身份，
// 从 Steam 的 appinfo 动态取反而要额外解析依赖。变了改这一处（docs/07 §11.2）。
constexpr wchar_t kAppId[] = L"2299510";
constexpr wchar_t kExeName[] = L"PixelComposer.exe";
constexpr wchar_t kDefaultDir[] = L"Pixel Composer";   // acf 缺失时的固定名兜底

bool readWhole(const fs::path& p, std::string& out) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return !out.empty();
}

bool regStr(HKEY root, const wchar_t* sub, const wchar_t* name, std::wstring& out) {
    DWORD type = 0;
    DWORD bytes = 0;
    // 先问长度，再读：Steam 路径可能超过 260（长库路径）。RegGetValueW 支持。
    if (RegGetValueW(root, sub, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, nullptr, &bytes) !=
        ERROR_SUCCESS)
        return false;
    if (bytes < sizeof(wchar_t)) return false;
    std::wstring buf(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, sub, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, buf.data(),
                     &bytes) != ERROR_SUCCESS)
        return false;
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    out = buf;
    return !out.empty();
}

std::wstring lowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

// 规范化 + 去尾分隔符。vdf 里偶尔写成 "G:\\SteamLibrary\\"，直接比较会当成两个库。
fs::path normDir(const fs::path& p) {
    fs::path n = p.lexically_normal();
    std::wstring s = n.native();
    while (s.size() > 1 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return fs::path(s);
}

} // namespace

std::vector<std::string> parseLibraryPaths(const std::string& vdf) {
    std::vector<std::string> out;
    size_t i = 0;
    const size_t n = vdf.size();
    auto skipWs = [&](size_t& k) {
        while (k < n && (vdf[k] == ' ' || vdf[k] == '\t' || vdf[k] == '\r' || vdf[k] == '\n')) ++k;
    };
    while (i < n) {
        if (vdf[i] != '"') { ++i; continue; }
        // 读一个带转义的引号串
        std::string tok;
        ++i;
        while (i < n && vdf[i] != '"') {
            if (vdf[i] == '\\' && i + 1 < n) { tok.push_back(vdf[i + 1]); i += 2; }
            else tok.push_back(vdf[i++]);
        }
        if (i < n) ++i;  // 关引号
        if (tok != "path") continue;
        size_t k = i;
        skipWs(k);
        if (k >= n || vdf[k] != '"') continue;
        std::string val;
        ++k;
        while (k < n && vdf[k] != '"') {
            if (vdf[k] == '\\' && k + 1 < n) { val.push_back(vdf[k + 1]); k += 2; }
            else val.push_back(vdf[k++]);
        }
        i = (k < n) ? k + 1 : k;
        if (!val.empty()) out.push_back(val);
    }
    return out;
}

std::string acfValue(const std::string& text, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = text.find(pat);
    if (p == std::string::npos) return {};
    p += pat.size();
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n'))
        ++p;
    if (p >= text.size() || text[p] != '"') return {};
    ++p;
    const size_t e = text.find('"', p);
    if (e == std::string::npos) return {};
    return text.substr(p, e - p);
}

fs::path steamRoot() {
    std::wstring s;
    if (regStr(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", s)) return normDir(fs::path(s));
    if (regStr(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamExe", s)) {
        fs::path p(s);
        if (!p.parent_path().empty()) return normDir(p.parent_path());
    }
    if (regStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", s))
        return normDir(fs::path(s));
    return {};
}

std::vector<fs::path> libraries() {
    std::vector<fs::path> out;
    std::vector<std::wstring> seen;
    const fs::path root = steamRoot();
    if (root.empty()) return out;

    auto add = [&](const fs::path& p) {
        fs::path n = normDir(p);
        if (n.empty()) return;
        std::wstring key = lowerW(n.native());
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
        seen.push_back(key);
        out.push_back(n);
    };
    add(root);

    // 主库先读；只有它一个都没解出来时才读 config 下那份（新版客户端可能两份都有）。
    const fs::path cands[] = {root / L"steamapps" / L"libraryfolders.vdf",
                              root / L"config" / L"libraryfolders.vdf"};
    for (const auto& vdf : cands) {
        std::string text;
        if (!readWhole(vdf, text)) continue;
        const size_t before = out.size();
        for (const auto& s : parseLibraryPaths(text)) add(pxc_util::pathFromUtf8(s));
        if (out.size() > before) break;   // 解出了额外库就不再读第二份
    }
    return out;
}

std::vector<Hit> find() {
    std::vector<Hit> out;
    for (const auto& lib : libraries()) {
        const fs::path sa = lib / L"steamapps";

        Hit h;
        h.library = lib;
        std::string text;
        if (readWhole(sa / (std::wstring(L"appmanifest_") + kAppId + L".acf"), text)) {
            h.installdir = acfValue(text, "installdir");
            h.state_flags = (unsigned)std::atoi(acfValue(text, "StateFlags").c_str());
            h.last_updated = std::atoll(acfValue(text, "LastUpdated").c_str());
        }

        // 两个候选：acf 给的目录名（权威），以及固定名（acf 被删/只拷了目录时的兜底）。
        std::vector<fs::path> cands;
        if (!h.installdir.empty())
            cands.push_back(sa / L"common" / pxc_util::pathFromUtf8(h.installdir) / kExeName);
        cands.push_back(sa / L"common" / kDefaultDir / kExeName);
        for (const auto& c : cands) {
            std::error_code ec;
            if (fs::is_regular_file(c, ec) && !ec) { h.exe = c; break; }
        }
        if (!h.exe.empty()) out.push_back(h);
    }
    return out;
}

bool isUnderCommon(const fs::path& exe, const fs::path& library) {
    if (exe.empty() || library.empty()) return false;
    std::wstring e = lowerW(exe.lexically_normal().native());
    std::wstring base = lowerW((library / L"steamapps" / L"common").lexically_normal().native());
    if (base.empty()) return false;
    if (base.back() != L'\\' && base.back() != L'/') base.push_back(L'\\');
    return e.size() > base.size() && e.compare(0, base.size(), base) == 0;
}

fs::path libraryOf(const fs::path& exe, const std::vector<fs::path>& libs) {
    for (const auto& l : libs)
        if (isUnderCommon(exe, l)) return l;
    return {};
}

} // namespace pxc_steam
