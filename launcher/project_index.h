#pragma once
// 工程索引（数据层，不依赖 UI）。
//
// 六个页签的数据有三种来源，都不归启动器所有：
//   近期文件 / 自动恢复 —— PXC 自己写（recent.json / Autosave/），这里只读；
//   全部 / 工程库 / 启动历史 / 收藏 —— 启动器自己写（data/projects.json）。
//   「全部」= 工程库 ∪ 启动历史（v4）。
// 这一层负责"读别人的数据而不改它"、"扫 4000 个文件也只取 name/size/mtime"、
// 以及把识别结果按 path+size+mtime 缓存下来，好让 UI 先出列表、版本列再慢慢填。
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "pxc_detect.h"

namespace pxc_index {

// 页签顺序：近期 / 全部 / 工程库 / 启动历史 / 收藏 / 自动恢复（v4 六页签）
// 「工程库」在存储里仍然是 imported —— 改名要写迁移，且旧版本启动器读新文件会当成空库，
// 为了一个显示名不值（docs/04 §8.1.3）。
enum class Tab { Recent = 0, All = 1, Library = 2, History = 3, Favorite = 4, Autosave = 5 };
constexpr int kTabCount = 6;

// 兼容旧名：v3 的代码/测试里写的 Tab::Imported 一律指工程库。
constexpr Tab kTabLibrary = Tab::Library;

const char* tabName(int tab);

// 置信度：0 高 / 1 中 / 2 低 / 3 未知（用 int 存是为了直接落盘 JSON，也让 UI 少一层转换）。
int confOf(pxc::Confidence c);
const char* confLabel(int conf);

struct Item {
    std::string path;      // 绝对路径（UTF-8）
    std::string name;      // 显示名：文件去掉 .pxc，文件夹用目录名
    bool folder = false;

    std::uint64_t size = 0;
    std::int64_t mtime = 0;    // unix 秒
    std::string addedAt;       // 加入工程库 / 收藏时间（ISO；近期与自动恢复为空）

    // 副行直接印它（v4：只到所在文件夹，不带文件名）。
    // 文件取 parent_path，文件夹取自己的上级 —— 两种行语义一致，扫一眼就知道"放在哪一堆里"。
    std::string parentDir;

    int childCount = -1;       // 文件夹里有多少条（-1 = 还没扫过）

    // 识别结果（懒填；detected=false 时版本列显示占位）
    bool detected = false;
    std::string version;
    std::string format;        // "JSON" / "zlib" / "PXCX" / "未知"
    int conf = 3;

    bool isFile() const { return !folder; }
};

// ---------------- 用户目录 ----------------
// 复用 PXC 自己的重定向记录（persistPreference.json），且**不要求** Locale 目录存在 ——
// 只读 recent.json / Autosave 时，Locale 有没有跟我们无关。
std::vector<std::filesystem::path> userDirCandidates();
std::filesystem::path resolveUserDir();   // 首个"看起来有 PXC 数据"的候选

// ---------------- 扫描 ----------------
// 近期：recent.json 的 Recents 数组，保持原顺序（PXC 自己就是最新在前）。
// 文件已不存在的条目按 §7.8 直接丢弃（并可由调用方记进 recent_hidden）。
std::vector<Item> readRecent(const std::filesystem::path& userDir,
                             std::vector<std::string>* missing = nullptr);

// 扫**一层**：子文件夹 + .pxc 文件（「双击进入文件夹」用）。
// 不递归 —— Autosave 那样的目录递归一次能炸出 3000 条，"进入"就失去了"我在看某个位置"的意义。
std::vector<Item> scanLayer(const std::filesystem::path& dir);

// 扫一个目录下的 .pxc。递归模式用于"导入的文件夹"展开、以及自动恢复（Autosave 是平的）。
// 只用 FindFirstFileW + WIN32_FIND_DATA（拿 name/size/mtime），不解压任何文件。
std::vector<Item> scanPxcDir(const std::filesystem::path& dir, bool recursive);

// 单个路径 -> Item（文件或文件夹），失败返回 false。
bool statPath(const std::filesystem::path& p, Item& out);

void applyDetect(Item& it, const pxc::DetectionResult& r);

// ---------------- 排序 / 搜索 / 漏斗 ----------------
enum class SortKey { None = 0, Name = 1, Mtime = 2, Version = 3 };
struct SortSpec {
    SortKey key = SortKey::None;
    bool asc = true;
};
// 默认排序列：近期 = 不排序（数组序）、自动恢复 = mtime ↓、其余 = 最近使用 ↓。
SortSpec defaultSort(int tab);
// 排「下标数组」而不是 Item 本体 —— UI 侧的视图是指向 all 的下标，排序要在下标上做，
// 否则每排一次就要拷一遍 4000 个 Item（还把懒识别的进度拷丢）。
void sortIndices(std::vector<int>& idx, const std::vector<Item>& items, SortSpec spec);
void sortItems(std::vector<Item>& items, SortSpec spec);
// 表头点击循环：文件名 ↑/↓ → 修改时间 ↓/↑ → 版本 ↑/↓ → 回来（按当前 spec 推下一档）。
SortSpec nextSort(SortSpec cur);
const char* sortLabel(SortSpec spec);

std::string foldQuery(const std::string& q);
bool matchQuery(const Item& it, const std::string& foldedQuery);

// ---- 版本状态（卡片圆点 / 筛选共用同一口径）----
// v4 把「识别不出」和「识别中」分开了：前者是结论（这个文件读不出结构），后者是过程（还没轮到它）。
// 混成一档会让用户以为"再等等就好了"，而实际上它永远好不了。
enum class VerState {
    Pending = 0,     // 识别中（还没轮到）—— 灰
    Ok = 1,          // 本机已登记这个版本，双击能直接开 —— 绿
    Missing = 2,     // 版本已知、本机没有（双击走跨版本协商）—— 橙
    Unreadable = 3,  // 识别不出（文件头不属于任何已知结构族）—— 红
};
const char* verStateName(int st);   // 提示/筛选用的中文名

// 筛选浮窗「状态」四档。**显示顺序**（可启动 → 缺失 → 识别不出 → 识别中）和
// VerState 的枚举顺序不一致，上一版就是因为两边各写一套下标，点「识别不出」筛出 0 条
// （下标 2 实际取到了 Missing 的位）。所以顺序只有这里一份，UI 按 chip 下标取位，测试也按这里验。
constexpr int kStateChipCount = 4;
VerState stateChipAt(int idx);        // idx 越界按 Pending 处理
int stateChipBit(int idx);            // = 1 << (int)stateChipAt(idx)
const char* stateChipName(int idx);

// registered：版本库里已登记的版本串（四段规范化后比较）。
VerState stateOf(const Item& it, const std::vector<std::string>& registered);

// ---- 筛选（v4：四维正交，分区之间 AND，分区内部 OR）----
// 版本多选里的空串代表「未知版本」——文件版本串为空（识别不出）的那些。
struct Filter {
    std::vector<std::string> versions;   // 空 = 不限
    // 位 = 1 << (int)VerState，即 1=识别中(Pending) 2=可启动(Ok) 4=版本缺失(Missing) 8=识别不出(Unreadable)。
    // 注意别按“显示顺序”左移：显示序 0..3 是 可启动/缺失/识别不出/识别中，和枚举序不同，
    // 取位一律走 stateChipBit(idx)（踩过：手写 1<<2 点“识别不出”筛出 0 条）。
    int states = 0;                      // 0 = 不限
    int timeRange = 0;                   // 0 不限 / 1 今天 / 2 近7天 / 3 近30天 / 4 今年 / 5 自定义
    std::int64_t from = 0, to = 0;       // 自定义（unix 秒，闭区间）
    bool onlyFav = false;

    bool active() const;
    int activeGroups() const;            // 「筛选 · N」的 N = 生效的分区数
    void reset();
};

bool passFilter(const Item& it, const Filter& f, const std::vector<std::string>& registered,
                bool isFav);

// 时间段档位名（筛选浮窗里那一排）
const char* timeRangeName(int r);
// 「今天 / 近 7 天 / …」换算成 [from, to] 闭区间；r == 0 时两者都不写。
void timeRangeBounds(int r, std::int64_t now, std::int64_t& from, std::int64_t& to);

// v3 的四档漏斗保留成一层薄封装（旧测试还在用）：1 可启动 / 2 版本缺失 / 3 识别不出。
const char* funnelName(int f);
bool passFunnel(const Item& it, int funnel, const std::vector<std::string>& registered);

// ---------------- projects.json ----------------
struct HistoryEntry {
    std::string path, version, exeVersion, format, conf, launchedAt;
    int count = 1;
};
struct ImportEntry {
    std::string path, type;   // type = "file" / "folder"
    std::string addedAt;
};
struct FavoriteEntry {
    std::string path, addedAt;
};
struct RunningEntry {
    std::string path, exe, startedAt;
    int pid = 0;
};
struct AssocState {
    bool registered = false;
    bool silentLaunch = true;
    bool reuseInstance = true;
    std::string previousProgid;
    std::string registeredAt;
};

struct Store {
    std::vector<HistoryEntry> history;
    std::vector<ImportEntry> imported;
    std::vector<FavoriteEntry> favorites;
    std::vector<std::string> recentHidden;
    std::vector<RunningEntry> running;
    AssocState assoc;
};

// 上限：history 500 / imported 2000，超出按最久未用淘汰；favorites 不限。
constexpr std::size_t kMaxHistory = 500;
constexpr std::size_t kMaxImported = 2000;

// 文件不存在或损坏：返回空 Store（ok=false），损坏的那份另存 .bad 后再由调用方重建。
Store loadStore(const std::filesystem::path& file, bool* ok = nullptr);
bool saveStore(const std::filesystem::path& file, const Store& s);   // 原子写（.tmp + rename）

void noteLaunch(Store& s, const std::string& path, const std::string& fileVersion,
                const std::string& exeVersion, const std::string& format, const std::string& conf);
void addImported(Store& s, const std::string& path, bool folder);
void removeImported(Store& s, const std::string& path);
bool isFavorite(const Store& s, const std::string& path);
bool toggleFavorite(Store& s, const std::string& path);   // 返回切换后是否收藏
void removeFavorite(Store& s, const std::string& path);
void hideRecent(Store& s, const std::string& path);
void pruneStore(Store& s);

std::int64_t nowUnix();
std::string nowIso();
std::string isoOf(std::int64_t unixSec);
std::int64_t parseIso(const std::string& s);

// ---------------- 识别缓存 ----------------
// key = path，值带 size+mtime；两者任一变化即视为未命中（文件被 PXC 改写后必须重识别）。
struct CacheEntry {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::string version, format;
    int conf = 3;
};

class DetectCache {
public:
    void load(const std::filesystem::path& file);
    bool get(const std::string& path, std::uint64_t size, std::int64_t mtime, CacheEntry& out) const;
    void set(const std::string& path, const CacheEntry& e);
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    bool save(const std::filesystem::path& file);
    std::size_t size() const { return map_.size(); }

private:
    std::map<std::string, CacheEntry> map_;
    std::vector<std::string> lru_;   // 每次 put 追加到尾部；超限从头部淘汰
    bool dirty_ = false;
};

} // namespace pxc_index
