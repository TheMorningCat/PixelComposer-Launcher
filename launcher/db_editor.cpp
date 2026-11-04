// 对照表构建器 —— 数据层实现（见 db_editor.h 的设计说明）。
#include "db_editor.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <set>

#include "io_util.h"
#include "util.h"
#include "native_ui.h"   // pxc_nui::userDataDir()

namespace pxc_dbe {

namespace fs = std::filesystem;
using pxc_store::JsonValue;

namespace {

std::string nowDate() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

// 非字符串字段（tooltip 为 {} / [] / bool）不是文案：官方 en 自带 18 处，PXC 照常读。
bool usable(const JsonValue* v) { return v && v->isStr() && !v->str.empty(); }

// 同一节点里可能出现两个同名端口 / 两个相同下拉值 —— 键相同就是同一个条目，
// 重复两行会让两个编辑框互相打架（后写覆盖前写）。seen 保证首次出现为准。
// 用 set 而不是线性查找：words 有 2000 条、节点字段 15000 条，线性去重是 O(n²)。
void addField(std::vector<KeyRef>& out, std::set<std::string>& seen, Cat cat, const std::string& id,
              const std::string& key, const std::string& en, const char* label) {
    if (!seen.insert(key).second) return;
    KeyRef r;
    r.cat = cat;
    r.id = id;
    r.key = key;
    r.en = en;
    r.label = label;
    out.push_back(std::move(r));
}

// 键 -> 分类（孤儿判定要按分类来：骨架没有某类文件（如 1.22 官方 en 无 UI.json）时，
// 表里那类键无从判断是否"消失"，不能算孤儿，否则整类 UI 译文都会被误标）。
Cat catOfKey(const std::string& key) {
    if (key.rfind("nodes.", 0) == 0) return Cat::Nodes;
    if (key.rfind("words.", 0) == 0) return Cat::Words;
    if (key.rfind("ui.", 0) == 0) return Cat::Ui;
    return Cat::Notes;
}

} // namespace

Cat catOf(const std::string& key) { return catOfKey(key); }

std::string idOf(const std::string& key) {
    if (key.rfind("nodes.", 0) == 0) {
        size_t dot = key.find('.', 6);
        return dot == std::string::npos ? key.substr(6) : key.substr(6, dot - 6);
    }
    size_t dot = key.find('.');
    return dot == std::string::npos ? key : key.substr(dot + 1);
}

std::vector<KeyRef> nodeFields(const JsonValue* node, const std::string& id) {
    std::vector<KeyRef> out;
    if (!node || !node->isObj()) return out;
    std::set<std::string> seen;
    if (usable(node->find("name"))) addField(out, seen, Cat::Nodes, id, pxc_tdb::kNodeName(id), node->find("name")->str, "节点名");
    if (usable(node->find("tooltip"))) addField(out, seen, Cat::Nodes, id, pxc_tdb::kNodeTip(id), node->find("tooltip")->str, "节点提示");
    for (const char* arrName : {"inputs", "outputs"}) {
        const JsonValue* arr = node->find(arrName);
        if (!arr || !arr->isArr()) continue;
        const bool isIn = (arrName[0] == 'i');
        for (const auto& item : arr->arr) {
            if (!item.isObj()) continue;
            const JsonValue* nameV = item.find("name");
            if (!usable(nameV)) continue;
            const std::string& portEn = nameV->str;
            addField(out, seen, Cat::Nodes, id,
                     isIn ? pxc_tdb::kInName(id, portEn) : pxc_tdb::kOutName(id, portEn), portEn,
                     isIn ? "输入端口" : "输出端口");
            if (isIn && usable(item.find("tooltip")))
                addField(out, seen, Cat::Nodes, id, pxc_tdb::kInTip(id, portEn), item.find("tooltip")->str, "输入提示");
            const JsonValue* dd = item.find("display_data");
            if (!dd || !dd->isArr()) continue;
            for (const auto& e : dd->arr) {
                if (!usable(&e)) continue;  // -1.0 / @ref 不是文案
                addField(out, seen, Cat::Nodes, id, pxc_tdb::kDd(id, e.str), e.str, "下拉项");
            }
        }
    }
    return out;
}

std::vector<KeyRef> enumerate(const pxc_base::Baseline& base) {
    std::vector<KeyRef> out;
    std::set<std::string> seen;
    if (base.hasNodes && base.nodes.isObj())
        for (const auto& kv : base.nodes.obj) {
            auto f = nodeFields(&kv.second, kv.first);  // 节点内已去重
            for (auto& r : f)
                if (seen.insert(r.key).second) out.push_back(std::move(r));
        }
    if (base.hasWords && base.words.isObj())
        for (const auto& kv : base.words.obj)
            if (usable(&kv.second))
                addField(out, seen, Cat::Words, kv.first, pxc_tdb::kWord(kv.first), kv.second.str, "界面词条");
    if (base.hasUi && base.ui.isObj())
        for (const auto& kv : base.ui.obj)
            if (usable(&kv.second))
                addField(out, seen, Cat::Ui, kv.first, pxc_tdb::kUi(kv.first), kv.second.str, "界面文本");
    // 注释：值是整篇 markdown。表里 en 只存文件名 —— 把全文塞进每条目会让表膨胀好几倍，
    // 而维护者编辑时右侧会直接从骨架文件读原文（app 侧负责）。
    if (base.hasNotes && !base.dir.empty()) {
        std::error_code ec;
        for (const auto& it : fs::directory_iterator(base.dir / "notes", ec)) {
            if (!it.is_regular_file(ec)) continue;
            std::string name = pxc_util::toUtf8(it.path().filename().wstring().c_str());
            addField(out, seen, Cat::Notes, name, pxc_tdb::kNotes(name), name, "整篇文档");
        }
    }
    return out;
}

Coverage coverage(const pxc_tdb::Db& db, const std::vector<KeyRef>& keys) {
    Coverage c;
    std::set<std::string> keySet;
    std::set<Cat> present;  // 骨架里实际存在的类别（决定哪些键可以判孤儿）
    std::set<std::string> pend(db.pendingReview.begin(), db.pendingReview.end());
    std::set<std::string> ptrans(db.pendingTranslate.begin(), db.pendingTranslate.end());
    for (const auto& k : keys) {
        if (!keySet.insert(k.key).second) continue;  // 重复键只算一次
        present.insert(k.cat);
        ++c.total;
        if (ptrans.count(k.key)) ++c.pendingTrans;
        auto it = db.entries.find(k.key);
        if (it == db.entries.end() || it->second.zh.empty()) {
            ++c.newKeys;   // 骨架有、表里没译文 = "新增"（结构事实）
            continue;
        }
        ++c.translated;
        if (pend.count(k.key)) ++c.pending;
    }
    for (const auto& kv : db.entries) {
        if (kv.second.zh.empty()) continue;
        if (keySet.count(kv.first)) continue;
        if (!present.count(catOfKey(kv.first))) continue;  // 该类别骨架里就没有，无从判断
        ++c.orphan;
    }
    return c;
}

// 从键集合里抽出节点 id（nodes.<id>.xxx，id 本身不带点）。
static std::set<std::string> nodeIdsOf(const std::set<std::string>& keys) {
    std::set<std::string> ids;
    for (const auto& k : keys) {
        if (k.rfind("nodes.", 0) != 0) continue;
        const size_t dot = k.find('.', 6);
        if (dot == std::string::npos) continue;
        ids.insert(k.substr(6, dot - 6));
    }
    return ids;
}

Diff diff(const pxc_tdb::Db& db, const std::vector<KeyRef>& keys,
          const std::set<std::string>* oldKeys) {
    Diff d;
    std::set<std::string> keySet;       // 去重：重复键（同名端口）只算一条
    std::set<std::string> nodeIds;      // 新骨架里的节点 id
    std::set<Cat> present;
    for (const auto& k : keys) {
        if (!keySet.insert(k.key).second) continue;
        present.insert(k.cat);
        if (k.cat == Cat::Nodes) nodeIds.insert(k.id);
        auto it = db.entries.find(k.key);
        if (it == db.entries.end() || it->second.zh.empty()) ++d.newKeys;
    }
    for (const auto& kv : db.entries) {
        if (kv.second.zh.empty()) continue;
        if (!keySet.count(kv.first) && present.count(catOfKey(kv.first))) ++d.orphanKeys;
    }
    // 节点级差异：**骨架 vs 上一版骨架**。拿"骨架 vs 表"来判会误报 ——
    // 表只存有译文的条目，而 FFT / FXAA / SDR 这类节点名本就不该翻，
    // 于是每导入一次同一份包都会把它们算成"新节点"（实测正好 13 个）。
    if (oldKeys) {
        const std::set<std::string> oldIds = nodeIdsOf(*oldKeys);
        for (const auto& id : nodeIds)
            if (!oldIds.count(id)) d.newNodes.push_back(id);
        for (const auto& id : oldIds)
            if (!nodeIds.count(id)) d.goneNodes.push_back(id);
    } else {
        // 没有可比对象：不报新节点（等于把整份骨架都说成新的），只报"表里有译名、
        // 新骨架里已经没有这个节点"的那批 —— 这个靠译文本身就能判断，不需要骨架。
        for (const auto& kv : db.entries) {
            if (kv.second.zh.empty()) continue;
            const std::string& k = kv.first;
            if (!keySet.count(k) && k.rfind("nodes.", 0) == 0 && k.size() > 11 &&
                k.compare(k.size() - 5, 5, ".name") == 0) {
                const std::string id = k.substr(6, k.size() - 11);
                if (!id.empty() && id.find('.') == std::string::npos && !nodeIds.count(id))
                    d.goneNodes.push_back(id);
            }
        }
    }
    std::sort(d.newNodes.begin(), d.newNodes.end());
    std::sort(d.goneNodes.begin(), d.goneNodes.end());
    return d;
}

Diff syncSkeleton(pxc_tdb::Db& db, const pxc_base::Baseline& base,
                  const std::vector<std::string>* newlyKeys,
                  const std::set<std::string>* oldKeys) {
    const std::vector<KeyRef> keys = enumerate(base);
    Diff d = diff(db, keys, oldKeys);
    // 只刷新英文原文：英文措辞会随版本微调，锚点（键）不变，译文自然继续有效。
    for (const auto& k : keys) {
        auto it = db.entries.find(k.key);
        if (it == db.entries.end()) continue;  // 未译的不占位，未译由骨架枚举体现
        if (!k.en.empty() && it->second.en != k.en) {
            it->second.en = k.en;
            ++d.enRefreshed;
        }
    }
    // 新包里有、表里没译文 → **默认标为待翻译**（§4.10）：维护者只需把不该翻的取消掉，
    // 而不是从一万七千条里一条条勾出该翻的。
    // 但只对"这次才出现的键"生效：表里没译文有两种情况 —— ①从没翻过；②维护者判定过"这条不翻"
    // 并取消标记。两者在表里无法区分，若一律补标，重新导入同一份包就会把 ② 全部复活，
    // 队列一边取消一边长回来（用户的实测症状）。
    std::set<std::string> only;
    if (newlyKeys) only.insert(newlyKeys->begin(), newlyKeys->end());
    std::set<std::string> ptrans(db.pendingTranslate.begin(), db.pendingTranslate.end());
    for (const auto& k : keys) {
        auto it = db.entries.find(k.key);
        if (it != db.entries.end() && !it->second.zh.empty()) continue;
        if (newlyKeys && !only.count(k.key)) continue;  // 老键：尊重维护者取消过的标记
        if (ptrans.insert(k.key).second) db.pendingTranslate.push_back(k.key);
    }
    return d;
}

PrefillStats importPack(pxc_tdb::Db& db, const fs::path& baselineDir, const fs::path& packDir,
                        Conflicts& conflicts) {
    PrefillStats p;
    if (baselineDir.empty() || packDir.empty()) return p;

    // 先把包单独对齐成一份"纯包译文"，才能同时回答两个问题：
    //  ① 表里空位该填什么（预填）；② 表里已有译文、包里给了不同说法的是哪些（冲突候选）。
    // 只做增量合并的话拿不到 ② —— build 的增量模式对已译键直接跳过，根本看不到包里的文本。
    pxc_tdb::Db packOnly;
    pxc_tdb::build(packOnly, baselineDir, {packDir}, /*incremental*/false);
    p.fromPack = 0;
    p.fromDict = 0;

    std::set<std::string> pend(db.pendingReview.begin(), db.pendingReview.end());
    std::set<std::string> ptrans(db.pendingTranslate.begin(), db.pendingTranslate.end());
    for (const auto& kv : packOnly.entries) {
        if (kv.second.zh.empty()) continue;
        auto it = db.entries.find(kv.first);
        if (it == db.entries.end() || it->second.zh.empty()) {
            pxc_tdb::Entry e;
            e.en = kv.second.en;
            e.zh = kv.second.zh;
            db.entries[kv.first] = e;
            ++p.filled;
            if (pend.insert(kv.first).second) db.pendingReview.push_back(kv.first);
            // 已经翻好了：从待翻译里摘掉（不要让它继续躺在导出队列里）
            if (ptrans.erase(kv.first)) {
                auto q = std::find(db.pendingTranslate.begin(), db.pendingTranslate.end(), kv.first);
                if (q != db.pendingTranslate.end()) db.pendingTranslate.erase(q);
            }
        } else if (it->second.zh != kv.second.zh) {
            // 同键不同文：**表内中文一个字都不动**，只记下候选。维护者用右侧两个按钮裁决。
            Conflict& c = conflicts[kv.first];
            c.pack = kv.second.zh;
            c.use = false;  // 每次导入都以"表内原文"为准
            ++p.conflicts;
        }
    }
    // build() 只给总数，不区分来源；这里按"表里原来有没有"给个可解释的拆分。
    p.fromPack = p.filled;
    p.fromDict = 0;
    return p;
}

void materialize(pxc_tdb::Db& db, Conflicts& c) {
    for (const auto& kv : c) {
        if (!kv.second.use || kv.second.pack.empty()) continue;
        auto it = db.entries.find(kv.first);
        if (it == db.entries.end()) continue;  // 键已被删掉：候选作废
        if (it->second.zh == kv.second.pack) continue;
        it->second.zh = kv.second.pack;
        // 采纳了新说法 = 这一条有结论了，撤掉待校对标记（否则复核队列里会留一条已经定稿的）
        auto p = std::find(db.pendingReview.begin(), db.pendingReview.end(), kv.first);
        if (p != db.pendingReview.end()) db.pendingReview.erase(p);
    }
    c.clear();  // 候选只活在编辑期：落盘后产物里没有这个概念
}

bool isPendingTranslate(const pxc_tdb::Db& db, const std::string& key) {
    return std::find(db.pendingTranslate.begin(), db.pendingTranslate.end(), key) !=
           db.pendingTranslate.end();
}
bool isPendingReview(const pxc_tdb::Db& db, const std::string& key) {
    return std::find(db.pendingReview.begin(), db.pendingReview.end(), key) != db.pendingReview.end();
}

void setPending(pxc_tdb::Db& db, const std::string& key, bool translate, bool review) {
    if (key.empty()) return;
    auto setOne = [&key](std::vector<std::string>& v, bool on) {
        auto it = std::find(v.begin(), v.end(), key);
        if (on && it == v.end()) v.push_back(key);
        else if (!on && it != v.end()) v.erase(it);
    };
    setOne(db.pendingTranslate, translate);
    setOne(db.pendingReview, review);
}

bool exportTranslateQueue(const fs::path& path, const pxc_tdb::Db& db, const std::vector<KeyRef>& keys,
                          std::string& err) {
    pxc_ovio::Bundle b;
    b.db_version = db.version;
    b.lang_dir = "zh";
    std::set<std::string> ptrans(db.pendingTranslate.begin(), db.pendingTranslate.end());
    for (const auto& k : keys) {
        if (!ptrans.count(k.key)) continue;  // 只导**标记过**的：没标记的连噪音都算不上
        auto it = db.entries.find(k.key);
        // 值填现有译文（可能是待重翻的老译文），没有就填英文原文 —— 翻译方总得看到源文。
        b.overrides[k.key] = (it != db.entries.end() && !it->second.zh.empty()) ? it->second.zh : k.en;
        if (b.overrides[k.key].empty()) b.overrides[k.key] = k.en;
    }
    if (b.overrides.empty()) {
        err = "「待翻译」列表是空的：先在条目右侧打开「待翻译」开关（导入英文包后新键会自动标记）。";
        return false;
    }
    if (!pxc_ovio::save(path, b)) {
        err = "写入失败（目标位置不可写？）。";
        return false;
    }
    return true;
}

ImportStats importTranslations(pxc_tdb::Db& db, const pxc_ovio::Bundle& bundle,
                               const std::vector<KeyRef>& keys, bool markPending) {
    ImportStats st;
    std::map<std::string, std::string> enOf;
    for (const auto& k : keys) enOf[k.key] = k.en;
    std::set<std::string> pend(db.pendingReview.begin(), db.pendingReview.end());
    std::set<std::string> ptrans(db.pendingTranslate.begin(), db.pendingTranslate.end());
    for (const auto& kv : bundle.overrides) {
        if (kv.second.empty()) continue;
        auto e = enOf.find(kv.first);
        if (e == enOf.end()) {  // 键在新骨架里不存在 → 是别的版本残留，不采用
            ++st.ignored;
            continue;
        }
        auto it = db.entries.find(kv.first);
        const bool had = (it != db.entries.end() && !it->second.zh.empty());
        // 导入译文是维护者**主动**的补译动作，允许覆盖草稿/机翻（这是它和"导入汉化包"的区别：
        // 后者是顺手拿别人的包，绝不能冲掉人工成果）。
        if (had && it->second.zh == kv.second) {
            ++st.kept;   // 一模一样，不用动（但状态仍要走下面那步：标记转移）
        } else {
            pxc_tdb::Entry en;
            en.en = e->second;
            en.zh = kv.second;
            db.entries[kv.first] = en;
            if (had) ++st.updated; else ++st.added;
        }
        // 状态转移：待翻译 → 待校对。机翻产物必过人工复核，
        // "一导入就当完成"会让复核队列恰好漏掉最该看的那批。
        if (ptrans.erase(kv.first)) {
            auto q = std::find(db.pendingTranslate.begin(), db.pendingTranslate.end(), kv.first);
            if (q != db.pendingTranslate.end()) db.pendingTranslate.erase(q);
        }
        if (markPending && pend.insert(kv.first).second) db.pendingReview.push_back(kv.first);
    }
    return st;
}

bool saveDb(const fs::path& path, pxc_tdb::Db& db, std::string& err) {
    if (db.entries.empty()) {
        err = "对照表是空的，没有可保存的内容。";
        return false;
    }
    // 待校对里已经找不回译文的键（被删过）顺手清掉，否则列表会越攒越脏。
    std::vector<std::string> kept;
    for (const auto& k : db.pendingReview) {
        auto it = db.entries.find(k);
        if (it != db.entries.end() && !it->second.zh.empty()) kept.push_back(k);
    }
    db.pendingReview.swap(kept);
    // 待翻译**不按"有没有译文"清理**：维护者可以给已经译好的老条目打标重翻（统一术语），
    // 那种条目既有译文又在队列里。这里只去重、去掉空键。
    {
        std::vector<std::string> uniq;
        std::set<std::string> seen;
        for (const auto& k : db.pendingTranslate)
            if (!k.empty() && seen.insert(k).second) uniq.push_back(k);
        db.pendingTranslate.swap(uniq);
    }
    if (db.builtAt.empty()) db.builtAt = nowDate();
    if (!pxc_tdb::save(path, db)) {
        err = "写入失败：" + pxc_util::toUtf8(path.wstring().c_str());
        return false;
    }
    return true;
}

bool loadDbFile(const fs::path& src, pxc_tdb::Db& out, std::string& err) {
    auto [db, ok] = pxc_tdb::load(src);
    if (!ok) {
        err = "无法解析该文件（不是有效的 JSON）。";
        return false;
    }
    if (db.entries.empty()) {
        err = "文件里没有 entries 条目 —— 这不像一份对照表。";
        return false;
    }
    out = std::move(db);
    return true;
}

std::string backupDbFile(const fs::path& dbPath) {
    std::error_code ec;
    if (!fs::exists(dbPath, ec)) return std::string();
    // 备份名用**旧表**的版本号，恢复时一眼能认出是哪一版
    auto [oldDb, oldOk] = pxc_tdb::load(dbPath);
    std::string ver = oldOk && !oldDb.version.empty() ? oldDb.version : std::string("old");
    for (char& c : ver)
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>') c = '_';
    fs::path dir = pxc_nui::userDataDir() / ".db_backup";
    fs::create_directories(pxc_util::toExtended(dir), ec);
    fs::path bak = dir / pxc_util::pathFromUtf8("db_" + ver + "_" + nowStamp() + ".json");
    fs::copy_file(pxc_util::toExtended(dbPath), pxc_util::toExtended(bak), fs::copy_options::overwrite_existing, ec);
    if (ec) return std::string();
    return pxc_util::toUtf8(bak.wstring().c_str());
}

bool replaceDbFile(const fs::path& dbPath, const fs::path& src, std::string& backupPath,
                   std::string& err) {
    pxc_tdb::Db probe;
    if (!loadDbFile(src, probe, err)) return false;  // 半个表比没有表更危险：先严校验再动

    backupPath = backupDbFile(dbPath);
    std::error_code ec;
    fs::copy_file(pxc_util::toExtended(src), pxc_util::toExtended(dbPath), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = "替换失败：" + pxc_util::autoToUtf8(ec.message());
        return false;
    }
    return true;
}

bool exportDbFile(const fs::path& dbPath, const fs::path& dst, std::string& err) {
    std::error_code ec;
    fs::copy_file(pxc_util::toExtended(dbPath), pxc_util::toExtended(dst), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = "导出失败：" + pxc_util::autoToUtf8(ec.message());
        return false;
    }
    return true;
}

bool setTranslation(pxc_tdb::Db& db, const std::string& key, const std::string& en,
                    const std::string& zh, bool reviewed) {
    if (key.empty()) return false;
    auto it = db.entries.find(key);
    const std::string cur = it == db.entries.end() ? std::string() : it->second.zh;
    // 值没变 → **什么都不动**，标记更不能碰。
    // 以前这里写着"值没变但人工看过 → 摘掉待校对标记"，而唯一的调用方 ovSaveAll 是每次
    // 切条目 / 切页 / 切筛选时无条件跑一遍的（用户根本没编辑任何东西），
    // 于是刚打上的「待校对」在切页的瞬间就被自己摘掉了：字段开关不亮、筛选数变 0，
    // 只有左列表那条聚合标记因为没重算还留着 —— 看起来就像"标记自己跑了"。
    // "人工改过译文 = 校过"这件事由 zh != cur 这条分支表达，不需要"打开就算看过"。
    if (zh == cur) return false;
    if (zh.empty()) {
        // 清空 = 删译文。但**保留待翻译标记**：清空通常意味着"这条我放弃/要重写"，
        // 若顺手把标记摘了，它就再也不会出现在导出队列里。
        if (it != db.entries.end()) db.entries.erase(it);
        auto p = std::find(db.pendingReview.begin(), db.pendingReview.end(), key);
        if (p != db.pendingReview.end()) db.pendingReview.erase(p);
        return true;
    }
    pxc_tdb::Entry e;
    e.en = en;
    e.zh = zh;
    db.entries[key] = e;
    // 人亲手写的译文：既算翻完也算校过，两个标记一并摘。
    // （与"导入译文"区别开：那是机翻，只把待翻译转成待校对。）
    if (reviewed) {
        auto drop = [&key](std::vector<std::string>& v) {
            auto p = std::find(v.begin(), v.end(), key);
            if (p != v.end()) v.erase(p);
        };
        drop(db.pendingReview);
        drop(db.pendingTranslate);
    }
    return true;
}

// ---------------- 草稿 ----------------
std::string draftStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

bool saveDraft(const fs::path& path, const pxc_tdb::Db& db, const Conflicts& c) {
    // 草稿 = 整份表（格式与正式表一致），这样万一草稿被当成正式表误用也读得出来。
    pxc_tdb::Db copy = db;
    Conflicts tmp = c;
    materialize(copy, tmp);  // 人来裁决过的候选先在副本里落地，草稿只存结论
    std::error_code ec;
    fs::create_directories(pxc_util::toExtended(path.parent_path()), ec);
    return pxc_tdb::save(path, copy);
}

bool loadDraft(const fs::path& path, Draft& out, std::string& err) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        err = "没有草稿。";
        return false;
    }
    auto [db, ok] = pxc_tdb::load(path);
    if (!ok || db.entries.empty()) {
        err = "草稿文件损坏或为空。";
        return false;
    }
    out.db = std::move(db);
    out.conflicts.clear();  // 候选不落盘（存草稿时已材质化），恢复后没有待裁决项
    out.savedAt = draftStamp();
    out.dirtyKeys = (int)out.db.entries.size();
    return true;
}

bool dropDraft(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return true;
    return fs::remove(path, ec), !ec;
}

} // namespace pxc_dbe
