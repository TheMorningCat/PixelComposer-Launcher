#pragma once
// 对照表构建器的**数据层**（维护者用，无 UI 依赖，可 headless 测）。
//
// 分工：
//   translation_db  = 表的结构与算法（以英文为锚的对齐、存取）；
//   db_editor       = 维护流程的动作（导入英文包 / 预填汉化包 / 补译 / 切表）与统计；
//   app.cpp         = 界面（与自定义译文层浮窗共用同一套「分页 + 左列表 + 右编辑」）。
//
// 一条贯穿全流程的原则：**导入永不覆盖已有译文**。要改译文只能靠人工编辑或「导入译文」这种
// 维护者主动的补译动作，绝不会是某次导入的副作用 —— 否则一个翻得差的新包会把攒了半年的译文冲掉。
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "baseline_scanner.h"
#include "override_io.h"
#include "store.h"
#include "translation_db.h"

namespace pxc_dbe {

// 分页（顺序必须与界面分页一致：节点 / 词条 / 界面 / 注释）
enum class Cat { Nodes = 0, Words = 1, Ui = 2, Notes = 3 };

// 键 -> 分类。孤儿判定要按分类来：骨架没有某类文件（如 1.22 官方 en 无 UI.json）时，
// 表里那类键无从判断是否"消失"，不能算孤儿，否则整类 UI 译文都会被误标。
Cat catOf(const std::string& key);

// 键 -> 条目 id（nodes.<id>.xxx → <id>；words.<k> → <k>）。
std::string idOf(const std::string& key);

// 一个可译条目在骨架里的位置。key 是对照表键；en 是英文原文（notes 页为文件名）。
struct KeyRef {
    Cat cat = Cat::Nodes;
    std::string id;     // 节点 id / 词条 key / UI key / 注释文件名
    std::string key;    // 对照表键
    std::string en;     // 英文原文
    std::string label;  // 分类名：节点名 / 节点提示 / 输入端口 / 输入提示 / 输出端口 / 下拉项
};

// 一个节点里的全部可译字段。构建器右侧与自定义译文层右侧**共用这一份定义**，
// 避免两处各写一遍枚举后悄悄漂移（曾经端口提示就是这样漏掉的）。
std::vector<KeyRef> nodeFields(const pxc_store::JsonValue* node, const std::string& id);

// 整个骨架的可译键：覆盖率的分母、孤儿判定的参照。
// 不含非字符串字段（tooltip 为 {} / [] / bool 时 PXC 照常读，不应当作文案）。
std::vector<KeyRef> enumerate(const pxc_base::Baseline& base);

struct Coverage {
    int total = 0;       // 骨架可译键总数
    int translated = 0;  // 其中有译文的
    int pending = 0;     // 其中还未校对过的（pending_review 里且仍有译文）
    int orphan = 0;      // 表里有译文、但新骨架里没有的键（孤儿：保留，仍参与生成）
    int pendingTrans = 0;// 其中被标了「待翻译」的（决定导出范围）
    int newKeys = 0;     // 其中骨架里有、表里没有译文的（"新增"筛选：结构事实，不可编辑）
};

Coverage coverage(const pxc_tdb::Db& db, const std::vector<KeyRef>& keys);

// 新骨架 × 上一版骨架 / 现有表的差异（§4.2 结构校验）。只描述，不阻断导入。
//
// newNodes / goneNodes 必须是**骨架 vs 骨架**的结构比较，不能拿"表里有没有译名"来判：
// 表只存有译文的条目，而 FFT / FXAA / HLSL 这类节点名本来就保留英文（不该翻），
// 于是每导入一次同一份包都会把它们当成"新节点"（实测 1.22 重新导入报"新节点 13"，
// 正好是 13 个专名节点）。所以这两个字段要求调用方给上一版骨架的键集合。
struct Diff {
    std::vector<std::string> newNodes;   // 新骨架有、**上一版骨架**没有的节点 id
    std::vector<std::string> goneNodes;  // 上一版骨架有、新骨架没有的节点 id
    int newKeys = 0;     // 骨架里有、表里没译文的键
    int orphanKeys = 0;  // 表里有译文、骨架里没有的键
    int enRefreshed = 0; // 英文原文被新骨架刷新的条目（措辞微调，键没变，译文继续有效）
    bool empty() const { return newNodes.empty() && goneNodes.empty() && !newKeys && !orphanKeys; }
};

// `oldKeys` = 上一版骨架的全部键（enumerate 的结果）。不传表示"没有可比对象"：
// 此时不报新节点（否则等于把整份骨架都说成新的），只报"有译名、节点已消失"的那批孤儿。
Diff diff(const pxc_tdb::Db& db, const std::vector<KeyRef>& keys,
          const std::set<std::string>* oldKeys = nullptr);

// ① 导入英文包：把新骨架的英文字段同步进表（**只刷新 en，不动译文**），返回结构差异。
//   未译条目不在表里占位（表只存有译文的），"未译"由骨架枚举得出 —— 这样表不会被空值撑大。
//
//   `newlyKeys`：只给「本次才出现的键」默认打上「待翻译」（§4.10）。
//   必须传它，不能给所有未译键都打标 —— 维护者会手工取消掉"不该翻"的条目标记（数字、@ref、
//   节点 id），而"没译文"和"判定过不翻"在表里长得一模一样；不区分的话每次重新导入同一份包，
//   被取消的标记就会原地复活，队列越导越长。传 nullptr = 全部未译都打（首次建表用）。
Diff syncSkeleton(pxc_tdb::Db& db, const pxc_base::Baseline& base,
                  const std::vector<std::string>* newlyKeys = nullptr,
                  const std::set<std::string>* oldKeys = nullptr);

struct PrefillStats {
    int filled = 0;    // 本次新填上的译文条数
    int fromPack = 0;  // 其中来自包内节点级对齐
    int fromDict = 0;  // 其中来自包内 words 词典兜底
    int conflicts = 0; // 表里已有译文、包里给了不同文本（记候选，绝不覆盖）
};

// 冲突候选（§4.11）：导入中文包时「表里有中文、包里也有中文、两者不同」的条目。
// **只存包里那段文本 + 一个开关，绝不动 zh** —— 原文就是 zh 本身，所以不需要另存 base，
// 两个按钮（替换为新包文本 / 恢复对照表原文）只是切 use，可以来回点。
// 代价：它不是分发语义，所以不落盘，下次导入中文包时整体重建。
struct Conflict {
    std::string pack;   // 新包给的文本（候选）
    bool use = false;   // false = 用表内中文（默认）；true = 用新包文本
};
using Conflicts = std::map<std::string, Conflict>;

// 落盘前材质化：use==true 的键把 pack 写进 zh，然后清空候选。
// 这样产物里没有"候选"这个概念 —— 运行期生成器不需要知道这回事。
void materialize(pxc_tdb::Db& db, Conflicts& c);

// 显式打标（右栏两个开关）：任何条目都能打，包括已经译好的老条目
// （维护者靠这个把一批塞进"待翻译"重翻、统一术语）。
void setPending(pxc_tdb::Db& db, const std::string& key, bool translate, bool review);
bool isPendingTranslate(const pxc_tdb::Db& db, const std::string& key);
bool isPendingReview(const pxc_tdb::Db& db, const std::string& key);

// ③ 预填：用含汉化的包给**空位**补译文（已译的一律不动，但同键不同文记进 conflicts），
//   补上的记入待校对。包里对不上新骨架的键一律不采用 —— 对齐以英文骨架为骨，这是唯一可信的参照。
PrefillStats importPack(pxc_tdb::Db& db, const std::filesystem::path& baselineDir,
                        const std::filesystem::path& packDir, Conflicts& conflicts);

struct ImportStats {
    int added = 0;    // 原本没译文，填入
    int updated = 0;  // 原本有译文、这次被导入的文本替换（仅"导入译文"会这样）
    int kept = 0;     // 已有译文且完全一致，没动
    int ignored = 0;  // 键在新骨架里不存在（别的版本残留），不采用
};

// ④ 补译：导出「待翻译」队列（只含标记过的键），再导回。
//   格式与「自定义译文」同构（override_io::Bundle），所以两边能互相读懂。
bool exportTranslateQueue(const std::filesystem::path& path, const pxc_tdb::Db& db,
                          const std::vector<KeyRef>& keys, std::string& err);

// 导入译文：**按 key 覆盖**（这是维护者主动的补译动作，允许冲掉草稿），
// 并把命中的键从「待翻译」移到「待校对」—— 机翻产物必过人工复核，不能一导入就当完成。
ImportStats importTranslations(pxc_tdb::Db& db, const pxc_ovio::Bundle& bundle,
                               const std::vector<KeyRef>& keys, bool markPending = true);

// ⑤ 发布：改版本号后写回。表就是运行期用的那一份，没有第二份副本。
bool saveDb(const std::filesystem::path& path, pxc_tdb::Db& db, std::string& err);

// 把当前表复制一份到 <表目录>/.db_backup/db_<版本>_<时间>.json，返回备份路径（空 = 没有旧表或复制失败）。
// 「生成并覆盖」也走这里 —— 覆盖运行期表之前必须留一份能认出版本的旧表，
// 否则"写错了"只能靠"重新生成"补救，而生成出来的语言文件已经被用户拿去用了。
std::string backupDbFile(const std::filesystem::path& dbPath);

// §4.9 切表：从文件导入（先严校验，再备份旧表到 .db_backup/），或把当前表另存到别处。
bool loadDbFile(const std::filesystem::path& src, pxc_tdb::Db& out, std::string& err);
bool replaceDbFile(const std::filesystem::path& dbPath, const std::filesystem::path& src,
                   std::string& backupPath, std::string& err);
bool exportDbFile(const std::filesystem::path& dbPath, const std::filesystem::path& dst,
                  std::string& err);

// 单词条编辑（构建器右侧输入框落盘用）：空 = 删译文；等于旧值 = 不动。
// 人工编辑视为"既翻完也校过" → 两个标记一并摘（清空不算完成，待翻译标记保留）。
// 返回 true 表示表被改动。
bool setTranslation(pxc_tdb::Db& db, const std::string& key, const std::string& en,
                    const std::string& zh, bool reviewed);

// ---------------- 草稿（防止关软件丢掉半天编辑） ----------------
// 每次改动后写一份过程文件，下次打开构建器时问"要不要接着上次的改"。
// 位置固定在 data/.draft/db_draft.json，不进 git、不参与分发。
struct Draft {
    pxc_tdb::Db db;
    Conflicts conflicts;
    std::string savedAt;    // "YYYY-MM-DD HH:MM"
    int dirtyKeys = 0;      // 相对于磁盘表的改动条数（提示用）
};
bool saveDraft(const std::filesystem::path& path, const pxc_tdb::Db& db, const Conflicts& c);
bool loadDraft(const std::filesystem::path& path, Draft& out, std::string& err);
bool dropDraft(const std::filesystem::path& path);
std::string draftStamp();  // "YYYY-MM-DD HH:MM"，草稿与备份共用

} // namespace pxc_dbe
