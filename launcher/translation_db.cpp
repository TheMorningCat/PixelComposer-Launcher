// 汉化对照表 —— 实现（见 translation_db.h 的设计说明）。
#include "translation_db.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "io_util.h"
#include "util.h"

namespace pxc_tdb {

using pxc_store::JsonValue;
namespace fs = std::filesystem;

// ---------------- 键 ----------------
std::string kWord(const std::string& k) { return "words." + k; }
std::string kUi(const std::string& k) { return "ui." + k; }
std::string kNodeName(const std::string& id) { return "nodes." + id + ".name"; }
std::string kNodeTip(const std::string& id) { return "nodes." + id + ".tip"; }
std::string kInName(const std::string& id, const std::string& p) { return "nodes." + id + ".in." + p + ".name"; }
std::string kInTip(const std::string& id, const std::string& p) { return "nodes." + id + ".in." + p + ".tip"; }
std::string kOutName(const std::string& id, const std::string& p) { return "nodes." + id + ".out." + p + ".name"; }
std::string kDd(const std::string& id, const std::string& e) { return "nodes." + id + ".dd." + e; }
std::string kNotes(const std::string& f) { return "notes." + f; }

// ---------------- 不译词表 ----------------
const std::vector<std::string>& defaultKeepEnglish() {
    static const std::vector<std::string> v = {
        // 专业名词与技术缩写（1.22 汉化说明第五节的惯例）
        "Phong", "PBR", "Lanczos3", "LUT", "SDF", "HLSL", "FFT", "MP4", "SVG", "JPEG", "JPG",
        "PNG", "GIF", "WEBP", "TIFF", "BMP", "TGA", "EXR", "PXC", "ACE", "UV", "UVW",
        "RGB", "RGBA", "HSV", "HSL", "OKLAB", "OKLCH", "CMYK", "BBOX", "AA", "FXAA",
        "ASCII", "UTF", "JSON", "XML", "CSV", "HTTP", "URL", "ID", "OK", "FPS", "DPI",
        // 单字母轴与常用代号
        "X", "Y", "Z", "W", "H", "R", "G", "B", "A", "N", "S", "T", "P", "Q", "U", "V"
    };
    return v;
}

bool keepEnglish(const Db& db, const std::string& text) {
    if (text.empty()) return false;
    const auto& list = db.keepEnglish.empty() ? defaultKeepEnglish() : db.keepEnglish;
    for (const auto& w : list)
        if (text == w) return true;
    return false;
}

const Entry* find(const Db& db, const std::string& key) {
    auto it = db.entries.find(key);
    return it == db.entries.end() ? nullptr : &it->second;
}

// ---------------- 键形变体（词典兜底用） ----------------
static std::string lowerAscii(const std::string& s) {
    std::string r = s;
    for (auto& c : r)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return r;
}
static std::string spacesToUnderscore(const std::string& s) {
    std::string r = s;
    for (auto& c : r)
        if (c == ' ') c = '_';
    return r;
}
static std::string stripSpaces(const std::string& s) {
    std::string r;
    for (char c : s)
        if (c != ' ' && c != '_' && c != '-') r += c;
    return r;
}
static const std::string* dictLookup(const std::map<std::string, std::string>& dict,
                                     const std::string& en) {
    auto it = dict.find(en);
    if (it != dict.end()) return &it->second;
    it = dict.find(lowerAscii(en));
    if (it != dict.end()) return &it->second;
    it = dict.find(lowerAscii(spacesToUnderscore(en)));
    if (it != dict.end()) return &it->second;
    it = dict.find(lowerAscii(stripSpaces(en)));
    if (it != dict.end()) return &it->second;
    return nullptr;
}

// 汉化包的短标签：若末级目录是语言名（zh / ce-zh / zh_pro …），取「上一级/语言名」，
// 否则取目录名本身。对照表随程序分发，不能写维护者的绝对路径。
std::string packLabel(const fs::path& pack) {
    std::string leaf = pxc_util::toUtf8(pack.filename().wstring().c_str());
    std::string lower = lowerAscii(leaf);
    bool isLang = lower == "zh" || lower == "ce-zh" || lower == "ec-zh" || lower == "en" ||
                  lower == "sample locale" ||
                  (lower.rfind("zh", 0) == 0 && lower.size() <= 16);
    if (isLang && pack.has_parent_path()) {
        std::string up = pxc_util::toUtf8(pack.parent_path().filename().wstring().c_str());
        if (up == "Locale") up = pxc_util::toUtf8(pack.parent_path().parent_path().filename().wstring().c_str());
        if (!up.empty()) return up + "/" + leaf;
    }
    return leaf.empty() ? std::string("(pack)") : leaf;
}

// ---------------- 填表 ----------------
// 先到先得：已有译文不覆盖（incremental 时天然保留旧译文）。
static bool fill(Db& db, const std::string& key, const std::string& en, const std::string& zh,
                 const std::string& src, BuildStats& st, bool fromDict) {
    if (zh.empty()) return false;
    auto it = db.entries.find(key);
    if (it == db.entries.end()) {
        Entry e;
        e.en = en;
        e.zh = zh;
        e.src = src;
        db.entries.emplace(key, e);
        st.newKeys++;
        if (fromDict) st.fromDict++; else st.fromAlign++;
        return true;
    }
    if (!it->second.zh.empty()) return false;
    it->second.zh = zh;
    it->second.src = src;
    if (it->second.en.empty()) it->second.en = en;
    if (fromDict) st.fromDict++; else st.fromAlign++;
    return true;
}

// ---------------- 构建 ----------------
BuildStats build(Db& db, const fs::path& baselineDir, const std::vector<fs::path>& packs,
                 bool incremental) {
    BuildStats st;
    (void)incremental; // 保留语义：已有译文一律不动（fill 的先到先得已保证）

    auto [bNodes, okNodes] = pxc_io::readJson(baselineDir / "nodes.json");
    auto [bWords, okWords] = pxc_io::readJson(baselineDir / "words.json");
    auto [bUi, okUi] = pxc_io::readJson(baselineDir / "UI.json");
    if (!okNodes && !okWords) return st;

    db.baseline = packLabel(baselineDir);  // 短标签，不写绝对路径
    if (db.keepEnglish.empty()) db.keepEnglish = defaultKeepEnglish();

    // 1) 骨架可译条目数（覆盖率分母）
    if (okWords && bWords.isObj())
        for (auto& kv : bWords.obj)
            if (kv.second.isStr() && !kv.second.str.empty() && !keepEnglish(db, kv.second.str)) st.baselineTotal++;
    if (okUi && bUi.isObj())
        for (auto& kv : bUi.obj)
            if (kv.second.isStr() && !kv.second.str.empty() && !keepEnglish(db, kv.second.str)) st.baselineTotal++;
    if (okNodes && bNodes.isObj()) {
        st.nodesTotal = (int)bNodes.obj.size();
        for (auto& kv : bNodes.obj) {
            if (!kv.second.isObj()) continue;
            if (auto* v = kv.second.find("name"); v && v->isStr() && !v->str.empty() && !keepEnglish(db, v->str)) st.baselineTotal++;
            if (auto* v = kv.second.find("tooltip"); v && v->isStr() && !v->str.empty()) st.baselineTotal++;
            for (const char* arr : {"inputs", "outputs"}) {
                if (auto* a = kv.second.find(arr); a && a->isArr()) {
                    for (auto& item : a->arr) {
                        if (!item.isObj()) continue;
                        if (auto* v = item.find("name"); v && v->isStr() && !v->str.empty() && !keepEnglish(db, v->str)) st.baselineTotal++;
                        if (auto* v = item.find("tooltip"); v && v->isStr() && !v->str.empty()) st.baselineTotal++;
                        if (auto* d = item.find("display_data"); d && d->isArr())
                            for (auto& e : d->arr)
                                if (e.isStr() && !e.str.empty() && !keepEnglish(db, e.str)) st.baselineTotal++;
                    }
                }
            }
        }
    }

    // 2) 逐包对齐（packs 下标 0 优先级最高）
    std::map<std::string, std::string> dict; // 英 -> 中（跨包累积，先到先得）
    std::set<std::string> alignedNodes;

    for (size_t pi = 0; pi < packs.size(); ++pi) {
        const fs::path& pack = packs[pi];
        // src 存「短标签」而不是绝对路径：对照表是随程序分发的资源，不该带维护者的机器路径。
        std::string src = packLabel(pack);

        auto [pWords, okPW] = pxc_io::readJson(pack / "words.json");
        auto [pUi, okPU] = pxc_io::readJson(pack / "UI.json");
        auto [pNodes, okPN] = pxc_io::readJson(pack / "nodes.json");

        // 2a) words：直接对齐 + 建词典
        if (okPW && pWords.isObj() && okWords && bWords.isObj()) {
            for (auto& kv : pWords.obj) {
                if (!kv.second.isStr()) continue;
                const JsonValue* enV = bWords.find(kv.first);
                std::string en = (enV && enV->isStr()) ? enV->str : std::string();
                if (en.empty() || en == kv.second.str) continue; // 骨架没有 / 未翻
                if (keepEnglish(db, en)) continue;
                dict.emplace(en, kv.second.str);
                fill(db, kWord(kv.first), en, kv.second.str, src, st, false);
            }
        }

        // 2b) UI：骨架可能没有 UI.json（1.22 官方 en 就没有），此时 en 留空
        if (okPU && pUi.isObj()) {
            for (auto& kv : pUi.obj) {
                if (!kv.second.isStr() || kv.second.str.empty()) continue;
                const JsonValue* enV = (okUi && bUi.isObj()) ? bUi.find(kv.first) : nullptr;
                std::string en = (enV && enV->isStr()) ? enV->str : std::string();
                if (!en.empty() && en == kv.second.str) continue;
                if (!en.empty() && keepEnglish(db, en)) continue;
                fill(db, kUi(kv.first), en, kv.second.str, src, st, false);
            }
        }

        // 2c) nodes
        if (okPN && pNodes.isObj() && okNodes && bNodes.isObj()) {
            for (auto& kv : pNodes.obj) {
                const std::string& id = kv.first;
                const JsonValue* enNode = bNodes.find(id);
                if (!enNode || !enNode->isObj()) continue;
                const JsonValue& zhNode = kv.second;
                if (!zhNode.isObj()) continue;
                bool aligned = false;

                auto pairStr = [&](const JsonValue& a, const JsonValue& b, const char* field,
                                   const std::string& key) {
                    const JsonValue* av = a.find(field);
                    const JsonValue* bv = b.find(field);
                    if (!av || !bv || !av->isStr() || !bv->isStr()) return;
                    if (av->str.empty() || av->str == bv->str) return;
                    if (keepEnglish(db, av->str)) return;
                    if (fill(db, key, av->str, bv->str, src, st, false)) aligned = true;
                };

                pairStr(*enNode, zhNode, "name", kNodeName(id));
                pairStr(*enNode, zhNode, "tooltip", kNodeTip(id));

                for (const char* arrName : {"inputs", "outputs"}) {
                    const JsonValue* aArr = enNode->find(arrName);
                    const JsonValue* bArr = zhNode.find(arrName);
                    if (!aArr || !bArr || !aArr->isArr() || !bArr->isArr()) continue;
                    if (aArr->arr.size() != bArr->arr.size()) continue; // 结构变了，不信任下标
                    for (size_t i = 0; i < aArr->arr.size(); ++i) {
                        const JsonValue& aItem = aArr->arr[i];
                        const JsonValue& bItem = bArr->arr[i];
                        if (!aItem.isObj() || !bItem.isObj()) continue;
                        const JsonValue* aName = aItem.find("name");
                        if (!aName || !aName->isStr() || aName->str.empty()) continue;
                        bool isIn = (arrName[0] == 'i');
                        pairStr(aItem, bItem, "name",
                                isIn ? kInName(id, aName->str) : kOutName(id, aName->str));
                        if (isIn) pairStr(aItem, bItem, "tooltip", kInTip(id, aName->str));

                        const JsonValue* aDd = aItem.find("display_data");
                        const JsonValue* bDd = bItem.find("display_data");
                        if (!aDd || !bDd || !aDd->isArr() || !bDd->isArr()) continue;
                        if (aDd->arr.size() != bDd->arr.size()) continue;
                        for (size_t j = 0; j < aDd->arr.size(); ++j) {
                            if (!aDd->arr[j].isStr() || !bDd->arr[j].isStr()) continue; // -1.0 等跳过
                            if (aDd->arr[j].str.empty() || aDd->arr[j].str == bDd->arr[j].str) continue;
                            if (keepEnglish(db, aDd->arr[j].str)) continue;
                            if (fill(db, kDd(id, aDd->arr[j].str), aDd->arr[j].str, bDd->arr[j].str,
                                     src, st, false))
                                aligned = true;
                        }
                    }
                }
                if (aligned) alignedNodes.insert(id);
            }
        }
    }
    st.nodesAligned = (int)alignedNodes.size();

    // 3) 词典兜底：节点级没对齐到的，用 words 词典补
    if (okWords && bWords.isObj()) {
        for (auto& kv : bWords.obj) {
            if (!kv.second.isStr() || kv.second.str.empty()) continue;
            const std::string& en = kv.second.str;
            if (keepEnglish(db, en)) continue;
            if (auto* e = find(db, kWord(kv.first)); e && !e->zh.empty()) continue;
            if (const std::string* zh = dictLookup(dict, en))
                fill(db, kWord(kv.first), en, *zh, std::string(), st, true);
        }
    }
    if (okNodes && bNodes.isObj()) {
        for (auto& kv : bNodes.obj) {
            if (!kv.second.isObj()) continue;
            const std::string& id = kv.first;
            auto fillByDict = [&](const std::string& key, const std::string& en) {
                if (en.empty() || keepEnglish(db, en)) return;
                if (auto* e = find(db, key); e && !e->zh.empty()) return;
                if (const std::string* zh = dictLookup(dict, en)) fill(db, key, en, *zh, std::string(), st, true);
            };
            if (auto* v = kv.second.find("name"); v && v->isStr()) fillByDict(kNodeName(id), v->str);
            if (auto* v = kv.second.find("tooltip"); v && v->isStr()) fillByDict(kNodeTip(id), v->str);
            for (const char* arrName : {"inputs", "outputs"}) {
                if (auto* a = kv.second.find(arrName); a && a->isArr()) {
                    for (auto& item : a->arr) {
                        if (!item.isObj()) continue;
                        const JsonValue* n = item.find("name");
                        if (!n || !n->isStr() || n->str.empty()) continue;
                        bool isIn = (arrName[0] == 'i');
                        fillByDict(isIn ? kInName(id, n->str) : kOutName(id, n->str), n->str);
                        if (isIn)
                            if (auto* t = item.find("tooltip"); t && t->isStr()) fillByDict(kInTip(id, n->str), t->str);
                        if (auto* d = item.find("display_data"); d && d->isArr())
                            for (auto& e : d->arr)
                                if (e.isStr()) fillByDict(kDd(id, e.str), e.str);
                    }
                }
            }
        }
    }

    st.entries = (int)db.entries.size();
    st.translated = st.entries;
    return st;
}

// ---------------- 存取 ----------------
bool save(const fs::path& path, const Db& db) {
    JsonValue root;
    root.type = JsonValue::Obj;
    auto put = [](JsonValue& o, const std::string& k, const std::string& v) {
        JsonValue s;
        s.type = JsonValue::Str;
        s.str = v;
        o.obj.emplace_back(k, s);
    };
    JsonValue meta;
    meta.type = JsonValue::Obj;
    put(meta, "version", db.version);   // 对照表版本（分发时唯一需要看的版本）
    put(meta, "baseline", db.baseline);
    put(meta, "built_at", db.builtAt);
    if (!db.pendingReview.empty()) {   // 空列表不写，避免给所有表加噪音
        JsonValue pend;
        pend.type = JsonValue::Arr;
        std::set<std::string> uniq(db.pendingReview.begin(), db.pendingReview.end());
        for (const auto& k : uniq) {
            JsonValue v;
            v.type = JsonValue::Str;
            v.str = k;
            pend.arr.push_back(v);
        }
        meta.obj.emplace_back("pending_review", pend);
    }
    if (!db.pendingTranslate.empty()) {  // 同上：没有待翻译的键就不写这个字段
        JsonValue pend;
        pend.type = JsonValue::Arr;
        std::set<std::string> uniq(db.pendingTranslate.begin(), db.pendingTranslate.end());
        for (const auto& k : uniq) {
            JsonValue v;
            v.type = JsonValue::Str;
            v.str = k;
            pend.arr.push_back(v);
        }
        meta.obj.emplace_back("pending_translate", pend);
    }
    if (!db.addedKeys.empty()) {  // 只在"刚导入了新版本英文包"之后存在
        JsonValue add;
        add.type = JsonValue::Arr;
        for (const auto& k : db.addedKeys) {
            JsonValue v;
            v.type = JsonValue::Str;
            v.str = k;
            add.arr.push_back(v);
        }
        meta.obj.emplace_back("added_keys", add);
    }
    root.obj.emplace_back("meta", meta);

    JsonValue keep;
    keep.type = JsonValue::Arr;
    for (const auto& w : db.keepEnglish) {
        JsonValue v;
        v.type = JsonValue::Str;
        v.str = w;
        keep.arr.push_back(v);
    }
    root.obj.emplace_back("keep_english", keep);

    JsonValue ents;
    ents.type = JsonValue::Obj;
    for (const auto& kv : db.entries) {
        if (kv.second.zh.empty()) continue; // 只存有译文的
        JsonValue e;
        e.type = JsonValue::Obj;
        put(e, "en", kv.second.en);
        put(e, "zh", kv.second.zh);
        // 不写 src：来源包属于构建期细节，写进分发资源只会让人误以为有版本语义。
        ents.obj.emplace_back(kv.first, e);
    }
    root.obj.emplace_back("entries", ents);

    return pxc_io::writeJson(path, root);
}

std::pair<Db, bool> load(const fs::path& path) {
    Db db;
    auto [root, ok] = pxc_io::readJson(path);
    if (!ok || !root.isObj()) return {db, false};
    if (auto* meta = root.find("meta"); meta && meta->isObj()) {
        if (auto* v = meta->find("version"); v && v->isStr()) db.version = v->str;
        if (auto* v = meta->find("baseline"); v && v->isStr()) db.baseline = v->str;
        if (auto* v = meta->find("built_at"); v && v->isStr()) db.builtAt = v->str;
        if (auto* v = meta->find("pending_review"); v && v->isArr())
            for (auto& k : v->arr)
                if (k.isStr()) db.pendingReview.push_back(k.str);
        if (auto* v = meta->find("pending_translate"); v && v->isArr())
            for (auto& k : v->arr)
                if (k.isStr()) db.pendingTranslate.push_back(k.str);
        if (auto* v = meta->find("added_keys"); v && v->isArr())
            for (auto& k : v->arr)
                if (k.isStr()) db.addedKeys.push_back(k.str);
    }
    if (auto* keep = root.find("keep_english"); keep && keep->isArr())
        for (auto& w : keep->arr)
            if (w.isStr()) db.keepEnglish.push_back(w.str);
    if (db.keepEnglish.empty()) db.keepEnglish = defaultKeepEnglish();
    if (auto* ents = root.find("entries"); ents && ents->isObj()) {
        for (auto& kv : ents->obj) {
            if (!kv.second.isObj()) continue;
            Entry e;
            if (auto* v = kv.second.find("en"); v && v->isStr()) e.en = v->str;
            if (auto* v = kv.second.find("zh"); v && v->isStr()) e.zh = v->str;
            if (auto* v = kv.second.find("src"); v && v->isStr()) e.src = v->str;
            if (!e.zh.empty()) db.entries.emplace(kv.first, e);
        }
    }
    return {db, true};
}

// ---------------- 目录 / 版本 ----------------
fs::path locateLangDir(const fs::path& dir, const std::string& prefer) {
    auto isLang = [](const fs::path& d) {
        std::error_code ec;
        return std::filesystem::exists(d / "nodes.json", ec) || std::filesystem::exists(d / "words.json", ec);
    };
    std::error_code ec;
    if (isLang(dir)) return dir;
    fs::path preferDir = pxc_util::pathFromUtf8(prefer);
    for (const fs::path& cand : {dir / "Locale" / preferDir, dir / "Locale" / "zh", dir / preferDir,
                                 dir / "zh", dir / "Locale" / "en", dir / "en"}) {
        if (isLang(cand)) return cand;
    }
    for (auto& it : std::filesystem::directory_iterator(dir, ec)) {
        if (it.is_directory(ec) && isLang(it.path())) return it.path();
    }
    return {};
}

std::string guessVersion(const fs::path& langDir) {
    for (const char* fn : {"vesion", "version", "Version"}) {
        auto [txt, ok] = pxc_io::readText(langDir / fn);
        if (!ok) continue;
        std::string t = txt;
        auto eq = t.find('=');
        if (eq != std::string::npos) t = t.substr(eq + 1);
        std::string digits;
        for (char c : t) {
            if (c >= '0' && c <= '9') digits += c;
            else if (c == '.' && !digits.empty() && digits.find('.') == std::string::npos) digits += c;
            else if (!digits.empty() && (c == '\n' || c == '\r')) break;
        }
        if (digits.size() >= 3) return digits;
    }
    for (fs::path p = langDir; !p.empty(); p = p.parent_path()) {
        std::string name = pxc_util::toUtf8(p.filename().wstring().c_str());
        std::string digits;
        for (size_t i = 0; i < name.size(); ++i) {
            if (name[i] >= '0' && name[i] <= '9') {
                digits += name[i];
                if (i + 1 < name.size() && name[i + 1] == '.') digits += '.';
            } else if (!digits.empty() && digits.back() != '.') {
                if (digits.size() >= 3) return digits;
                digits.clear();
            }
        }
        if (digits.size() >= 3) return digits;
    }
    return {};
}

} // namespace pxc_tdb
