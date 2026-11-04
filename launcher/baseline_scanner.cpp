// 英文骨架扫描器 —— 实现（口径见 baseline_scanner.h 与 docs/08 §8.1）。
#include "baseline_scanner.h"

#include <algorithm>
#include <cstdio>

#include "io_util.h"
#include "locale_zip.h"
#include "log.h"
#include "util.h"
#include "version_registry.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef small

namespace pxc_base {

namespace fs = std::filesystem;
using pxc_store::JsonValue;

namespace {

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && fs::is_regular_file(p, ec);
}

bool dirExists(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

int countNodesIn(const fs::path& dir) {
    auto [v, ok] = pxc_io::readJson(dir / "nodes.json");
    if (!ok || !v.isObj()) return -1;
    return (int)v.obj.size();
}

std::string u8(const fs::path& p) { return pxc_util::toUtf8(p.wstring().c_str()); }

// 语言目录 -> 版本猜测（看自身/父目录名里的数字串，如 "1.22"）
std::string guessVerFor(const fs::path& langDir) {
    fs::path p = langDir;
    for (int i = 0; i < 3 && !p.empty(); ++i, p = p.parent_path()) {
        std::string name = pxc_util::toUtf8(p.filename().wstring().c_str());
        std::string digits;
        for (size_t k = 0; k < name.size(); ++k) {
            if (name[k] >= '0' && name[k] <= '9') {
                digits += name[k];
                if (k + 1 < name.size() && name[k + 1] == '.') digits += '.';
            } else if (!digits.empty() && digits.back() != '.') {
                if (digits.size() >= 3) return digits;
                digits.clear();
            }
        }
        if (digits.size() >= 3) return digits;
    }
    return {};
}

// `Locale/version` 里那个整数编码（主×100000 + 次×1000 + 修订×10 + 构建）-> 四段版本号。
// 本机实测 118050 = 1.18.5.0（docs/08 §8.1.5）。
std::string decodeVersionCode(long long code) {
    if (code <= 0) return {};
    const long long major = code / 100000;
    long long rem = code % 100000;
    const long long minor = rem / 1000;
    rem %= 1000;
    const long long patch = rem / 10;
    const long long build = rem % 10;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%lld.%lld.%lld.%lld", major, minor, patch, build);
    return std::string(buf);
}

std::string readUserDirVersion(const fs::path& userDir) {
    auto [v, ok] = pxc_io::readJson(userDir / "Locale" / "version");
    if (!ok || !v.isObj()) return {};
    const JsonValue* n = v.find("version");
    if (!n) return {};
    if (n->type == JsonValue::Num) return decodeVersionCode((long long)n->num);
    if (n->isStr()) {
        long long code = 0;
        for (char c : n->str) {
            if (c < '0' || c > '9') {
                code = 0;
                break;
            }
            code = code * 10 + (c - '0');
        }
        return decodeVersionCode(code);
    }
    return {};
}

// ---- Steam：zip 解压到缓存目录，内容没变就不重解（docs/08 §8.1.7.3）----

struct ZipStamp {
    long long mtime = 0;
    unsigned long long size = 0;
};

ZipStamp zipStampOf(const fs::path& zip) {
    ZipStamp s;
    std::error_code ec;
    auto t = fs::last_write_time(zip, ec);
    if (!ec) s.mtime = (long long)t.time_since_epoch().count();
    auto sz = fs::file_size(zip, ec);
    if (!ec) s.size = (unsigned long long)sz;
    return s;
}

// 缓存目录里那份指纹是否与当前 zip 一致（一致且 en/nodes.json 在 = 直接复用）
bool stampValid(const fs::path& stampFile, const ZipStamp& now) {
    auto [v, ok] = pxc_io::readJson(stampFile);
    if (!ok || !v.isObj()) return false;
    const JsonValue* m = v.find("zip_mtime");
    const JsonValue* s = v.find("zip_size");
    if (!m || !s || m->type != JsonValue::Num || s->type != JsonValue::Num) return false;
    return (long long)m->num == now.mtime && (unsigned long long)s->num == now.size;
}

bool writeStamp(const fs::path& stampFile, const ZipStamp& now) {
    JsonValue v;
    v.type = JsonValue::Obj;
    auto put = [&](const char* k, double d) {
        JsonValue x;
        x.type = JsonValue::Num;
        x.num = d;
        x.numFloat = false;
        v.obj.emplace_back(k, x);
    };
    put("zip_mtime", (double)now.mtime);
    put("zip_size", (double)now.size);
    return pxc_io::writeJson(stampFile, v);
}

// 把 zip 里的 prefix 解到 <cacheRoot>/<version>/<子目录名>；内容没变则跳过。
// 返回落成后的目录（失败返回空）。
fs::path ensureZipDir(const fs::path& zip, const std::string& prefix, const fs::path& cacheRoot,
                      const std::string& version, const std::string& subName, bool withFonts) {
    if (version.empty() || cacheRoot.empty()) return {};
    const fs::path verDir = pxc_util::joinUtf8(cacheRoot, version);
    const fs::path dest = pxc_util::joinUtf8(verDir, subName);
    const fs::path stampFile = verDir / ".source";
    const ZipStamp now = zipStampOf(zip);
    if (stampValid(stampFile, now) && fileExists(dest / "nodes.json")) return dest;

    std::string err;
    const int n = pxc_zip::extractDir(zip, prefix, dest, withFonts, err);
    if (n <= 0) {
        pxc_log::write("WARN", "Base", "locale.zip 解压失败（" + prefix + "）：" + err);
        return {};
    }
    writeStamp(stampFile, now);
    pxc_log::write("INFO", "Base", "locale.zip -> " + u8(dest) + "（" + std::to_string(n) + " 个文件）");
    return dest;
}

// 骨架目录里有 en/words 但没 fonts/ 时，从**同一版本**的 sample locale 补槽位表。
// 只补 fonts.json 这一个文件，不搬字体本体（字体由用户在下拉里选）。
void fillFontsFromSibling(Baseline& b, const fs::path& baseLocaleDir, const fs::path& zip,
                          const fs::path& cacheRoot, const std::string& version) {
    if (b.hasFonts) return;
    if (!baseLocaleDir.empty()) {
        const fs::path f = baseLocaleDir / "sample locale" / "fonts";
        if (fileExists(f / "fonts.json")) {
            b.hasFonts = true;
            b.fontsDir = f;
            return;
        }
    }
    if (!zip.empty() && fileExists(zip)) {
        const fs::path dst = pxc_util::joinUtf8(pxc_util::joinUtf8(cacheRoot, version),
                                                "sample locale/fonts/fonts.json");
        std::string err;
        if (pxc_zip::extractOne(zip, "sample locale/fonts/fonts.json", dst, err)) {
            b.hasFonts = true;
            b.fontsDir = dst.parent_path();
        }
    }
}

} // namespace

bool isDegradedOrigin(const std::string& origin) {
    return origin == "user-en" || origin == "user-sample" || origin == "user-missing" ||
           origin == "fallback";
}

std::vector<Candidate> candidatesFor(const fs::path& installDir, const std::string& version,
                                     const fs::path& cacheRoot) {
    std::vector<Candidate> out;
    if (installDir.empty()) return out;
    const std::string ver = version.empty() ? guessVerFor(installDir) : version;

    auto pushPlain = [&](const fs::path& d, const char* origin) {
        if (!fileExists(d / "nodes.json")) return;
        Candidate c;
        c.dir = d;
        c.origin = origin;
        c.version = ver;
        c.source = d;
        out.push_back(c);
    };
    pushPlain(installDir / "data" / "locale" / "en", "install-en");
    pushPlain(installDir / "data" / "locale" / "sample locale", "install-sample");

    // Steam：没有 data/ 明文，语言在 pack/locale.zip
    const fs::path zip = installDir / "pack" / "locale.zip";
    if (fileExists(zip)) {
        if (fs::path d = ensureZipDir(zip, "en/", cacheRoot, ver, "en", false); !d.empty()) {
            Candidate c;
            c.dir = d;
            c.origin = "install-zip";
            c.version = ver;
            c.source = zip;
            out.push_back(c);
        }
        if (fs::path d = ensureZipDir(zip, "sample locale/", cacheRoot, ver, "sample locale", true);
            !d.empty()) {
            Candidate c;
            c.dir = d;
            c.origin = "install-zip-sample";
            c.version = ver;
            c.source = zip;
            out.push_back(c);
        }
    }
    return out;
}

bool load(const Candidate& c, Baseline& out) {
    out.dir = c.dir;
    out.origin = c.origin;
    out.version = c.version;
    out.sourcePath = c.source;

    if (auto [v, ok] = pxc_io::readJson(c.dir / "nodes.json"); ok && v.isObj()) {
        out.nodes = v;
        out.hasNodes = true;
        out.nodeCount = (int)v.obj.size();
    }
    if (auto [v, ok] = pxc_io::readJson(c.dir / "words.json"); ok && v.isObj()) {
        out.words = v;
        out.hasWords = true;
    }
    if (auto [v, ok] = pxc_io::readJson(c.dir / "UI.json"); ok && v.isObj()) {
        out.ui = v;
        out.hasUi = true;
    }
    if (auto [v, ok] = pxc_io::readJson(c.dir / "junctions.json"); ok && v.isObj()) {
        out.junctions = v;
        out.hasJunctions = true;
    }
    if (auto [v, ok] = pxc_io::readJson(c.dir / "config.json"); ok && v.isObj()) {
        out.config = v;
        out.hasConfig = true;
    }
    out.hasNotes = dirExists(c.dir / "notes");
    if (dirExists(c.dir / "fonts") && fileExists(c.dir / "fonts" / "fonts.json")) {
        out.hasFonts = true;
        out.fontsDir = c.dir / "fonts";
    }
    return out.hasNodes || out.hasWords;
}

std::vector<InstalledVersion> scanInstalled(const std::vector<pxc_store::ExeEntry>& registry,
                                            const fs::path& cacheRoot, bool withCounts) {
    std::vector<InstalledVersion> out;
    for (const auto& e : registry) {
        if (e.exe_path.empty() || e.product_version.empty()) continue;
        const fs::path exe = pxc_util::pathFromUtf8(e.exe_path);
        const fs::path installDir = installDirOfExe(exe);
        auto cands = candidatesFor(installDir, e.product_version, cacheRoot);
        InstalledVersion v;
        v.version = pxc_reg::normalizeVersion(e.product_version);
        v.exePath = e.exe_path;
        v.installDir = installDir;
        v.channel = e.channel;
        v.hasSkeleton = !cands.empty();
        if (!cands.empty()) {
            v.origin = cands.front().origin;
            if (withCounts) v.nodeCount = countNodesIn(cands.front().dir);
        }
        // 同版本多渠道合并成一行：itch 优先，其次有骨架的那条
        auto same = std::find_if(out.begin(), out.end(), [&](const InstalledVersion& x) {
            return pxc_reg::sameVersion(x.version, v.version);
        });
        if (same == out.end()) {
            out.push_back(std::move(v));
        } else {
            const bool better = (!same->hasSkeleton && v.hasSkeleton) ||
                                (same->hasSkeleton == v.hasSkeleton &&
                                 same->channel != "itch" && v.channel == "itch");
            if (better) *same = std::move(v);
        }
    }
    std::sort(out.begin(), out.end(), [](const InstalledVersion& a, const InstalledVersion& b) {
        return pxc_reg::compareVersion(a.version, b.version) > 0;  // 版本降序
    });
    return out;
}

namespace {

std::string highestInstalledVersion(const Request& r) {
    auto list = scanInstalled(r.registry, r.cacheRoot, false);
    for (const auto& v : list)
        if (v.hasSkeleton) return v.version;
    return {};
}

// 在 registry 里按版本挑第一个能拿到骨架的候选
bool tryVersion(const Request& r, const std::string& version, Baseline& out) {
    for (const auto& e : r.registry) {
        if (!pxc_reg::sameVersion(e.product_version, version)) continue;
        const fs::path installDir = installDirOfExe(pxc_util::pathFromUtf8(e.exe_path));
        for (const auto& c : candidatesFor(installDir, pxc_reg::normalizeVersion(e.product_version),
                                           r.cacheRoot)) {
            Baseline b;
            if (load(c, b)) {
                out = std::move(b);
                return true;
            }
        }
    }
    return false;
}

// 用户目录三份（只在降级时用）。返回 true 表示拿到。
bool tryUserDir(const Request& r, Baseline& out) {
    const fs::path loc = r.userDir / "Locale";
    const std::string ver = readUserDirVersion(r.userDir);

    if (fileExists(loc / "en" / "nodes.json")) {
        Candidate c;
        c.dir = loc / "en";
        c.origin = "user-en";
        c.version = ver.empty() ? "?" : ver;   // 能与 Locale/version 对上才敢写版本
        c.source = c.dir;
        Baseline b;
        if (load(c, b)) {
            out = std::move(b);
            return true;
        }
    }
    if (fileExists(loc / "sample locale" / "nodes.json")) {
        Candidate c;
        c.dir = loc / "sample locale";
        c.origin = "user-sample";
        c.version = "?";   // 与 Locale/version 没有绑定关系（更早一次解包留下的）
        c.source = c.dir;
        Baseline b;
        if (load(c, b)) {
            out = std::move(b);
            return true;
        }
    }
    // nodes_missing.json 结构就是 nodes.json，但它是**差集**（docs/08 §8.1.5.1），
    // 所以只当最后一档的整份骨架，绝不并进别的骨架。
    if (fileExists(loc / "nodes_missing.json")) {
        auto [v, ok] = pxc_io::readJson(loc / "nodes_missing.json");
        if (ok && v.isObj() && !v.obj.empty()) {
            Baseline b;
            b.dir = loc;
            b.origin = "user-missing";
            b.version = "?";
            b.sourcePath = loc / "nodes_missing.json";
            b.nodes = std::move(v);
            b.hasNodes = true;
            b.nodeCount = (int)b.nodes.obj.size();
            out = std::move(b);
            return true;
        }
    }
    return false;
}

} // namespace

Baseline resolve(const Request& r) {
    Baseline base;
    std::string target = r.targetVersion.empty() ? highestInstalledVersion(r)
                                                 : pxc_reg::normalizeVersion(r.targetVersion);
    if (target.empty()) {
        base.origin = "fallback";
        base.note = "没有可用的 PXC 版本 —— 先在版本库登记一个 PXC。";
        return base;
    }

    // 主链：目标版本自己的安装目录（含 Steam zip）
    if (tryVersion(r, target, base)) {
        // 同一版本目录里补字体槽位表（唯一允许的跨目录补缺，见 docs/08 §8.1.3）
        if (!base.hasFonts) {
            for (const auto& e : r.registry) {
                if (!pxc_reg::sameVersion(e.product_version, target)) continue;
                const fs::path installDir = installDirOfExe(pxc_util::pathFromUtf8(e.exe_path));
                fillFontsFromSibling(base, installDir / "data" / "locale",
                                     installDir / "pack" / "locale.zip", r.cacheRoot, target);
                if (base.hasFonts) break;
            }
        }
        base.note = "骨架 " + base.version + "（" + base.origin + "），节点 " +
                    std::to_string(base.nodeCount);
        return base;
    }

    // 降级链（§8.1.5）：≥目标的最低 → <目标的最高
    std::vector<std::string> others;
    for (const auto& v : scanInstalled(r.registry, r.cacheRoot, false))
        if (v.hasSkeleton && !pxc_reg::sameVersion(v.version, target)) others.push_back(v.version);
    std::sort(others.begin(), others.end(), pxc_reg::compareVersion);   // 升序
    std::vector<std::string> order;
    for (const auto& v : others)
        if (pxc_reg::compareVersion(v, target) > 0) order.push_back(v);  // 先向上，最低优先
    for (auto it = others.rbegin(); it != others.rend(); ++it)
        if (pxc_reg::compareVersion(*it, target) < 0) order.push_back(*it);  // 再向下，最高优先

    for (const auto& v : order) {
        if (tryVersion(r, v, base)) {
            base.note = "目标 " + target + " 的骨架取不到，改用 " + v + "（" +
                        base.origin + "），节点 " + std::to_string(base.nodeCount);
            pxc_log::write("WARN", "Base", base.note);
            return base;
        }
    }

    if (tryUserDir(r, base)) {
        base.note = "目标 " + target + " 无骨架，用用户目录那份（" + base.origin + "，版本 " +
                    base.version + "），节点 " + std::to_string(base.nodeCount);
        pxc_log::write("WARN", "Base", base.note);
        return base;
    }

    base.origin = "fallback";
    base.note = "找不到可用的英文骨架（目标 " + target +
                " 的安装目录、pack/locale.zip、用户目录都没有）—— 先去版本库登记一个 PXC。";
    pxc_log::write("WARN", "Base", base.note);
    return base;
}

Baseline resolveForExe(const fs::path& exe, const std::string& version,
                       const std::vector<pxc_store::ExeEntry>& registry,
                       const fs::path& userDir, const fs::path& cacheRoot) {
    Request r;
    r.userDir = userDir;
    r.cacheRoot = cacheRoot;
    r.targetVersion = version;
    for (const auto& e : registry) {
        // 让主链只认这个 exe：其它条目版本对不上，tryVersion 自然不会命中；
        // 但如果同一个版本另有一份能用的副本，用它也没错（内容同版本）。
        r.registry.push_back(e);
    }
    if (!exe.empty()) {
        const fs::path installDir = installDirOfExe(exe);
        for (const auto& c : candidatesFor(installDir, version, cacheRoot)) {
            Baseline b;
            if (load(c, b)) {
                if (!b.hasFonts)
                    fillFontsFromSibling(b, installDir / "data" / "locale",
                                         installDir / "pack" / "locale.zip", cacheRoot, version);
                b.note = "骨架 " + b.version + "（" + b.origin + "），节点 " +
                         std::to_string(b.nodeCount);
                return b;
            }
        }
    }
    return resolve(r);
}

std::vector<fs::path> findUserDirs() {
    std::vector<fs::path> out;
    auto add = [&](const fs::path& d) {
        if (d.empty()) return;
        std::error_code ec;
        if (!fs::is_directory(d, ec)) return;   // 判据放宽到「目录自身存在」（§8.7.2）
        if (std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
    };

    // 1) 环境变量：LOCALAPPDATA / APPDATA（宽字符读取，避免中文用户名被 GBK 解码错）
    auto envWide = [](const wchar_t* name) -> fs::path {
        wchar_t buf[1024] = {0};
        DWORD n = GetEnvironmentVariableW(name, buf, 1024);
        if (n == 0 || n >= 1024) return {};
        return fs::path(std::wstring(buf, n));
    };
    const fs::path localAppData = envWide(L"LOCALAPPDATA");
    const fs::path appData = envWide(L"APPDATA");

    // 2) persistPreference.json 记录的自定义用户目录
    for (const fs::path& root : {localAppData, appData}) {
        if (root.empty()) continue;
        auto [txt, ok] = pxc_io::readText(root / "PixelComposer" / "persistPreference.json");
        if (!ok) continue;
        auto [jv, okj] = pxc_store::parse(txt);
        if (!okj || !jv.isObj()) continue;
        if (const JsonValue* p = jv.find("path"); p && p->isStr() && !p->str.empty())
            add(pxc_util::pathFromUtf8(p->str));
    }

    // 3) 默认位置
    add(localAppData / "PixelComposer");
    add(appData / "PixelComposer");
    return out;
}

bool looksLikeUserDir(const fs::path& dir) {
    if (dir.empty()) return false;
    return dirExists(dir / "Locale") || dirExists(dir / "Preferences");
}

std::filesystem::path installDirOfExe(const std::filesystem::path& exe) {
    if (exe.empty()) return {};
    return exe.parent_path();
}

} // namespace pxc_base
