#pragma once
// 汉化对照表（Translation DB）—— v2。
//
// 角色：这是**构建期产物**，随程序分发、运行期只读。最终用户不编辑它。
//
// 键设计：**以英文为锚**（跨版本稳定）
//   words.<key>                     界面词条（key 本身是英文标识）
//   ui.<key>                        UI 文本
//   nodes.<id>.name / .tooltip      节点名 / 节点提示
//   nodes.<id>.in.<英文端口名>.name  输入端口（端口没有 id，用英文名当锚）
//   nodes.<id>.in.<英文端口名>.tip
//   nodes.<id>.out.<英文端口名>.name 输出端口
//   nodes.<id>.dd.<英文枚举值>       下拉项（按值不按下标：数组长度随版本变）
//
// 对齐：以目标版本的**英文骨架**为骨，逐包对齐。
//   节点按 id；端口/下拉在「两边数组长度一致」时才按位对应（长度不一致 = 结构变了，不可信，跳过）。
//   长度一致时记录的是 英文锚 -> 中文，所以换个版本骨架也能复用。
//   节点级没对齐到的，用包内 words.json 当英->中词典兜底。
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace pxc_tdb {

// ---- 键构造 ----
std::string kWord(const std::string& key);
std::string kUi(const std::string& key);
std::string kNodeName(const std::string& id);
std::string kNodeTip(const std::string& id);
std::string kInName(const std::string& id, const std::string& portEn);
std::string kInTip(const std::string& id, const std::string& portEn);
std::string kOutName(const std::string& id, const std::string& portEn);
std::string kDd(const std::string& id, const std::string& enumEn);
// notes/<英文文件名>（整篇文档覆盖：值就是 markdown 全文）
std::string kNotes(const std::string& fileName);

struct Entry {
    std::string en;   // 英文（骨架里的值；UI 补充键可能为空）
    std::string zh;   // 译文（空 = 无译文，运行期回退英文）
    std::string src;  // 来源包 id（空 = 来自词典兜底或未译）
};

struct Db {
    // 对照表版本（维护者手动维护，形如 "1.22.0.5-机翻版"）。这是分发资源的唯一版本标识：
    // 不再记录"由哪些汉化包合并而来"——那些包是构建期的中间输入，对使用者没有意义。
    std::string version;
    std::string baseline;                  // 构建时使用的英文骨架路径（仅构建期参考，不参与分发语义）
    std::string builtAt;                    // 构建时间
    std::vector<std::string> keepEnglish;   // 不译词表（命中则保留英文）
    std::map<std::string, Entry> entries;

    // 待校对：由「导入含汉化的包」预填、还没人工确认过的键。存**键列表**而不是给每条目加字段 ——
    // 几千条里可能只有几百条待校对，逐条加 reviewed/src 会让表重新膨胀（正是删掉 src 的原因）。
    std::vector<std::string> pendingReview;

    // 待翻译：**要不要翻**（与 pendingReview 的"翻得对不对"是两个问题）。
    // 为什么需要它而不是"导出全部未译"：PXC 语言文件里大量条目本就不该翻（单字母轴、Phong/SDF
    // 这类专名、纯 %s 占位、示例文本），全导出去等于把几千条噪音丢给翻译方，真正该翻的几十条
    // 反而形不成队列。标记写进表才能跨会话续翻（"上次还剩 43 条"）。
    std::vector<std::string> pendingTranslate;

    // 新增：**本次导入英文包后才出现**的键（旧骨架里没有）。与"未译"不同 ——
    // 未译包含历史上一直没翻的老键，新增只标这次升级带来的那批，维护者先处理这一批最划算。
    // 覆盖当前对照表后清空（那时它们已经进了表，不再"新增"）。
    std::vector<std::string> addedKeys;
};

struct BuildStats {
    int baselineTotal = 0;// 骨架里「英文非空」的可译条目总数（覆盖率分母）
    int entries = 0;     // 存下的条目数（只存有译文的）
    int translated = 0;  // 有译文的条目数
    int fromAlign = 0;   // 其中来自节点级对齐
    int fromDict = 0;    // 其中来自词典兜底
    int nodesTotal = 0;  // 骨架节点数
    int nodesAligned = 0;// 与至少一个包完成结构对齐的节点数
    int wordsTotal = 0;
    int newKeys = 0;     // 本次新增条目（增量构建时才有意义）
};

// 构建：以 baselineDir 的英文骨架为骨，按 packs 顺序（优先级从高到低）填译文。
// incremental=true：保留 db 里已有译文，只补新键（PXC 升级后的常规动作）。
BuildStats build(Db& db, const std::filesystem::path& baselineDir,
                 const std::vector<std::filesystem::path>& packs, bool incremental = false);

bool save(const std::filesystem::path& path, const Db& db);
std::pair<Db, bool> load(const std::filesystem::path& path);

// 查表；没有返回 nullptr。
const Entry* find(const Db& db, const std::string& key);

// 是否属于「保留英文」的词（专业名词 / 单字母轴 / 扩展名）。
bool keepEnglish(const Db& db, const std::string& text);

// 默认不译词表。
const std::vector<std::string>& defaultKeepEnglish();

// 语言目录定位：dir 自身就是语言目录，或含 Locale/<lang>，或含 <lang> 子目录。
std::filesystem::path locateLangDir(const std::filesystem::path& dir, const std::string& prefer = "zh");

// 版本标签猜测（vesion/version 文件 → 目录名 → 父目录名）。
std::string guessVersion(const std::filesystem::path& langDir);

// 汉化包短标签（如 "1.22.0.5/zh"）。对照表随程序分发，不写绝对路径。
std::string packLabel(const std::filesystem::path& pack);

} // namespace pxc_tdb
