// 轻量 JSON 实现 + 应用配置持久化。
#include "store.h"
#include "util.h"   // pxc_util::pathFromUtf8（UTF-8 窄串 -> 宽 path，避免 MSVC 窄串 GBK 解码）

#include <cctype>
#include <cstdio>
#include <ctime>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pxc_store {

// ---------------- 解析 ----------------
namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    explicit Parser(const std::string& src) : s(src) {}

    void skipWs() {
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
            // JSONC 注释：// 行注释 与 /* 块注释 */（PXC locale 文件含注释）
            if (c == '/' && i + 1 < s.size()) {
                if (s[i + 1] == '/') {
                    i += 2;
                    while (i < s.size() && s[i] != '\n') ++i;
                    continue;
                }
                if (s[i + 1] == '*') {
                    i += 2;
                    while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
                    if (i + 1 < s.size()) i += 2; // 跳过 */
                    continue;
                }
            }
            break;
        }
    }
    // 跳过 UTF-8 BOM（EF BB BF），否则首字符非 { 会解析失败。
    void skipBom() {
        if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            i = 3;
    }
    bool at(char c) { return i < s.size() && s[i] == c; }

    JsonValue parseValue() {
        skipWs();
        JsonValue v;
        if (i >= s.size()) { ok = false; return v; }
        char c = s[i];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') { v.type = JsonValue::Str; v.str = parseString(); return v; }
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') { i += 4; v.type = JsonValue::Null; return v; }
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        ok = false;
        return v;
    }

    JsonValue parseObject() {
        JsonValue v; v.type = JsonValue::Obj;
        ++i; // {
        skipWs();
        if (at('}')) { ++i; return v; }
        while (true) {
            skipWs();
            if (at('}')) { ++i; break; } // 容忍尾随逗号：逗号后直接遇到 }
            if (!at('"')) { ok = false; return v; }
            std::string key = parseString();
            skipWs();
            if (!at(':')) { ok = false; return v; }
            ++i;
            JsonValue val = parseValue();
            v.obj.emplace_back(std::move(key), std::move(val));
            skipWs();
            if (at(',')) { ++i; continue; }
            if (at('}')) { ++i; break; }
            ok = false; break;
        }
        return v;
    }

    JsonValue parseArray() {
        JsonValue v; v.type = JsonValue::Arr;
        ++i; // [
        skipWs();
        if (at(']')) { ++i; return v; }
        while (true) {
            skipWs();
            if (at(']')) { ++i; break; } // 容忍尾随逗号
            JsonValue val = parseValue();
            v.arr.push_back(std::move(val));
            skipWs();
            if (at(',')) { ++i; continue; }
            if (at(']')) { ++i; break; }
            ok = false; break;
        }
        return v;
    }

    std::string parseString() {
        ++i; // opening quote
        std::string out;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') break;
            if (c == '\\') {
                if (i >= s.size()) { ok = false; break; }
                char e = s[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (i + 4 > s.size()) { ok = false; break; }
                        unsigned code = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[i++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                            else { ok = false; break; }
                        }
                        // 仅处理 BMP 基本平面（足够覆盖中文字符与英文）
                        char buf[4];
                        if (code < 0x80) { buf[0] = (char)code; out.append(buf, 1); }
                        else if (code < 0x800) {
                            buf[0] = (char)(0xC0 | (code >> 6));
                            buf[1] = (char)(0x80 | (code & 0x3F));
                            out.append(buf, 2);
                        } else {
                            buf[0] = (char)(0xE0 | (code >> 12));
                            buf[1] = (char)(0x80 | ((code >> 6) & 0x3F));
                            buf[2] = (char)(0x80 | (code & 0x3F));
                            out.append(buf, 3);
                        }
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    JsonValue parseNumber() {
        JsonValue v; v.type = JsonValue::Num;
        size_t start = i;
        if (at('-')) ++i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' ||
               s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
        std::string lit = s.substr(start, i - start);
        try {
            v.num = std::stod(lit);
        } catch (...) { ok = false; }
        v.numFloat = lit.find('.') != std::string::npos || lit.find('e') != std::string::npos ||
                     lit.find('E') != std::string::npos;
        return v;
    }

    JsonValue parseBool() {
        JsonValue v; v.type = JsonValue::Bool;
        if (s.compare(i, 4, "true") == 0) { v.b = true; i += 4; }
        else if (s.compare(i, 5, "false") == 0) { v.b = false; i += 5; }
        else ok = false;
        return v;
    }
};

} // namespace

// ---------------- 路径 helper（相对路径落盘，docs/07 §11.2.6）----------------
namespace {
// 字符串是否表示绝对路径（Windows：盘符 C:\ 或 UNC \\）。相对路径落盘时以非盘符开头。
bool isAbsoluteU8(const std::string& u8) {
    if (u8.size() >= 2 && std::isalpha((unsigned char)u8[0]) && u8[1] == ':') return true;
    if (u8.size() >= 2 && u8[0] == '\\' && u8[1] == '\\') return true;
    return false;
}
// child 是否**严格位于** ancestor 之下（不访问磁盘，纯词法）。
// 相等 / 不可比较（跨盘）/ 不在之下都返回 false。
bool underAncestor(const std::filesystem::path& child, const std::filesystem::path& ancestor) {
    if (ancestor.empty()) return false;
    auto rel = child.lexically_relative(ancestor);
    if (rel.empty() || rel.string() == ".") return false;
    if (rel.begin()->string() == "..") return false;
    return true;
}
// 反斜杠统一成正斜杠（相对路径在 JSON 里更易读、可移植）。
std::string toSlash(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}
} // namespace

const JsonValue* JsonValue::find(const std::string& k) const {
    if (type != Obj) return nullptr;
    for (const auto& kv : obj) if (kv.first == k) return &kv.second;
    return nullptr;
}
JsonValue* JsonValue::find(const std::string& k) {
    if (type != Obj) return nullptr;
    for (auto& kv : obj) if (kv.first == k) return &kv.second;
    return nullptr;
}

std::pair<JsonValue, bool> parse(const std::string& text) {
    Parser p(text);
    p.skipBom();
    JsonValue v = p.parseValue();
    p.skipWs();
    if (p.ok && p.i != text.size()) p.ok = false; // 尾部多余字符仍判失败
    return {v, p.ok};
}

// ---------------- 序列化 ----------------
namespace {
void indentTo(std::string& out, int depth, int step) {
    for (int k = 0; k < depth * step; ++k) out += ' ';
}
void escapeString(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    out += '"';
}
void serializeValue(std::string& out, const JsonValue& v, int depth, int step) {
    switch (v.type) {
        case JsonValue::Null: out += "null"; break;
        case JsonValue::Bool: out += v.b ? "true" : "false"; break;
        case JsonValue::Num: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.10g", v.num);
            out += buf;
            // 原为浮点写法但值是整数（如 9404030.0 / 0.0）→ 补 ".0" 保持形态
            if (v.numFloat && buf[0] != '\0') {
                bool hasDot = false, hasExp = false;
                for (const char* q = buf; *q; ++q) {
                    if (*q == '.') hasDot = true;
                    if (*q == 'e' || *q == 'E') hasExp = true;
                }
                if (!hasDot && !hasExp) out += ".0";
            }
            break;
        }
        case JsonValue::Str: escapeString(out, v.str); break;
        case JsonValue::Arr: {
            if (v.arr.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t k = 0; k < v.arr.size(); ++k) {
                indentTo(out, depth + 1, step);
                serializeValue(out, v.arr[k], depth + 1, step);
                if (k + 1 < v.arr.size()) out += ",";
                out += "\n";
            }
            indentTo(out, depth, step);
            out += "]";
            break;
        }
        case JsonValue::Obj: {
            if (v.obj.empty()) { out += "{}"; break; }
            out += "{\n";
            for (size_t k = 0; k < v.obj.size(); ++k) {
                indentTo(out, depth + 1, step);
                escapeString(out, v.obj[k].first);
                out += ": ";
                serializeValue(out, v.obj[k].second, depth + 1, step);
                if (k + 1 < v.obj.size()) out += ",";
                out += "\n";
            }
            indentTo(out, depth, step);
            out += "}";
            break;
        }
    }
}
} // namespace

std::string serialize(const JsonValue& v, int indent) {
    std::string out;
    serializeValue(out, v, 0, indent);
    out += "\n";
    return out;
}

// ---------------- schema 读写 ----------------
static std::string todayIso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

static JsonValue toJson(const AppConfig& c, const std::filesystem::path& baseDir) {
    JsonValue root; root.type = JsonValue::Obj;
    root.obj.emplace_back("version", JsonValue{});
    root.obj.back().second.type = JsonValue::Num; root.obj.back().second.num = c.version;

    JsonValue reg; reg.type = JsonValue::Arr;
    for (const auto& e : c.registry) {
        JsonValue o; o.type = JsonValue::Obj;
        auto put = [&](const char* k, const std::string& val) {
            o.obj.emplace_back(k, JsonValue{});
            o.obj.back().second.type = JsonValue::Str; o.obj.back().second.str = val;
        };
        // 相对路径落盘：落在 baseDir（启动器目录）或其父级下的 PXC 写相对路径，
        // 便于和启动器一起打包；其它（Steam / itch 绝对安装）保持绝对。
        std::string outPath = e.exe_path;
        if (!baseDir.empty()) {
            std::error_code ec;
            std::filesystem::path abs =
                std::filesystem::absolute(pxc_util::pathFromUtf8(e.exe_path), ec).lexically_normal();
            if (abs.empty()) abs = pxc_util::pathFromUtf8(e.exe_path);
            // **只对落在启动器目录（含子目录）里的 PXC 写相对路径**，其它一律绝对。
            //
            // 绝不能带 baseDir.parent_path()：启动器装在盘符顶层下一层时（如 D:\PXC Launcher），
            // parent 就是 D:\ —— underAncestor 对 D 盘**任意**路径都成立，于是全盘 PXC 都被写成
            // `../Soft/itch/apps/pixel-composer/PixelComposer.exe` 这种相对形态。
            // 于是「磁盘上存的字符串」和「内存/扫描得到的字符串」不同构：读回拼接后是
            // D:\PXC Launcher\..\Soft\...，与扫描给出的 D:\Soft\... 逐字符不等，
            // findEntry 判不出是同一条 → 每次启动把这批安装整批再登记一遍。
            //
            // 这跟便携扫描范围、跟 itch/Steam 渠道都没有关系：条目可能离启动器很远、
            // 完全在扫描范围外，却因为「同盘」被写成了相对路径。同理 isPortablePath 是拿
            // 绝对路径拼出来比的，遇到相对串一律 false → 这些卡片带着「删除」项，
            // 看起来就是普通 itch 条目 —— 这正是「用户能删掉、所以不是便携版」的来源。
            if (underAncestor(abs, baseDir)) {
                auto rel = std::filesystem::proximate(abs, baseDir, ec);
                if (!rel.empty() && rel != abs) outPath = toSlash(rel.u8string());
            }
        }
        put("exe_path", outPath);
        put("product_version", e.product_version);
        put("display_name", e.display_name);
        put("source", e.source);
        put("registered_at", e.registered_at);
        // 空串也照写：channel 空 = 未判定，acked 空 = 还没确认过，两者都是有效状态，
        // 不写会让"下次读盘"和"从没写过"混为一谈。
        put("channel", e.channel);
        put("acked_version", e.acked_version);
        reg.arr.push_back(std::move(o));
    }
    root.obj.emplace_back("registry", std::move(reg));

    // 偏好表（docs/07 §11.6.2）。空串是有效值（问过但没表态），所以这里不跳过空值。
    if (!c.prefer.empty()) {
        JsonValue pr; pr.type = JsonValue::Obj;
        for (const auto& kv : c.prefer) {
            pr.obj.emplace_back(kv.first, JsonValue{});
            pr.obj.back().second.type = JsonValue::Str;
            pr.obj.back().second.str = kv.second;
        }
        root.obj.emplace_back("prefer", std::move(pr));
    }

    JsonValue loc; loc.type = JsonValue::Obj;
    auto putS = [&](const char* k, const std::string& val) {
        loc.obj.emplace_back(k, JsonValue{});
        loc.obj.back().second.type = JsonValue::Str; loc.obj.back().second.str = val;
    };
    putS("source_dir", c.locale.source_dir);
    putS("mode", c.locale.mode);
    putS("install_dir", c.locale.install_dir);
    putS("font", c.locale.font);
    JsonValue ov; ov.type = JsonValue::Obj;
    for (const auto& kv : c.locale.overrides) {
        ov.obj.emplace_back(kv.first, JsonValue{});
        ov.obj.back().second.type = JsonValue::Str; ov.obj.back().second.str = kv.second;
    }
    loc.obj.emplace_back("overrides", std::move(ov));
    root.obj.emplace_back("locale", std::move(loc));

    JsonValue bak; bak.type = JsonValue::Obj;
    bak.obj.emplace_back("enabled", JsonValue{}); bak.obj.back().second.type = JsonValue::Bool; bak.obj.back().second.b = c.backup.enabled;
    bak.obj.emplace_back("crossMajor", JsonValue{}); bak.obj.back().second.type = JsonValue::Bool; bak.obj.back().second.b = c.backup.crossMajor;
    bak.obj.emplace_back("dir", JsonValue{}); bak.obj.back().second.type = JsonValue::Str; bak.obj.back().second.str = c.backup.dir;
    root.obj.emplace_back("backup", std::move(bak));

    // ui：界面缩放 / 文本缩放（docs/06 §10.5）。数字一律写成浮点，避免 1 被读成整数。
    JsonValue ui; ui.type = JsonValue::Obj;
    ui.obj.emplace_back("scale", JsonValue{});
    ui.obj.back().second.type = JsonValue::Num; ui.obj.back().second.numFloat = true;
    ui.obj.back().second.num = c.ui.scale;
    ui.obj.emplace_back("textScale", JsonValue{});
    ui.obj.back().second.type = JsonValue::Num; ui.obj.back().second.numFloat = true;
    ui.obj.back().second.num = c.ui.text_scale;
    ui.obj.emplace_back("accent", JsonValue{});
    ui.obj.back().second.type = JsonValue::Str; ui.obj.back().second.str = c.ui.accent;
    root.obj.emplace_back("ui", std::move(ui));

    JsonValue gen; gen.type = JsonValue::Obj;
    auto putG = [&](const char* k, const std::string& val) {
        gen.obj.emplace_back(k, JsonValue{});
        gen.obj.back().second.type = JsonValue::Str; gen.obj.back().second.str = val;
    };
    auto putB = [&](const char* k, bool val) {
        gen.obj.emplace_back(k, JsonValue{});
        gen.obj.back().second.type = JsonValue::Bool; gen.obj.back().second.b = val;
    };
    putG("user_dir", c.gen.user_dir);
    putG("lang_dir", c.gen.lang_dir);
    gen.obj.emplace_back("preset", JsonValue{});
    gen.obj.back().second.type = JsonValue::Num; gen.obj.back().second.num = c.gen.preset;
    putG("ex_node_name", c.gen.ex_node_name);
    putG("ex_node_tip", c.gen.ex_node_tip);
    putG("ex_in_name", c.gen.ex_in_name);
    putG("ex_out_name", c.gen.ex_out_name);
    putG("ex_dd", c.gen.ex_dd);
    putG("ex_text", c.gen.ex_text);
    putG("ui_font", c.gen.ui_font);
    putG("code_font", c.gen.code_font);
    putG("target_version", c.gen.target_version);
    putB("auto_gen_on_launch", c.gen.auto_gen_on_launch);
    putB("write_ui", c.gen.write_ui);
    putB("write_fonts", c.gen.write_fonts);
    putB("write_notes", c.gen.write_notes);
    putG("baseline", c.gen.baseline);
    JsonValue packs; packs.type = JsonValue::Arr;
    for (const auto& p : c.gen.pack_sources) {
        JsonValue v; v.type = JsonValue::Str; v.str = p;
        packs.arr.push_back(v);
    }
    gen.obj.emplace_back("pack_sources", std::move(packs));
    root.obj.emplace_back("gen", std::move(gen));

    root.obj.emplace_back("lastFolder", JsonValue{});
    root.obj.back().second.type = JsonValue::Str; root.obj.back().second.str = c.lastFolder;
    return root;
}

// 登记表里的路径比较：先做词法归一（去 `..`/`.`/重复分隔符），再统一分隔符与大小写。
// 只在 load 的迁移里用，不进扫描热路径 —— 那边用 version_registry::samePath（轻量、无构造）。
static bool samePathLoose(const std::string& a, const std::string& b) {
    auto norm = [](const std::string& s) {
        std::error_code ec;
        std::string r = pxc_util::pathFromUtf8(s).lexically_normal().u8string();
        for (auto& ch : r) {
            if (ch == '/') ch = '\\';
            ch = (char)std::tolower((unsigned char)ch);
        }
        return r;
    };
    return norm(a) == norm(b);
}

// 两条同路径条目合并：保留信息最全的那份，不丢用户裁定过的版本号。
static void mergeEntry(ExeEntry& keep, const ExeEntry& drop) {
    if (keep.display_name.empty()) keep.display_name = drop.display_name;
    if (keep.channel.empty()) keep.channel = drop.channel;
    if (keep.registered_at.empty()) keep.registered_at = drop.registered_at;
    if (keep.acked_version.empty()) keep.acked_version = drop.acked_version;
    if (keep.product_version.empty()) keep.product_version = drop.product_version;
    // source=manual 是用户裁定的版本（刷新不覆盖），优先级高于自动读到的 PE 值。
    if (keep.source != "manual" && drop.source == "manual") {
        keep.source = "manual";
        if (!drop.product_version.empty()) keep.product_version = drop.product_version;
    }
}

static void fromJson(AppConfig& c, const JsonValue& r, const std::filesystem::path& baseDir,
                     int* dupRemoved = nullptr) {
    if (!r.isObj()) return;
    if (auto* v = r.find("version")) if (v->type == JsonValue::Num) c.version = (int)v->num;
    // 迁移判据要用**读进来的原值**（下面会把 c.version 抬高到当前版本）。
    const int rawVersion = c.version;
    if (c.version < 3) c.version = 3;   // 老配置升到 v3（只多了 ui 段，缺字段走默认，不需要迁移代码）
    if (c.version < 4) c.version = 4;   // v4：ui.textScale 从"绝对倍率"改成"相对基准的倍率"
    if (c.version < 5) c.version = 5;   // v5：registry 多 channel/acked_version，加 prefer 段（都可缺省）
    if (c.version < 6) c.version = 6;   // v6：gen 多 target_version / auto_gen_on_launch（都可缺省）
    if (auto* reg = r.find("registry")) {
        if (reg->isArr()) {
            for (const auto& o : reg->arr) {
                if (!o.isObj()) continue;
                ExeEntry e;
                auto get = [&](const char* k) -> std::string {
                    if (auto* f = o.find(k)) return f->type == JsonValue::Str ? f->str : "";
                    return "";
                };
                e.exe_path = get("exe_path");
                // 相对路径落盘 -> 按启动器目录解析回绝对（内存态始终是绝对路径）。
                //
                // **落盘格式和内存格式必须同构**，否则"同一条"在两边长得不一样，去重就永远失配：
                //   · `proximate` 在"父目录方向"写出来是 `..\PixelComposer.exe`，只做字符串拼接
                //     得到 `D:\x\launcher\..\PixelComposer.exe`，和扫描出的 `D:\x\PixelComposer.exe`
                //     逐字符不等 → 每次启动重新发现一整批（真机：卡片越重启越多）。
                //   · 只 `lexically_normal` 能修 `..`，但修不了 8.3 短名、junction、符号链接 ——
                //     同一个 exe 换一种写法进来照样判成两条。这里用 `weakly_canonical`：
                //     纯只读、文件不在时退化成词法归一，开销只在读盘这一次（不在刷新热路径）。
                if (!baseDir.empty() && !isAbsoluteU8(e.exe_path)) {
                    std::error_code ec;
                    e.exe_path =
                        (baseDir / pxc_util::pathFromUtf8(e.exe_path)).lexically_normal().u8string();
                }
                if (!e.exe_path.empty()) {
                    std::error_code ec;
                    const std::filesystem::path p = pxc_util::pathFromUtf8(e.exe_path);
                    const std::filesystem::path canon = std::filesystem::weakly_canonical(p, ec);
                    if (!ec && !canon.empty()) e.exe_path = canon.u8string();
                }
                e.product_version = get("product_version");
                e.display_name = get("display_name");
                e.source = get("source");
                e.registered_at = get("registered_at");
                e.channel = get("channel");
                e.acked_version = get("acked_version");
                if (!e.exe_path.empty()) c.registry.push_back(e);
            }
        }
    }
    // 历史数据清理：老版本把"启动器父目录下的 PXC"存成 `..\PixelComposer.exe`，读回没归一，
    // 于是每次启动都认不出这是同一条，再发现一次 —— 积累下来就是用户看到的"卡片越重启越多"。
    // 上面已把每条归一成绝对路径，这里按路径合并重复（保留信息最全的一条）。
    // 幂等：干净的数据走进来原样出去，不会动顺序。
    if (c.registry.size() > 1) {
        std::vector<ExeEntry> dedup;
        dedup.reserve(c.registry.size());
        for (auto& e : c.registry) {
            bool merged = false;
            for (auto& k : dedup) {
                if (!samePathLoose(k.exe_path, e.exe_path)) continue;
                mergeEntry(k, e);
                merged = true;
                break;
            }
            if (!merged) dedup.push_back(std::move(e));
        }
        if (dedup.size() != c.registry.size() && dupRemoved)
            *dupRemoved = (int)(c.registry.size() - dedup.size());
        c.registry.swap(dedup);
    }
    if (auto* pr = r.find("prefer")) {
        if (pr->isObj())
            for (const auto& kv : pr->obj)
                if (kv.second.type == JsonValue::Str) c.prefer[kv.first] = kv.second.str;
    }
    if (auto* loc = r.find("locale")) {
        if (loc->isObj()) {
            auto get = [&](const char* k) -> std::string {
                if (auto* f = loc->find(k)) return f->type == JsonValue::Str ? f->str : "";
                return "";
            };
            c.locale.source_dir = get("source_dir");
            c.locale.mode = get("mode");
            c.locale.install_dir = get("install_dir");
            c.locale.font = get("font");
            if (auto* ov = loc->find("overrides")) {
                if (ov->isObj()) {
                    for (const auto& kv : ov->obj)
                        if (kv.second.type == JsonValue::Str) c.locale.overrides[kv.first] = kv.second.str;
                }
            }
        }
    }
    if (auto* bak = r.find("backup")) {
        if (bak->isObj()) {
            if (auto* f = bak->find("enabled")) if (f->type == JsonValue::Bool) c.backup.enabled = f->b;
            if (auto* f = bak->find("crossMajor")) if (f->type == JsonValue::Bool) c.backup.crossMajor = f->b;
            if (auto* f = bak->find("dir")) if (f->type == JsonValue::Str) c.backup.dir = f->str;
        }
    }
    if (auto* ui = r.find("ui")) {
        if (ui->isObj()) {
            if (auto* f = ui->find("scale")) if (f->type == JsonValue::Num) c.ui.scale = (float)f->num;
            if (auto* f = ui->find("textScale")) {
                if (f->type == JsonValue::Num) {
                    c.ui.text_scale = (float)f->num;
                    // v3 及更早：textScale 是"相对原始字号"的绝对倍率；v4 起它是"相对 1.4 基准"的倍率。
                    // 不迁移的话，存过 1.3 的人升级后直接变成 1.3×1.4 = 1.82 —— 布局会碎。
                    // 除一次基准，视觉上原地不动（1.00 旧值 → 0.714 → 量化到 0.70 档，实际 0.98）。
                    // 只在**字段真的存在**时迁移：缺字段说明人是第一次跑，该走 1.0 这个新默认。
                    if (rawVersion < 4) c.ui.text_scale /= kTextBaseScale;
                }
            }
            // 只认 #RRGGBB（6 位十六进制）；写坏了就退回默认橙色，别把界面变成全黑。
            if (auto* f = ui->find("accent")) {
                if (f->type == JsonValue::Str && f->str.size() == 7 && f->str[0] == '#') {
                    auto nib = [](char ch) -> int {
                        if (ch >= '0' && ch <= '9') return ch - '0';
                        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                        return -1;
                    };
                    int v[6];
                    bool ok = true;
                    for (int i = 0; i < 6; ++i) {
                        v[i] = nib(f->str[(size_t)i + 1]);
                        if (v[i] < 0) { ok = false; break; }
                    }
                    if (ok) {
                        char buf[8];
                        for (int i = 0; i < 3; ++i) {
                            buf[i * 2 + 0] = "0123456789ABCDEF"[v[i * 2]];
                            buf[i * 2 + 1] = "0123456789ABCDEF"[v[i * 2 + 1]];
                        }
                        buf[6] = 0;
                        c.ui.accent = std::string("#") + buf;
                    }
                }
            }
        }
    }
    if (auto* gen = r.find("gen")) {
        if (gen->isObj()) {
            auto get = [&](const char* k) -> std::string {
                if (auto* f = gen->find(k)) return f->type == JsonValue::Str ? f->str : "";
                return "";
            };
            c.gen.user_dir = get("user_dir");
            c.gen.lang_dir = get("lang_dir");
            if (auto* f = gen->find("preset")) if (f->type == JsonValue::Num) c.gen.preset = (int)f->num;
            auto getDef = [&](const char* k, const std::string& def) -> std::string {
                std::string v = get(k);
                return v.empty() ? def : v;
            };
            c.gen.ex_node_name = getDef("ex_node_name", "%zh");
            c.gen.ex_node_tip = getDef("ex_node_tip", "%zh");
            c.gen.ex_in_name = getDef("ex_in_name", "%zh");
            c.gen.ex_out_name = getDef("ex_out_name", "%zh");
            c.gen.ex_dd = getDef("ex_dd", "%zh");
            c.gen.ex_text = getDef("ex_text", "%zh");
            c.gen.ui_font = get("ui_font");
            c.gen.code_font = get("code_font");
            c.gen.target_version = get("target_version");
            if (auto* f = gen->find("auto_gen_on_launch"))
                if (f->type == JsonValue::Bool) c.gen.auto_gen_on_launch = f->b;
            if (auto* f = gen->find("write_ui")) if (f->type == JsonValue::Bool) c.gen.write_ui = f->b;
            if (auto* f = gen->find("write_fonts")) if (f->type == JsonValue::Bool) c.gen.write_fonts = f->b;
            if (auto* f = gen->find("write_notes")) if (f->type == JsonValue::Bool) c.gen.write_notes = f->b;
            c.gen.baseline = get("baseline");
            if (auto* ps = gen->find("pack_sources"))
                if (ps->isArr())
                    for (const auto& v : ps->arr)
                        if (v.type == JsonValue::Str && !v.str.empty()) c.gen.pack_sources.push_back(v.str);
        }
    }
    if (auto* f = r.find("lastFolder")) if (f->type == JsonValue::Str) c.lastFolder = f->str;
}

std::pair<AppConfig, bool> load(const std::filesystem::path& path, const std::filesystem::path& baseDir,
                                int* dupRemoved) {
    AppConfig c;
    FILE* fp = _wfopen(path.wstring().c_str(), L"rb");
    if (!fp) return {c, false};
    std::string text;
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), fp)) text.append(buf, n);
    fclose(fp);
    auto [v, ok] = parse(text);
    if (!ok) return {c, false};
    fromJson(c, v, baseDir, dupRemoved);
    return {c, true};
}

bool save(const std::filesystem::path& path, const AppConfig& cfg, const std::filesystem::path& baseDir) {
    JsonValue v = toJson(cfg, baseDir);
    std::string text = serialize(v, 2);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    FILE* fp = _wfopen(path.wstring().c_str(), L"wb");
    if (!fp) return false;
    fwrite(text.data(), 1, text.size(), fp);
    fclose(fp);
    return true;
}

} // namespace pxc_store
