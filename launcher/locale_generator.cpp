// 语言文件生成引擎 —— 实现（见 locale_generator.h 的三条硬约束）。
#include "locale_generator.h"

#include <algorithm>
#include <ctime>
#include <set>

#include "expr.h"
#include "io_util.h"
#include "log.h"
#include "util.h"

namespace pxc_gen {

namespace fs = std::filesystem;
using pxc_store::JsonValue;

// 阶段打点：生成是重 IO 流程，出问题时必须知道停在哪一步。
static void stage(const std::string& msg) { pxc_log::write("INFO", "Gen", msg); }

ExprSet ExprSet::preset(int which) {
    ExprSet e; // 默认全部 %zh
    if (which == 1) e.nodeName = "%zh_%en";       // 中文在前：混合_Blend
    else if (which == 2) e.nodeName = "%en_%zh";  // 英文在前：Blend_混合
    return e;
}

static std::string renderOf(const std::string& tmpl, const std::string& en, const std::string& zh) {
    pxc_expr::Options o;
    o.fallbackEn = true;
    o.cleanupBrackets = true;
    return pxc_expr::render(tmpl, en, zh, o);
}

// 自定义译文层命中计数（生成是单线程同步流程，用文件内静态变量即可，避免给 7 个调用点都加参数）。
static int g_ovHits = 0;

// 端口名的「归一化」形式：转小写 + 连续空格压成一个 + 去首尾空格。
// 对照表里的键是 `nodes.<id>.in.<英文端口名>.name` 直接拼出来的，所以同一端口在不同版本里
// 大小写/空格写法一变（`Angle Range` ↔ `Angle range`）就成了两条不同的键。
// **只对端口键用它**（§8.1.6）：节点键是 id、words/ui 本来就是归一化键，再套一层会误命中。
static std::string normPortName(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    bool prevSpace = false;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!o.empty() && !prevSpace) {
                o += ' ';
                prevSpace = true;
            }
            continue;
        }
        prevSpace = false;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        o += c;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    if (o == s) return {};   // 与原文相同 = 归一化没有新信息，不必二次查表
    return o;
}

static int g_normHits = 0;

// 查表 + 渲染。命中译文返回 true。
// 顺序：**自定义译文层 → 对照表（精确键）→ 对照表（归一化键，仅端口键）**。
// 自定义译文层只存用户显式填过的键，所以两者不会互相污染。
static bool fieldKey(const pxc_tdb::Db& db, const std::map<std::string, std::string>* ov,
                     const std::string& key, const std::string& tmpl,
                     const std::string& en, std::string& out) {
    if (ov) {
        auto it = ov->find(key);
        if (it != ov->end() && !it->second.empty()) {
            out = renderOf(tmpl, en, it->second);
            ++g_ovHits;
            return true;
        }
    }
    const pxc_tdb::Entry* e = pxc_tdb::find(db, key);
    std::string zh = (e && !e->zh.empty()) ? e->zh : std::string();
    out = renderOf(tmpl, en, zh);
    return !zh.empty();
}

// altKey 为空 = 没有归一化候选（节点键 / words / ui / notes 都是这样）。
static bool field(const pxc_tdb::Db& db, const std::map<std::string, std::string>* ov,
                  const std::string& key, const std::string& tmpl,
                  const std::string& en, std::string& out,
                  const std::string& altKey = std::string()) {
    if (fieldKey(db, ov, key, tmpl, en, out)) return true;
    if (altKey.empty()) return false;
    if (!fieldKey(db, ov, altKey, tmpl, en, out)) return false;
    ++g_normHits;
    return true;
}

static void noteMissing(Stats& st, const std::string& key) {
    if ((int)st.missingKeys.size() < 2000) st.missingKeys.push_back(key);
}

static bool copyFileTo(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(pxc_util::toExtended(src), pxc_util::toExtended(dst),
                  fs::copy_options::overwrite_existing, ec);
    return !ec;
}

static std::string stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return std::string(buf);
}

// 默认字体槽位表（骨架没有 fonts.json 时用；取自官方 sample locale 的槽位与字号）
static const std::vector<std::pair<std::string, int>>& defaultSlots() {
    static const std::vector<std::pair<std::string, int>> v = {
        {"h1", 48}, {"h2", 32}, {"h3", 20}, {"h5", 16},
        {"p0", 12}, {"p0b", 12}, {"p1", 11}, {"p1b", 11},
        {"p2", 10}, {"p2b", 10}, {"code", 12}, {"sdf", 16}};
    return v;
}

static bool writeFonts(const fs::path& outLang, const pxc_base::Baseline& base,
                       const FontPick& font, Stats& st) {
    std::error_code ec;
    fs::path fontsDir = outLang / "fonts";
    fs::create_directories(fontsDir, ec);

    std::string uiName = pxc_util::toUtf8(font.uiFont.filename().wstring().c_str());
    std::string codeName = font.codeFont.empty()
                               ? uiName
                               : pxc_util::toUtf8(font.codeFont.filename().wstring().c_str());
    if (uiName.empty()) return false;
    // 字体文件名可能是中文（用户自装字体）——必须走 UTF-8→宽路径，不能直接拼
    if (!copyFileTo(font.uiFont, pxc_util::joinUtf8(fontsDir, uiName))) return false;
    if (!font.codeFont.empty() && !codeName.empty())
        copyFileTo(font.codeFont, pxc_util::joinUtf8(fontsDir, codeName));

    // 槽位表：优先骨架的 fonts.json（保留 size），替换 path
    JsonValue slots;
    bool haveSlots = false;
    if (base.hasFonts && !base.fontsDir.empty()) {
        auto [v, ok] = pxc_io::readJson(base.fontsDir / "fonts.json");
        if (ok && v.isObj()) {
            slots = v;
            haveSlots = true;
        }
    }
    if (!haveSlots) {
        slots.type = JsonValue::Obj;
        for (auto& s : defaultSlots()) {
            JsonValue o;
            o.type = JsonValue::Obj;
            JsonValue p;
            p.type = JsonValue::Str;
            p.str = "./" + uiName;
            o.obj.emplace_back("path", p);
            JsonValue sz;
            sz.type = JsonValue::Num;
            sz.num = s.second;
            o.obj.emplace_back("size", sz);
            slots.obj.emplace_back(s.first, o);
        }
    } else {
        for (auto& kv : slots.obj) {
            if (!kv.second.isObj()) continue;
            bool isCode = false;
            std::string lower = kv.first;
            for (auto& c : lower)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (lower.find("code") != std::string::npos || lower.find("mono") != std::string::npos)
                isCode = true;
            if (auto* p = kv.second.find("path")) {
                p->type = JsonValue::Str;
                p->str = "./" + (isCode ? codeName : uiName);
            }
        }
    }
    if (!pxc_io::writeJson(fontsDir / "fonts.json", slots)) return false;
    st.files++;
    return true;
}

Stats generate(const pxc_base::Baseline& base, const pxc_tdb::Db& db, const ExprSet& ex,
               const FontPick& font, const Options& opt, const fs::path& outRoot,
               const std::string& langDir) {
    Stats st;
    g_ovHits = 0;
    g_normHits = 0;
    if (!base.hasNodes && !base.hasWords) {
        st.error = "英文骨架为空（既没有 nodes.json 也没有 words.json）。";
        return st;
    }
    if (langDir.empty()) {
        st.error = "语言文件夹名为空（在启动器配置里填写，如 zh）。";
        return st;
    }

    std::error_code ec;
    fs::path tmpRoot = outRoot / ".pxc_gen_tmp";
    fs::remove_all(tmpRoot, ec); // 自建的浅层目录，安全
    // langDir 是用户输入的 UTF-8 串：直接 path / string 会按 GBK 解码并抛
    // std::system_error 1113（这正是"点生成就报编码错误"的根因）
    fs::path tmp = pxc_util::joinUtf8(tmpRoot, langDir);
    fs::create_directories(tmp, ec);
    stage("开始生成 -> " + pxc_util::toUtf8(tmp.wstring().c_str()) +
          " | 骨架节点 " + std::to_string((int)base.nodes.obj.size()) +
          " 词条 " + std::to_string((int)base.words.obj.size()) +
          " 对照表 " + std::to_string((int)db.entries.size()));

    // ---------------- nodes.json ----------------
    if (base.hasNodes) {
        JsonValue out;
        out.type = JsonValue::Obj;
        for (auto kv : base.nodes.obj) {
            const std::string& id = kv.first;
            JsonValue node = kv.second; // 拷贝，结构照抄
            bool anyZh = false;
            if (node.isObj()) {
                if (auto* v = node.find("name"); v && v->isStr() && !v->str.empty()) {
                    std::string s;
                    st.fields++;
                    if (field(db, opt.overrides, pxc_tdb::kNodeName(id), ex.nodeName, v->str, s)) {
                        anyZh = true;
                        st.translated++;
                    } else {
                        st.fallbackEn++;
                        noteMissing(st, pxc_tdb::kNodeName(id));
                    }
                    if (!s.empty()) v->str = s;
                }
                if (auto* v = node.find("tooltip"); v && v->isStr() && !v->str.empty()) {
                    std::string s;
                    st.fields++;
                    if (field(db, opt.overrides, pxc_tdb::kNodeTip(id), ex.nodeTip, v->str, s)) {
                        anyZh = true;
                        st.translated++;
                    } else {
                        st.fallbackEn++;
                        noteMissing(st, pxc_tdb::kNodeTip(id));
                    }
                    if (!s.empty()) v->str = s;
                }
                for (const char* arrName : {"inputs", "outputs"}) {
                    JsonValue* arr = node.find(arrName);
                    if (!arr || !arr->isArr()) continue;
                    bool isIn = (arrName[0] == 'i');
                    for (auto& item : arr->arr) {
                        if (!item.isObj()) continue;
                        const JsonValue* nameV = item.find("name");
                        if (!nameV || !nameV->isStr() || nameV->str.empty()) continue;
                        const std::string portEn = nameV->str; // 英文端口名 = 锚
                        {
                            std::string s;
                            st.fields++;
                            std::string key = isIn ? pxc_tdb::kInName(id, portEn)
                                                   : pxc_tdb::kOutName(id, portEn);
                            const std::string np = normPortName(portEn);
                            const std::string alt =
                                np.empty() ? std::string()
                                           : (isIn ? pxc_tdb::kInName(id, np)
                                                   : pxc_tdb::kOutName(id, np));
                            if (field(db, opt.overrides, key, isIn ? ex.inName : ex.outName, portEn, s, alt)) {
                                anyZh = true;
                                st.translated++;
                            } else {
                                st.fallbackEn++;
                                noteMissing(st, key);
                            }
                            if (!s.empty()) item.find("name")->str = s;
                        }
                        if (isIn)
                            if (auto* t = item.find("tooltip"); t && t->isStr() && !t->str.empty()) {
                                std::string s;
                                st.fields++;
                                std::string key = pxc_tdb::kInTip(id, portEn);
                                const std::string np = normPortName(portEn);
                                if (field(db, opt.overrides, key, ex.nodeTip, t->str, s,
                                          np.empty() ? std::string() : pxc_tdb::kInTip(id, np))) {
                                    anyZh = true;
                                    st.translated++;
                                } else {
                                    st.fallbackEn++;
                                    noteMissing(st, key);
                                }
                                if (!s.empty()) t->str = s;
                            }
                        JsonValue* dd = item.find("display_data");
                        if (!dd || !dd->isArr()) continue;
                        for (auto& e : dd->arr) {
                            if (!e.isStr() || e.str.empty()) continue; // -1.0 / @ref 原样保留
                            std::string s;
                            st.fields++;
                            std::string key = pxc_tdb::kDd(id, e.str);
                            if (field(db, opt.overrides, key, ex.dd, e.str, s)) {
                                anyZh = true;
                                st.translated++;
                            } else {
                                st.fallbackEn++;
                                noteMissing(st, key);
                            }
                            if (!s.empty()) e.str = s;
                        }
                    }
                }
            }
            out.obj.emplace_back(id, node);
            st.nodes++;
            if (!anyZh) st.missingNodes++;
        }
        if (!pxc_io::writeJson(tmp / "nodes.json", out)) {
            st.error = "写 nodes.json 失败。";
            return st;
        }
        st.files++;
    }

    stage("nodes.json 完成：节点 " + std::to_string(st.nodes) + " 翻译命中 " + std::to_string(st.translated));

    // ---------------- words.json ----------------
    if (base.hasWords) {
        JsonValue out;
        out.type = JsonValue::Obj;
        for (auto kv : base.words.obj) {
            JsonValue v = kv.second;
            if (v.isStr() && !v.str.empty()) {
                std::string s;
                st.fields++;
                if (field(db, opt.overrides, pxc_tdb::kWord(kv.first), ex.text, v.str, s))
                    st.translated++;
                else {
                    st.fallbackEn++;
                    noteMissing(st, pxc_tdb::kWord(kv.first));
                }
                if (!s.empty()) v.str = s;
            }
            out.obj.emplace_back(kv.first, v);
            st.words++;
        }
        if (!pxc_io::writeJson(tmp / "words.json", out)) {
            st.error = "写 words.json 失败。";
            return st;
        }
        st.files++;
    }

    stage("words.json 完成：词条 " + std::to_string(st.words));

    // ---------------- UI.json ----------------
    {
        JsonValue out;
        out.type = JsonValue::Obj;
        std::set<std::string> seen;
        if (base.hasUi) {
            for (auto kv : base.ui.obj) {
                JsonValue v = kv.second;
                if (v.isStr() && !v.str.empty()) {
                    std::string s;
                    st.fields++;
                    if (field(db, opt.overrides, pxc_tdb::kUi(kv.first), ex.text, v.str, s))
                        st.translated++;
                    else
                        st.fallbackEn++;
                    if (!s.empty()) v.str = s;
                }
                out.obj.emplace_back(kv.first, v);
                seen.insert(kv.first);
                st.ui++;
            }
        }
        // 补充键：对照表里 ui.* 有、骨架没有的（1.22 官方 en 不带 UI.json）
        for (const auto& kv : db.entries) {
            if (kv.first.rfind("ui.", 0) != 0) continue;
            std::string key = kv.first.substr(3);
            if (seen.count(key)) continue;
            // 注意：这里也必须走 field()，否则补充键会绕过自定义译文层（曾漏掉这一处）
            std::string s;
            field(db, opt.overrides, kv.first, ex.text, kv.second.en, s);
            if (s.empty()) continue;
            JsonValue v;
            v.type = JsonValue::Str;
            v.str = s;
            out.obj.emplace_back(key, v);
            seen.insert(key);
            st.ui++;
            st.translated++;
        }
        if (opt.writeUi && !out.obj.empty()) {
            if (!pxc_io::writeJson(tmp / "UI.json", out)) {
                st.error = "写 UI.json 失败。";
                return st;
            }
            st.files++;
        }
    }

    stage("UI.json 完成：键 " + std::to_string(st.ui));

    // ---------------- junctions.json + notes ----------------
    if (base.hasJunctions) {
        JsonValue j = base.junctions; // 照抄结构
        std::map<std::string, std::string> notesMap;
        if (opt.writeNotes && !opt.notesZhDir.empty()) {
            auto [mv, ok] = pxc_io::readJson(opt.notesZhDir / "_map.json");
            if (ok && mv.isObj())
                for (auto& kv : mv.obj)
                    if (kv.second.isStr()) notesMap[kv.first] = kv.second.str;
        }
        // note 文件名：有中文映射就改指向
        if (!notesMap.empty()) {
            for (auto& kv : j.obj) {
                if (!kv.second.isObj()) continue;
                for (auto& port : kv.second.obj) {
                    if (!port.second.isObj()) continue;
                    if (auto* n = port.second.find("note"); n && n->isStr()) {
                        auto it = notesMap.find(n->str);
                        if (it != notesMap.end()) n->str = it->second;
                    }
                }
            }
        }
        if (!pxc_io::writeJson(tmp / "junctions.json", j)) {
            st.error = "写 junctions.json 失败。";
            return st;
        }
        st.files++;
    }

    if (opt.writeNotes && base.hasNotes && !base.dir.empty()) {
        fs::path srcNotes = base.dir / "notes";
        fs::path dstNotes = tmp / "notes";
        fs::create_directories(dstNotes, ec);
        std::map<std::string, std::string> notesMap;
        if (!opt.notesZhDir.empty()) {
            auto [mv, ok] = pxc_io::readJson(opt.notesZhDir / "_map.json");
            if (ok && mv.isObj())
                for (auto& kv : mv.obj)
                    if (kv.second.isStr()) notesMap[kv.first] = kv.second.str;
        }
        for (auto& it : fs::directory_iterator(srcNotes, ec)) {
            if (!it.is_regular_file(ec)) continue;
            std::string name = pxc_util::toUtf8(it.path().filename().wstring().c_str());
            // 自定义译文层：整篇 markdown 覆盖（键 notes.<英文文件名>，值就是全文）
            if (opt.overrides) {
                auto oi = opt.overrides->find(pxc_tdb::kNotes(name));
                if (oi != opt.overrides->end() && !oi->second.empty()) {
                    if (pxc_io::writeText(pxc_util::joinUtf8(dstNotes, name), oi->second)) {
                        ++st.files;
                        ++g_ovHits;
                        continue;
                    }
                }
            }
            // 对照表里的整篇译文（构建器维护的 notes.<文件名>）：自定义译文之后、外部中文目录之前。
            // 少了这一步，构建器里编辑的注释文档会被骨架英文原样盖掉 —— 等于白编辑。
            if (const pxc_tdb::Entry* e = pxc_tdb::find(db, pxc_tdb::kNotes(name)); e && !e->zh.empty()) {
                if (pxc_io::writeText(pxc_util::joinUtf8(dstNotes, name), e->zh)) {
                    ++st.files;
                    continue;
                }
            }
            auto mi = notesMap.find(name);
            if (mi != notesMap.end()) {
                fs::path zh = pxc_util::joinUtf8(opt.notesZhDir, mi->second);
                if (copyFileTo(zh, pxc_util::joinUtf8(dstNotes, mi->second))) continue; // 中文版优先
            }
            copyFileTo(it.path(), pxc_util::joinUtf8(dstNotes, name));
            st.files++;
        }
    }

    stage("junctions/notes 完成");

    // ---------------- config.json ----------------
    {
        JsonValue cfg = base.hasConfig ? base.config : JsonValue{};
        cfg.type = JsonValue::Obj;
        JsonValue* pc = cfg.find("per_character_line_break");
        if (!pc) {
            JsonValue v;
            v.type = JsonValue::Bool;
            v.b = true;
            cfg.obj.emplace_back("per_character_line_break", v);
        } else if (opt.perCharacterLineBreak) {
            pc->type = JsonValue::Bool;
            pc->b = true;
        }
        if (!pxc_io::writeJson(tmp / "config.json", cfg)) {
            st.error = "写 config.json 失败。";
            return st;
        }
        st.files++;
    }

    stage("config.json 完成");

    // ---------------- fonts ----------------
    if (opt.writeFonts && !font.uiFont.empty()) writeFonts(tmp, base, font, st);

    stage("fonts 完成");

    // ---------------- 自检 ----------------
    {
        for (const char* fn : {"nodes.json", "words.json", "UI.json", "junctions.json", "config.json"}) {
            fs::path p = tmp / fn;
            if (!fs::exists(p, ec)) continue;
            auto [v, ok] = pxc_io::readJson(p);
            if (!ok) {
                st.error = std::string("自检失败：") + fn + " 无法解析。";
                fs::remove_all(tmpRoot, ec);
                return st;
            }
        }
        if (base.hasNodes) {
            auto [v, ok] = pxc_io::readJson(tmp / "nodes.json");
            if (!ok || (int)v.obj.size() != (int)base.nodes.obj.size()) {
                st.error = "自检失败：节点数与骨架不一致。";
                fs::remove_all(tmpRoot, ec);
                return st;
            }
            // 类型一致：逐节点比对 name/tooltip 类型与 inputs/outputs 长度
            for (size_t i = 0; i < v.obj.size() && i < base.nodes.obj.size(); ++i) {
                const JsonValue& a = base.nodes.obj[i].second;
                const JsonValue& b = v.obj[i].second;
                if (a.type != b.type) {
                    st.error = "自检失败：节点 " + v.obj[i].first + " 类型与骨架不一致。";
                    fs::remove_all(tmpRoot, ec);
                    return st;
                }
                if (!a.isObj()) continue;
                for (const char* f : {"name", "tooltip"}) {
                    const JsonValue* xa = a.find(f);
                    const JsonValue* xb = b.find(f);
                    if ((xa == nullptr) != (xb == nullptr)) {
                        st.error = std::string("自检失败：字段缺失 ") + f;
                        fs::remove_all(tmpRoot, ec);
                        return st;
                    }
                    if (xa && xa->type != xb->type) {
                        st.error = std::string("自检失败：字段类型变化 ") + f;
                        fs::remove_all(tmpRoot, ec);
                        return st;
                    }
                }
                for (const char* arrName : {"inputs", "outputs"}) {
                    const JsonValue* xa = a.find(arrName);
                    const JsonValue* xb = b.find(arrName);
                    if (!xa || !xb) continue;
                    if (xa->arr.size() != xb->arr.size()) {
                        st.error = std::string("自检失败：端口数量变化 ") + arrName;
                        fs::remove_all(tmpRoot, ec);
                        return st;
                    }
                    for (size_t k = 0; k < xa->arr.size(); ++k) {
                        const JsonValue* da = xa->arr[k].find("display_data");
                        const JsonValue* dbv = xb->arr[k].find("display_data");
                        if (da && dbv && da->arr.size() != dbv->arr.size()) {
                            st.error = "自检失败：下拉项数量变化。";
                            fs::remove_all(tmpRoot, ec);
                            return st;
                        }
                    }
                }
            }
        }
    }

    stage("自检通过，准备替换目标目录");

    // ---------------- 备份旧目录 + 原子替换 ----------------
    fs::path dest = pxc_util::joinUtf8(outRoot, langDir);
    if (fs::exists(dest, ec)) {
        fs::path bakRoot = outRoot / ".pxc_backup";
        fs::create_directories(bakRoot, ec);
        fs::path bak = pxc_util::joinUtf8(bakRoot, langDir + "_" + stamp());
        std::error_code rc;
        fs::rename(dest, bak, rc); // 同盘，等价于移动
        if (rc) {
            st.error = "备份旧语言目录失败：" + pxc_util::toUtf8(dest.wstring().c_str());
            fs::remove_all(tmpRoot, ec);
            return st;
        }
    }
    std::error_code rc;
    fs::create_directories(outRoot, rc);
    fs::rename(tmp, dest, rc);
    if (rc) {
        st.error = "替换到目标目录失败：" + pxc_util::toUtf8(dest.wstring().c_str());
        return st;
    }
    fs::remove_all(tmpRoot, ec);

    stage("生成完成 -> " + pxc_util::toUtf8(dest.wstring().c_str()));
    st.ok = true;
    st.ovHits = g_ovHits;
    st.normHits = g_normHits;
    if (g_normHits > 0) stage("端口归一化命中 " + std::to_string(g_normHits) + " 条");
    st.outDir = pxc_util::toUtf8(dest.wstring().c_str());
    return st;
}

} // namespace pxc_gen
