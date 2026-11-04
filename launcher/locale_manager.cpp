// 汉化管理实现。
#include "locale_manager.h"
#include "store.h"
#include "log.h"
#include "util.h"

#include <cstdio>

namespace pxc_locale {

static bool looksLikeLangDir(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    return std::filesystem::exists(dir / "words.json", ec) ||
           std::filesystem::exists(dir / "nodes.json", ec);
}

// 递归复制目录：用 Win32 FindFirstFileW + 扩展长度路径（\\?\），
// 既突破 260 限制，又避免 std::filesystem::directory_iterator 在超长/嵌套路径上的不稳定行为。
// depth 上限防止病态/循环结构无限递归。
static bool copyTree(const std::filesystem::path& from, const std::filesystem::path& to, int depth) {
    if (depth > 64) {
        pxc_log::write("ERROR", "Locale", "copyTree 递归深度超限，疑似循环结构: " + to.u8string());
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(pxc_util::toExtended(to), ec);
    std::wstring pat = pxc_util::toExtended(from).wstring() + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        pxc_log::write("WARN", "Locale", "遍历跳过（路径过长/无权限/空目录）: " + from.u8string());
        return true; // 空目录或不可访问：不致命
    }
    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::filesystem::path childFrom = pxc_util::toExtended(from) / fd.cFileName;
        std::filesystem::path childTo   = to / fd.cFileName;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir) {
            if (!copyTree(childFrom, childTo, depth + 1)) { ok = false; break; }
        } else {
            std::error_code ec2;
            std::filesystem::copy_file(pxc_util::toExtended(childFrom), pxc_util::toExtended(childTo),
                                      std::filesystem::copy_options::overwrite_existing, ec2);
            if (ec2) {
                pxc_log::write("ERROR", "Locale", "复制失败: " + childFrom.u8string() + " -> " + childTo.u8string() +
                               " | " + ec2.message());
                ok = false; break;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

bool copyDir(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    if (!std::filesystem::is_directory(from, ec)) {
        pxc_log::write("WARN", "Locale", "copyDir 源不是目录: " + from.u8string());
        return false;
    }
    return copyTree(from, to, 0);
}

std::filesystem::path locateLangDir(const std::filesystem::path& sourceDir, const std::string& mode) {
    std::error_code ec;
    if (!std::filesystem::exists(sourceDir, ec)) return {};
    // 自身即语言目录
    if (looksLikeLangDir(sourceDir)) return sourceDir;

    // 否则尝试子目录：mode -> zh -> 任意含 locale 的子目录
    std::vector<std::filesystem::path> candidates;
    std::string m = mode.empty() ? "zh" : mode;
    candidates.push_back(pxc_util::joinUtf8(sourceDir, m));
    candidates.push_back(sourceDir / "zh");
    for (const auto& e : std::filesystem::directory_iterator(sourceDir, ec)) {
        if (e.is_directory(ec)) candidates.push_back(e.path());
    }
    for (const auto& c : candidates) {
        if (looksLikeLangDir(c)) return c;
    }
    return {};
}

bool deploy(const std::filesystem::path& langDir, const std::filesystem::path& installDir,
            const std::string& mode) {
    if (!looksLikeLangDir(langDir)) {
        pxc_log::write("ERROR", "Locale", "源语言目录无效（缺 words.json/nodes.json）: " + langDir.u8string());
        return false;
    }
    std::filesystem::path target = installDir / "data" / "locale" / pxc_util::pathFromUtf8(mode);
    // 递归复制并覆盖（手动递归 + 扩展长度路径，突破 260 限制且保证终止）
    bool copied = copyTree(langDir, target, 0);
    if (!copied) {
        pxc_log::write("ERROR", "Locale", "部署失败: " + langDir.u8string() + " -> " + target.u8string());
        return false;
    }
    pxc_log::write("INFO", "Locale", "已部署语言目录: " + langDir.u8string() + " -> " + target.u8string());
    return true;
}

std::vector<std::string> listFonts(const std::filesystem::path& langDir) {
    std::vector<std::string> out;
    std::error_code ec;
    std::filesystem::path fdir = langDir / "fonts";
    if (!std::filesystem::exists(fdir, ec)) return out;
    for (const auto& e : std::filesystem::directory_iterator(fdir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string ext = e.path().extension().u8string();
        if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") out.push_back(e.path().filename().u8string());
    }
    return out;
}

static std::string readFile(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return "";
    // 用宽路径打开：MSVC 对窄字符串按 GBK 解码，中文路径会打不开。
    FILE* fp = _wfopen(p.wstring().c_str(), L"rb");
    if (!fp) return "";
    std::string s; char buf[8192];
    while (size_t n = fread(buf, 1, sizeof(buf), fp)) s.append(buf, n);
    fclose(fp);
    return s;
}
static bool writeFile(const std::filesystem::path& p, const std::string& s) {
    // _wfopen 宽路径打开，避免中文路径 GBK 解码失败。
    FILE* fp = _wfopen(p.wstring().c_str(), L"wb");
    if (!fp) return false;
    fwrite(s.data(), 1, s.size(), fp);
    fclose(fp);
    return true;
}

bool applyOverrides(const std::filesystem::path& wordsJsonPath,
                    const std::map<std::string, std::string>& overrides) {
    if (overrides.empty()) return true;
    std::string text = readFile(wordsJsonPath);
    if (text.empty()) {
        pxc_log::write("WARN", "Locale", "words.json 读取为空，跳过覆盖: " + wordsJsonPath.u8string());
        return false;
    }
    auto [v, ok] = pxc_store::parse(text);
    if (!ok || !v.isObj()) {
        pxc_log::write("WARN", "Locale", "words.json 解析失败，跳过覆盖（部署仍生效）: " + wordsJsonPath.u8string());
        return false;
    }
    int applied = 0;
    for (const auto& kv : overrides) {
        pxc_store::JsonValue* f = v.find(kv.first);
        if (f && f->type == pxc_store::JsonValue::Str) { f->str = kv.second; ++applied; }
        else if (f && f->type == pxc_store::JsonValue::Null) { f->type = pxc_store::JsonValue::Str; f->str = kv.second; ++applied; }
    }
    if (applied == 0) {
        pxc_log::write("INFO", "Locale", "覆盖层中无匹配键，未修改 words.json");
        return true;
    }
    std::string out = pxc_store::serialize(v, 2);
    bool wrote = writeFile(wordsJsonPath, out);
    pxc_log::write(wrote ? "INFO" : "ERROR", "Locale",
                   "覆盖层已合并到 words.json: 应用 " + std::to_string(applied) + " 项 | " + wordsJsonPath.u8string());
    return wrote;
}

bool setFont(const std::filesystem::path& fontsJsonPath, const std::string& ttfName) {
    if (ttfName.empty()) return true;
    std::string text = readFile(fontsJsonPath);
    if (text.empty()) return false;
    auto [v, ok] = pxc_store::parse(text);
    if (!ok || !v.isObj()) {
        pxc_log::write("WARN", "Locale", "fonts.json 解析失败，跳过字体替换");
        return false;
    }
    std::string path = "./" + ttfName;
    int changed = 0;
    for (auto& kv : v.obj) {
        if (kv.second.isObj()) {
            if (auto* p = kv.second.find("path")) {
                if (p->type == pxc_store::JsonValue::Str) { p->str = path; ++changed; }
            }
        }
    }
    std::string out = pxc_store::serialize(v, 2);
    bool wrote = writeFile(fontsJsonPath, out);
    pxc_log::write(wrote ? "INFO" : "ERROR", "Locale",
                   "字体已替换: " + ttfName + " | 槽位 " + std::to_string(changed) + " 个 | " + fontsJsonPath.u8string());
    return wrote;
}

} // namespace pxc_locale
