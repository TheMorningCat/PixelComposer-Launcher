// PXC 启动器 —— EUI-NEO 三页窗口程序。
// 页 1 版本库：添加 exe（浏览 -> PE 自动读版本；读不到则手动填）并持久化。
// 页 2 工程文件：选择文件夹 -> 递归识别 .pxc -> 逐行「启动」（必要时先备份）。
// 页 3 汉化：基于只读对照表 + 目标版本的英文骨架，生成完整语言文件写入「用户目录/Locale/<语言目录名>」。
// 角色分清：用户只做「生成」；维护对照表是维护者的事，藏在汉化页角落的「构建器」里
// （与自定义译文层共用同一套浮窗，只换数据源：overrides ↔ translation_db.json）。
//
// 文件系统全程 std::filesystem::path（宽路径）；UTF-8 仅用于 UI 文本与 JSON 存储。
// EUI-NEO 只处理指针事件，不接收 OS 级拖放；故由 launcher/native_ui 取主窗口 HWND，
// 注册 DragAcceptFiles + 子类化窗口过程接收 WM_DROPFILES（版本库页收 exe、工程文件页收文件夹/单个 .pxc、
// 汉化页收目录）。文件/文件夹选择统一走 IFileDialog（可输入地址 + 左侧快速访问）。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <ctime>
#include <utility>

// EUI-NEO 头文件必须先于 Windows 头文件包含（避免 rpcndr.h 的 #define small 污染 EUI 主题令牌）。
#include "eui_neo.h"
#include "eui/signal.h"
#include "components/button.h"
#include "components/colorpicker.h"
#include "components/virtuallist.h"
#include "components/contextmenu.h"
#include "components/dropdown.h"
#include "components/dialog.h"
#include "components/input.h"
#include "components/mousearea.h"
#include "components/navbar.h"
#include "components/scrollview.h"
#include "components/segmented.h"
#include "components/slider.h"
#include "components/theme.h"

#include "pxc_detect.h"
#include "store.h"
#include "pxc_prefs.h"
#include "version_registry.h"
#include "version_probe.h"
#include "version_advisor.h"
#include "project_launcher.h"
#include "project_index.h"
#include "project_ui.h"
#include "assoc.h"
#include "single_instance.h"
#include "pxc_reuse.h"
#include "launch_runner.h"
#include "launch_stage.h"
#include "launch_toast.h"
#include "backup_manager.h"
#include "launcher.h"
#include "locale_manager.h"
#include "translation_db.h"
#include "override_io.h"
#include "db_editor.h"
#include "search_fold.h"
#include "baseline_scanner.h"
#include "gen_autolaunch.h"
#include "locale_generator.h"
#include "expr.h"
#include "io_util.h"
#include "log.h"
#include "util.h"
#include "native_ui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <commdlg.h>
#undef small

namespace app {

enum class Page { Registry, Projects, Locale, Settings };
static Page g_page = Page::Projects;

static pxc_store::AppConfig g_cfg;
static std::filesystem::path g_exeDir;

// 版本库页（v5，docs/07）：渠道分组卡片 + 刷新 + 三类弹窗 + 卡片右键。
static std::filesystem::path g_pendingExe;   // 待填版本的 exe（「无法识别当前 PXC 版本」弹窗）
static eui::Signal<std::string> g_manualVersion{""};
static bool g_verDlgOpen = false;
static std::string g_regSel;                        // 选中的卡片（exe_path）；空 = 没选
static std::set<std::string> g_regMissing;          // 文件已不在的条目（刷新时填）
static std::vector<pxc_reg::VersionChange> g_regChanges;   // 上次刷新发现的 Steam 版本变化
static std::string g_prefAsk;                       // 待询问启动偏好的版本号（跨渠道同版本、未表态）
static bool g_prefDlgOpen = false;
static std::string g_prefLaunchExe;                 // 被偏好询问拦下的那次启动（双击卡片）；空 = 无
static bool g_dbgNoPrefDlg = false;                 // 调试：进页不弹偏好询问（验"启动时问"那条）
static double g_regClickMs = 0.0;                   // 上一次单击的时刻（双击启动靠它判）
static std::string g_regHover;                      // 悬停的卡片 exe（画完整路径用）
static float g_regHoverX = 0.0f, g_regHoverY = 0.0f;
static float g_regHoverScroll = -1.0f;              // 悬停当时的滚动量：滚动一变就说明鼠标下已换了卡片

// 卡片右键菜单：自绘（components::contextMenu 只有一维字符串项，画不出"设为默认启动"的子项与勾）。
struct RegMenu {
    bool open = false;
    std::string exe;
    std::string version;
    float x = 0.0f, y = 0.0f;
    bool sub = false;          // 是否展开「设为默认启动 ▸」
};
static RegMenu g_regMenu;

// 同渠道同版本撞车（§11.6.1 情况 A）：弹窗二选一。
struct RegDup {
    bool open = false;
    std::filesystem::path path;
    std::string version, channel, existing;
};
static RegDup g_dup;

// Steam 版本变化说明（§11.7）。
struct RegChangeDlg {
    bool open = false;
    std::string exe, from, to;
};
static RegChangeDlg g_changeDlg;

// 卡片素材（SVG 标记文本，一次读盘；绝不每帧读文件）。
static std::string g_iconPxc, g_iconItch, g_iconSteam;
static bool g_regIconsTried = false;

// 工程文件页
static std::vector<std::string> g_versionList;       // 去重后的已登记版本
static std::string g_lastLaunch;

// 缺版本协商浮窗：文件识别出的版本在本机没有登记时，让用户从已登记版本里挑一个启动。
// 状态放在这里而不是函数内静态量，是因为弹窗每帧重建，且要能被列表、拖入、以后的双击共用。
struct VersionPick {
    bool open = false;
    std::filesystem::path path;
    pxc_adv::Advice advice;
    int sel = 0;
};
static VersionPick g_pick;

// ---------- 工程文件工作台（v4）状态 ----------
// 数据来源都不归启动器所有（PXC 的 recent.json / Autosave、用户手拖进来的文件），
// 所以这一层的两条纪律：① 别人的文件只读；② 列表先出来，版本列靠懒识别慢慢填。
struct WbState {
    int tab = 0;
    std::vector<pxc_index::Item> all;   // **当前这一层**的条目（识别结果写这里）
    std::vector<int> rows;              // 过滤 + 排序后要画的行 = all 的下标

    // 层缓存：目录路径 -> 该层条目（"" = 根层）。
    // 切层不重建 all 而是换一份缓存：识别结果与"共 N 个"都留着，回来时不用重新 stat 一遍
    // （自动恢复那种 3000 条的层会明显卡一下）。
    std::map<std::string, std::vector<pxc_index::Item>> layers;
    std::string dir;                    // 当前层路径（"" = 根层）
    std::vector<std::pair<std::string, std::string>> crumbs;   // (显示名, 路径)，不含根段

    bool loaded[pxc_index::kTabCount] = {false, false, false, false, false, false};
    pxc_index::Filter filter;
    bool filterOpen = false;
    std::string status;
    std::string emptyText;
    std::string iconSvg;                // .pxc 图标（素材 SVG 的文本，一次读盘）
    bool iconTried = false;
    float listTop = 158.0f;             // 本帧列表顶边（进入态会下移，右键菜单要用它算位置）
};
static WbState g_wb;
static pxc_index::SortSpec g_wbSort[pxc_index::kTabCount];
static eui::Signal<std::string> g_wbQuery{""};
static eui::Signal<float> g_wbScroll{0.0f};
static int g_wbListGen = 0;                 // 虚拟列表滚动复位靠换 id（见 §4.7 的教训）
static std::string g_wbAppliedQuery;         // 上次建视图用的查询串
static pxc_index::Store g_wbStore;
static pxc_index::DetectCache g_wbCache;
static std::filesystem::path g_wbUserDir;
static bool g_wbBooted = false;
static bool g_wbStoreDirty = false;
static double g_wbSaveAt = 0.0;
static double g_wbCacheSaveAt = 0.0;
static float g_scrH = 720.0f;                // 本帧屏幕高（composeProjects 要用）
static float g_scrW = 1280.0f;               // 本帧屏幕宽（右键菜单/提示框夹边界要用）
static pxc_assoc::Info g_wbAssoc;
static double g_wbAssocAt = 0.0;
// 复用已有进程期间的挂起请求（复用失败 = 静默退新进程）
// worker 干完活后 PostMessage 给主窗的消息号。
// 为什么不用 compose 里的轮询：主窗最小化后 compose 一次都不跑（启动恰恰发生在最小化时）。
static const unsigned kMsgLaunchDone = WM_APP + 0x61;
// 拉起后是否常驻（不退出、最小化留在任务栏）。
// 注意：目前是常量 true，还没接 assoc.stay_resident 配置项（docs/05 §9.9 的开关尚未落地）。
static bool g_stayResident = true;
static bool g_runnerReady = false;
// 注意：这里**不再**存"待启动工程的路径"。连开两个工程时那是个单格变量，
// 第二个会盖掉第一个，结果回来时记进历史的就是错的文件；路径现在跟 Result 一起回来。
// 右键菜单
struct WbMenu {
    bool open = false;
    float x = 0.0f, y = 0.0f;
    int row = -1;
    std::vector<std::string> items;
};
static WbMenu g_wbMenu;
// 拖入/第二实例转发过来的待导入路径（窗口过程里只记，处理放到 compose）
static std::vector<std::filesystem::path> g_wbInbox;
static int g_wbSel = -1;                     // 选中行（右键/刷新的锚点）
// 双击 .pxc 的静默启动：先藏窗，等"工程交出去"有结果再退（见 compose 末尾）
static bool g_silentLaunch = false;
static double g_silentStartMs = 0.0;
// 「把启动器窗口顶到前台」还剩几次重试（只在协商窗那次提窗口失败时非 0）。
// 为什么要留给后面的帧：缺版本协商可能发生在窗口刚建出来、还没显示的那一瞬
// （静默启动的首帧），那一次 SetForegroundWindow 拿不到可见窗口，只能下一帧再来。
static int g_fgRetry = 0;
// 工作台动作（定义在下方「工程文件工作台」一节）：拖放与命令行要提前引用
static void wbImportPaths(const std::vector<std::filesystem::path>& paths, bool switchTab = true);
// 文本宽度/裁剪：版本库卡片在文件靠前的位置，两个工具定义在工作台一节，这里先声明。
static float wbRealTextW(const std::string& s, float fs, int weight);
static std::string wbFit(const std::string& s, float maxW, float fs);
static std::string wbFitHead(const std::string& s, float maxW, float fs);
// 「启动前自动生成」（docs/08 §8.2.5）：三条"新起 PXC 进程"的路径都要过这一关，实现见汉化页那节。
// 前置声明放这么前，是因为版本库双击（launchExeNow）与缺版本协商（pickLaunch）都比它早。
static void runAutoGenFor(const std::filesystem::path& exe, const std::string& version);

// 左侧导航宽度（画布 x 偏移）。版本库卡片的右键菜单/提示框要按客户区坐标定位，会用到它。
static constexpr float kNavW = 200.0f;


// 汉化页（运行期）：对照表只读 + 生成语言文件
static pxc_tdb::Db g_db;
static bool g_dbLoaded = false;
// 当前页内容的实际高度（上一帧测量值）：页面用绝对定位排版，滚动容器的画布高度
// 必须跟着内容走，否则内容会超出屏幕被裁掉、且滚不动。
static float g_pageH = 1200.0f;
static pxc_gen::Stats g_lastGen;   // 上次生成统计（调试/日志用；结果本体走 toast）
static bool g_hasGen = false;
static bool g_baseReady = false;   // 英文骨架缓存是否有效（用户目录 / 目标版本变更时失效）
static pxc_base::Baseline g_base;  // 骨架缓存：生成与自定义译文层编辑器共用同一份
static bool ensureBase();          // 定义在「自定义译文层」一节（先声明给 onGenerate 用）
static void invalidateBase();      // 骨架缓存失效（换目标版本 / 换用户目录 / 登记表变动）

// ---- toast（docs/08 §8.6.3）----
// 本页不再有常驻状态行：结果一律走 EUI 自带的右下角浮窗。只保留一条，
// 后来的覆盖前面的（不排队 —— 排队会让"刚点完又点一次"看到上一条的旧结论）。
struct LocToast {
    bool on = false;
    std::string title, msg;
    bool error = false;   // 失败：红色图标 + 不自动消失（必须手动关）
};
static LocToast g_toast;
static void showToast(const std::string& title, const std::string& msg, bool error = false);

// ---- 目标版本（docs/08 §8.2.4）----
// 只列**用户已装且能取到骨架**的版本。列在页面上的数字（节点数/覆盖率）都要读一次骨架，
// 所以整份清单按需刷新、常驻缓存，绝不进帧循环。
static std::vector<pxc_base::InstalledVersion> g_skelOpts;
static bool g_skelOptsDirty = true;
static std::filesystem::path skeletonCacheRoot();

// ---- 页面级悬停提示（自绘，根级）----
// 两个地方共用一套：`!` 解释"多版本共用一份包"的代价，卡内第一行的标题解释那个开关干什么。
// 两者都在 scrollView 的裁剪画布里，所以浮窗必须画在根级；锚点坐标在 hover 时换成客户区。
struct LocTip {
    bool on = false;
    std::string text;          // 段间用 \n
    float x = 0.0f, y = 0.0f;  // 锚点：浮窗从它的右下 6px 开始
    float w = 320.0f;
};
static LocTip g_locTip;
static bool g_locExclaimHover = false;   // 只用来给 `!` 换描边色，不参与浮窗内容
static void locTipShow(const std::string& text, float x, float y, float w) {
    g_locTip.on = true;
    g_locTip.text = text;
    g_locTip.x = x;
    g_locTip.y = y;
    g_locTip.w = w;
    // 不手动 requestUpdate：EUI 在 onHoverChanged 内部已置 composeRequested_，
    // 下一帧必重组；这里再请求一次是纯冗余。
}
static void locTipHide() {
    if (!g_locTip.on) return;
    g_locTip.on = false;
    g_locTip.text.clear();
}

// ---- 界面语言写入被拒时的两个弹窗（§8.5.2 / §8.5.3）----
struct LocDlg {
    bool open = false;
    int kind = 0;          // 0 PXC 在跑 / 1 找不到 keys.json
    std::string want;      // 用户刚才想设成什么（日志用）
    // kind 0 且来自「生成并应用」时带上：骨架来源/已生成/覆盖，展示在弹窗第二段。
    // 此时不再弹成功 toast（toast 会盖住模态弹窗，用户 2026-11-03）。顶部下拉来的是空串。
    std::string genInfo;
};
static LocDlg g_locDlg;
static void composeLocaleDialogs(core::dsl::Ui& ui, const eui::Screen& screen);
// PXC 界面语言（Preferences/<构建号>/keys.json 的 "local"）：变量放前面，
// 是因为 onGenerate 生成完要顺手改它；实现放在汉化页那一节。
static pxc_prefs::LocaleState g_pxcPrefs;
static std::vector<std::string> g_locLangItems;
static int g_locLangSel = -1;
static void refreshPxcPrefs(bool force = false);
static void applyPxcLang(const std::string& lang, const std::string& genInfo = {});
static eui::Signal<std::string> g_langDir{"zh"};
static eui::Signal<std::string> g_exNodeName{"%zh"};
static eui::Signal<std::string> g_exNodeTip{"%zh"};
static eui::Signal<std::string> g_exInName{"%zh"};
static eui::Signal<std::string> g_exOutName{"%zh"};
static eui::Signal<std::string> g_exDd{"%zh"};
static eui::Signal<std::string> g_exText{"%zh"};
static eui::Signal<int> g_preset{0};
static std::vector<std::pair<std::string, std::filesystem::path>> g_fonts; // 显示名 -> 路径
static eui::Signal<int> g_fontUi{-1};
static eui::Signal<int> g_fontCode{-1};


// ---------- 工具 ----------
static std::string toUtf8(const wchar_t* ws) {
    if (!ws) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
    return s;
}
static std::filesystem::path exeDir() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    auto pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(0, pos);
    return std::filesystem::path(p);
}
static std::string registrySignature();   // 登记表指纹（定义在后面，saveConfig 要用）
// 上一次落盘时的登记表指纹。**必须记"落盘时"的值**，不能拿"进 refreshRegistry 时"的值当基准：
// 添加/删除/替换都是先改 g_cfg.registry、再调 refreshRegistry，那样的基准已经是改完的了，
// 前后一样 → 永远不写盘（真机表现：加完 exe 重启就没了）。
static std::string g_regSavedSig;

// 「便携」标记查表（docs/07 §11.4）。卡片是每帧重画的，而 `isPortablePath` 要遍历候选目录
// （`is_directory` + `directory_iterator`）—— 每帧每条都调一次等于在帧循环里做磁盘 IO，
// 违反「dslAppConfig 每帧 20+ 次的地方禁读盘」那条。所以只在登记表变动时算一次缓存。
// key 与 `samePath` 同口径（小写 + 分隔符归一），免得分隔符写法不同查不到。
static std::set<std::string> g_regPortable;
static std::string portableKey(const std::string& p) {
    std::string k = p;
    for (auto& c : k) {
        if (c == '/') c = '\\';
        c = (char)std::tolower((unsigned char)c);
    }
    return k;
}
static bool isPortableEntry(const std::string& exePath) {
    return g_regPortable.count(portableKey(exePath)) > 0;
}

static void saveConfig() {
    if (g_exeDir.empty()) g_exeDir = exeDir();
    pxc_store::save(pxc_nui::userDataDir() / "launcher.json", g_cfg, g_exeDir);
    g_regSavedSig = registrySignature();
}
// 内嵌版本探测的缓存（docs/07 §11.8.2）：与 launcher.json 同目录，丢了只会重扫一次。
static std::filesystem::path probeCachePath() {
    return pxc_nui::userDataDir() / "version_probe.json";
}
static void refreshVersionList() {
    g_versionList.clear();
    for (const auto& e : g_cfg.registry) {
        if (e.product_version.empty()) continue;
        if (std::find(g_versionList.begin(), g_versionList.end(), e.product_version) == g_versionList.end())
            g_versionList.push_back(e.product_version);
    }
    std::sort(g_versionList.begin(), g_versionList.end());
}

// 登记表指纹：只在真变了的时候写盘。启动/启动工程前都会刷新，无条件 save 等于每次启动都重写
// launcher.json（内容一样、mtime 一直变），既没必要也会让人误以为"配置被动过"。
static std::string registrySignature() {
    std::string s;
    s.reserve(g_cfg.registry.size() * 64);
    for (const auto& e : g_cfg.registry) {
        s += e.exe_path;
        s += '|';
        s += e.product_version;
        s += '|';
        s += e.channel;
        s += '|';
        s += e.acked_version;
        s += '|';
        // source 也进指纹：它决定"刷新要不要覆盖版本号"（manual = 用户裁定，不覆盖），
        // 是落盘字段里唯一会改变行为的，改了却没写盘 = 重启后锁丢了。
        s += e.source;
        s += '\n';
    }
    return s;
}

// 这个版本要不要问启动偏好：跨渠道同版本 + 还没表态（prefer 里连键都没有）。
// 三态口径见 §11.6.2：无键 = 没问过；空串 = 问过没表态；渠道名 = 已表态。
static bool needPrefAsk(const std::string& version) {
    if (version.empty()) return false;
    if (!pxc_reg::hasCrossChannel(g_cfg.registry, version)) return false;
    const std::string key = pxc_reg::normalizeVersion(version);
    return !g_cfg.prefer.count(key) && !g_cfg.prefer.count(version);
}

// 一次刷新（docs/07 §11.3）：Steam 重发现 → 存在性 → 重读 PE 版本 → 重建版本索引。
// 只挂在三个用户动作上（启动 / 刷新按钮 / 启动工程前），**绝不进帧循环**。
// 日志里那四个数字是这条链唯一的可观测出口：真机验"3 个库命中 1 个"就靠它。
// 末尾的毫秒数同理：这条链要读注册表 + 遍历 Steam 库 + 扫便携目录 + 逐条开 exe 读版本资源，
// 判"能不能更频繁地调"必须看这个数，别靠估。
static void commitRegistryEdit();   // 定义在后面，refreshRegistry 要复用

static void refreshRegistry(const char* why) {
    const auto t0 = std::chrono::steady_clock::now();
    const int scans0 = pxc_probe::scanCount();
    const int portableAdded = pxc_reg::discoverPortable(g_cfg.registry, g_exeDir);
    const pxc_reg::RefreshResult r = pxc_reg::refreshAll(g_cfg);
    g_regMissing.clear();
    for (const auto& p : r.missing) g_regMissing.insert(p);
    commitRegistryEdit();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    pxc_log::write("INFO", "Registry",
                   std::string("刷新（") + why + "）：便携新增 " + std::to_string(portableAdded) +
                       " / Steam 新增 " + std::to_string(r.steam_added) + " / 重读 " +
                       std::to_string(r.reread) + " / 渠道回填 " +
                       std::to_string(r.channel_filled) + " / 丢失 " +
                       std::to_string((int)r.missing.size()) + " / 变化 " +
                       std::to_string((int)r.changes.size()) + " / 探测 " +
                       std::to_string(pxc_probe::scanCount() - scans0) + " / " + std::to_string(ms) + "ms");
}

// 登记表改完后的公共收尾：索引 / 骨架缓存 / 变更列表 / 落盘 / 探测缓存。
// 单条刷新与「删除条目」共用 —— 它们都不能跑 Steam 重发现（刚删的条目会被加回来）。
static void commitRegistryEdit() {
    refreshVersionList();
    // 「便携」标记缓存随登记表一起重建。这里可以读盘（用户动作触发，不是帧循环）：
    // 以后卡片渲染只查 set，不再调 isPortablePath。
    g_regPortable.clear();
    if (!g_exeDir.empty()) {
        for (const auto& e : g_cfg.registry)
            if (pxc_reg::isPortablePath(e.exe_path, g_exeDir))
                g_regPortable.insert(portableKey(e.exe_path));
    }
    g_skelOptsDirty = true;
    invalidateBase();
    g_regChanges = pxc_reg::pendingChanges(g_cfg);
    // 跨渠道同版本、而这个版本还没问过偏好 → 记下来，等进版本库页时问一次（§11.6.2）。
    g_prefAsk.clear();
    for (const auto& e : g_cfg.registry) {
        if (!needPrefAsk(e.product_version)) continue;
        g_prefAsk = e.product_version;
        break;
    }
    if (registrySignature() != g_regSavedSig) saveConfig();
    pxc_probe::flush(probeCachePath());
}

// 只刷新一条（docs/07 §11.3.1）。用于「添加 exe / 手动登记 / 重新读取版本 / 启动工程前」：
// 这些场景只关心这一条，而全量刷新会顺带做 Steam 重发现与便携扫描 ——
// 既慢（首次会给每条 exe 做版本探测），也会把刚删掉的条目重新发现回来。
static void refreshRegistryEntry(const std::string& exePath, const char* why) {
    const auto t0 = std::chrono::steady_clock::now();
    const int scans0 = pxc_probe::scanCount();
    const pxc_reg::EntryRefresh r = pxc_reg::refreshEntry(g_cfg, exePath);
    if (!r.changedPath.empty()) {
        if (r.missing) g_regMissing.insert(r.changedPath);
        else g_regMissing.erase(r.changedPath);
    }
    commitRegistryEdit();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    pxc_log::write("INFO", "Registry",
                   std::string("刷新单条（") + why + "）：" +
                       (r.found ? "" : "条目不在 / ") +
                       (r.missing ? "丢失" : (r.changed ? "版本已更新" : "无变化")) + " / 探测 " +
                       std::to_string(pxc_probe::scanCount() - scans0) + " / " + std::to_string(ms) +
                       "ms");
}
static const char* formatStr(pxc::Format f) {
    switch (f) { case pxc::Format::JSON: return "JSON"; case pxc::Format::ZLIB: return "ZLIB";
                 case pxc::Format::PXCX: return "PXCX"; default: return "未知"; }
}
static const char* confidenceStr(pxc::Confidence c) {
    switch (c) { case pxc::Confidence::HIGH: return "高"; case pxc::Confidence::MEDIUM: return "中";
                 case pxc::Confidence::LOW: return "低"; default: return "无"; }
}
static core::Color confColor(const std::string& c) {
    if (c == "高") return {0.36f, 0.82f, 0.50f, 1.0f};
    if (c == "中") return {0.95f, 0.78f, 0.36f, 1.0f};
    if (c == "低") return {0.92f, 0.42f, 0.42f, 1.0f};
    return {0.62f, 0.66f, 0.72f, 1.0f};
}
// ---------- 主题色（强调色） ----------
// 只改 theme 里的 primary 一个 token，其余灰阶（面板底 / 分隔线 / 正文）一概不动 ——
// 「换主题色」对用户的语义就是"把我的高亮换成这个颜色"，不是换一套配色。
// 按钮、滑块进度、开关、导航选中、页签下划线、下拉箭头全都从 theme 取色，所以这一处生效即全局跟随。
static constexpr const char* kAccentDefault = "#EC7449";   // 用户指定的默认主题色
static core::Color g_accent = {1.0f, 145.0f / 255.0f, 102.0f / 255.0f, 1.0f};   // = #FF9166

static std::string colorToHex(core::Color c) {
    auto ch = [](float v) {
        int n = (int)(v * 255.0f + 0.5f);
        n = n < 0 ? 0 : (n > 255 ? 255 : n);
        return n;
    };
    const int r = ch(c.r), g = ch(c.g), b = ch(c.b);
    char buf[8];
    buf[0] = '#';
    buf[1] = "0123456789ABCDEF"[(r >> 4) & 0xF];
    buf[2] = "0123456789ABCDEF"[r & 0xF];
    buf[3] = "0123456789ABCDEF"[(g >> 4) & 0xF];
    buf[4] = "0123456789ABCDEF"[g & 0xF];
    buf[5] = "0123456789ABCDEF"[(b >> 4) & 0xF];
    buf[6] = "0123456789ABCDEF"[b & 0xF];
    buf[7] = 0;
    return std::string(buf);
}

// 解析失败返回 false（调用方保留原值）。只认 #RRGGBB，与 store 侧的校验口径一致。
static bool hexToColor(const std::string& s, core::Color& out) {
    if (s.size() != 7 || s[0] != '#') return false;
    auto nib = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = nib(s[(size_t)i + 1]);
        if (v[i] < 0) return false;
    }
    out = {(float)(v[0] * 16 + v[1]) / 255.0f, (float)(v[2] * 16 + v[3]) / 255.0f,
           (float)(v[4] * 16 + v[5]) / 255.0f, 1.0f};
    return true;
}

static components::theme::ThemeColorTokens uiTheme() {
    auto th = components::theme::dark();
    th.primary = g_accent;
    return th;
}

// ---------- 文件/文件夹选择（IFileDialog：可输入地址 + 左侧快速访问） ----------
static std::filesystem::path pickFolderPath(const wchar_t* title) {
    std::filesystem::path p = pxc_nui::pickFolder(title);
    pxc_log::write("INFO", "Pick", std::string("文件夹对话框[") + toUtf8(title) + "] -> " +
                   (p.empty() ? "(取消)" : p.u8string()));
    return p;
}
static std::filesystem::path pickExeFile() {
    // 冒烟钩子：PXC_LAUNCHER_DEBUG_EXE=<exe 路径> 时直接返回它，不弹模态文件对话框。
    // 用途是让"点按钮 → 由 exe 上溯安装目录 → 换骨架"这条链能被真机点击验证
    // （模态对话框没法用合成鼠标点，而这条链的价值恰恰在"推理出目录"这一段）。
    {
        wchar_t b[1024] = {0};
        if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_EXE", b, 1024) > 0) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(std::filesystem::path(b), ec)) {
                pxc_log::write("INFO", "Pick", "exe 对话框被 PXC_LAUNCHER_DEBUG_EXE 跳过 -> " +
                                               pxc_util::toUtf8(b));
                return std::filesystem::path(b);
            }
            pxc_log::write("WARN", "Pick",
                           "PXC_LAUNCHER_DEBUG_EXE 指向的不是文件，回落到对话框：" +
                               pxc_util::toUtf8(b));
        }
    }
    std::filesystem::path p = pxc_nui::pickFile(L"选择 PXC 可执行文件",
                                                {{L"可执行文件", L"*.exe"}, {L"所有文件", L"*.*"}});
    pxc_log::write("INFO", "Pick", "exe 对话框 -> " + (p.empty() ? "(取消)" : p.u8string()));
    return p;
}
static std::filesystem::path pickPxcFile() {
    std::filesystem::path p = pxc_nui::pickFile(L"选择 PXC 工程文件",
                                                {{L"PXC 工程", L"*.pxc"}, {L"所有文件", L"*.*"}});
    pxc_log::write("INFO", "Pick", "pxc 对话框 -> " + (p.empty() ? "(取消)" : p.u8string()));
    return p;
}

// ---------- 版本库页动作 ----------
// 用户操作统一兜底：抛异常只记日志，不让进程消失。
// 状态文字交给各回调自己维护（版本库页写 g_regStatus；汉化页一律走 toast）。
template <class F>
static void guarded(const char* what, F&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        pxc_log::write("ERROR", "App", std::string(what) + " 异常：" + pxc_util::autoToUtf8(e.what()));
    } catch (...) {
        pxc_log::write("ERROR", "App", std::string(what) + " 未知异常");
    }
}

// 登记一个 exe。三条路按顺序判：
//   ① PE 版本读不到 → 弹「无法识别当前 PXC 版本，请设置版本号」（§11.8，取消即不登记）
//   ② 同渠道已有同版本的另一个副本 → 弹二选一（§11.6.1 A，两份一样的 PXC 没有意义）
//   ③ 正常登记，并定下渠道与"已确认版本"
static void openVerDlg(const std::filesystem::path& p) {
    g_pendingExe = p;
    g_manualVersion.set("");
    g_verDlgOpen = true;
    pxc_log::write("WARN", "Registry", "PE 版本读取失败，弹窗要求手动指定: " + p.u8string());
    requestUpdate();
}

// 登记后就地补两件 addExe/addManual 不管的事：渠道、以及 Steam 条目的 acked_version。
// 为什么不在 addExe 里做：它被 headless 测试直接调用，判渠道要读注册表 + vdf，不该塞进那个纯函数。
static void stampChannel(const std::filesystem::path& p, const std::string& channel) {
    for (auto& e : g_cfg.registry) {
        if (!pxc_reg::samePath(e.exe_path, p.u8string())) continue;
        e.channel = channel;
        // 首次登记不算"版本变化"：acked 跟上当前版本，否则第一张 Steam 卡就带「!」（§11.6 初值）
        if (channel == "steam") e.acked_version = e.product_version;
        break;
    }
}

static void registerExe(const std::filesystem::path& p) {
    if (p.empty()) return;
    const std::string ch = pxc_reg::detectChannel(p);
    std::string ver = pxc::readExeVersion(p);
    if (ver.empty()) { openVerDlg(p); return; }
    if (const auto* dup = pxc_reg::findSameChannelDup(g_cfg.registry, p.u8string(), ver, ch)) {
        g_dup = {true, p, ver, ch, dup->exe_path};
        pxc_log::write("WARN", "Registry",
                       std::string("同渠道同版本重复：") + p.u8string() + " vs " + dup->exe_path);
        requestUpdate();
        return;
    }
    std::string out;
    pxc_reg::addExe(g_cfg.registry, p, out);
    stampChannel(p, ch);
    g_baseReady = false;   // 登记表变了，安装目录/版本可能跟着变
    refreshRegistryEntry(p.u8string(), "添加 exe");
    requestUpdate();
}

static void onAddExe() {
    std::filesystem::path p = pickExeFile();
    if (p.empty()) { requestUpdate(); return; }
    guarded("添加 exe", [p] { registerExe(p); });
}

// 「用新的替换」= 删旧**条目**、登记新的；**不动磁盘文件**（我们不负责删用户的 PXC）。
static void resolveDup(bool useNew) {
    if (!g_dup.open) return;
    const RegDup d = g_dup;
    g_dup = RegDup{};
    if (useNew) {
        pxc_reg::removeExe(g_cfg.registry, d.existing);
        std::string out;
        pxc_reg::addExe(g_cfg.registry, d.path, out);
        if (out.empty()) pxc_reg::addManual(g_cfg.registry, d.path, d.version);
        stampChannel(d.path, d.channel);
        g_baseReady = false;
        pxc_log::write("INFO", "Registry", "替换同版本副本：" + d.existing + " -> " + d.path.u8string());
    }
    if (useNew) refreshRegistryEntry(d.path.u8string(), "替换同版本副本");
    else { pxc_log::write("INFO", "Registry", "保留现有副本：" + d.existing); commitRegistryEdit(); }
    requestUpdate();
}

static void onConfirmManual() {
    if (g_pendingExe.empty()) return;
    std::string v = g_manualVersion.get();
    // 去掉首尾空白：版本号里不会有空白，有就是误输入
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n'))
        v.pop_back();
    if (v.empty()) { requestUpdate(); return; }      // 空值不许提交：按钮不响应
    const std::string ch = pxc_reg::detectChannel(g_pendingExe);
    if (const auto* dup = pxc_reg::findSameChannelDup(g_cfg.registry, g_pendingExe.u8string(), v, ch)) {
        g_verDlgOpen = false;
        g_dup = {true, g_pendingExe, v, ch, dup->exe_path};
        g_pendingExe.clear();
        requestUpdate();
        return;
    }
    pxc_reg::addManual(g_cfg.registry, g_pendingExe, v);
    stampChannel(g_pendingExe, ch);
    pxc_log::write("INFO", "Registry", "手动登记：" + g_pendingExe.u8string() + " 版本=" + v);
    // 单条刷新要用路径，但上面随即会清空 g_pendingExe，先留一份
    const std::string justSet = g_pendingExe.u8string();
    g_verDlgOpen = false;
    g_pendingExe.clear();
    g_baseReady = false;
    refreshRegistryEntry(justSet, "手动登记");
    requestUpdate();
}

static void onCancelManual() {
    g_verDlgOpen = false;
    g_pendingExe.clear();
    pxc_log::write("INFO", "Registry", "取消手动登记（该 exe 不进入版本库）");
    requestUpdate();
}

static void onRemoveExe(const std::string& exePath) {
    pxc_reg::removeExe(g_cfg.registry, exePath);
    if (pxc_reg::samePath(g_regSel, exePath)) g_regSel.clear();
    g_regMissing.erase(exePath);
    g_baseReady = false;
    // 只做收尾，**不做** Steam 重发现：删掉的若是 Steam 条目，重发现会立刻把它加回来
    pxc_log::write("INFO", "Registry", "删除登记：" + exePath);
    commitRegistryEdit();
    requestUpdate();
}

// 「重新读取版本」：用户主动重读 = 已确认，acked 一起推进（§11.7 的唯一两个推进点之一）。
// 同时把 source 改回 auto：这一项是「设置版本号」那次锁定的**唯一出口** ——
// 手动指定的版本号刷新时不会被 PE 值覆盖，所以要恢复自动跟踪只能走这里。
static void onRereadVersion(const std::string& exePath) {
    // 先解锁（source=auto），再让单条刷新去读 —— 读 PE、必要时扫内嵌数据包，一套逻辑只写一遍。
    for (auto& e : g_cfg.registry) {
        if (!pxc_reg::samePath(e.exe_path, exePath)) continue;
        e.source = "auto";
        break;
    }
    refreshRegistryEntry(exePath, "重新读取版本");
    // 用户主动重读 = 已确认：acked 跟上当前版本（§11.7 的唯一两个推进点之一）
    for (auto& e : g_cfg.registry) {
        if (!pxc_reg::samePath(e.exe_path, exePath)) continue;
        if (!e.acked_version.empty()) e.acked_version = e.product_version;
        break;
    }
    commitRegistryEdit();
    requestUpdate();
}

// 卡片双击：用这一张的 exe 启动 PXC（空工程）。
//
// 走 pxc_runner 只是**没有** pxcPath 而已 —— worker 那边看到空路径就跳过复用直接首启。
// 为什么不再直接 ShellExecuteW：
//   · 那条路完全不发提示窗（用户 2026-11-04：「双击版本库卡片右下角没反应」），
//     用户只有"点了没点着"的猜测；
//   · 它也不进启动队列，会和正在跑的工程启动撞在一起（两个 CreateProcess 抢同一个 exe）。
// 换来的是：右下角滚"PXC 启动中 已 N 秒"→"已启动"，失败有原因，并且排在工程启动后面。
static void launchExeNow(const std::string& exePath) {
    const std::filesystem::path exe = pxc_util::pathFromUtf8(exePath);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(exe, ec)) {
        pxc_log::write("WARN", "Registry", "启动失败，exe 已不在：" + exePath);
        refreshRegistryEntry(exePath, "启动失败重扫");
        requestUpdate();
        return;
    }
    // 启动前按这个 exe 的版本重写一遍语言包（开关关着就什么都不做）。
    // 仍留在主线程：buildAutoGenRequest 要读 g_cfg / 对照表，worker 不碰 UI 状态。
    std::string ver;
    for (const auto& e : g_cfg.registry)
        if (pxc_reg::samePath(e.exe_path, exePath)) {
            ver = e.product_version;
            runAutoGenFor(exe, ver);
            break;
        }

    pxc_runner::Job job;
    job.exePath = exe;
    job.version = ver;
    job.fileName = "PixelComposer";   // 没有工程文件名可显示
    job.allowReuse = false;           // 空启动没有工程可交给已有实例
    job.spawn = [exe](const pxc_runner::SpawnHooks& h,
                      const std::filesystem::path&) -> pxc_runner::SpawnOutcome {
        pxc_runner::SpawnOutcome so;
        // 空启动没有副本、没有备份，只有"拉开"这一步 —— 自己报，因为 startProject 那条
        // 链才会上报阶段。报完按钮即置灰：进程马上就在了，取消已经没有意义。
        if (h.stage) h.stage(pxc_stage::Stage::Spawn);
        const auto r = pxc_launch::launchExe(exe, {});   // 空 pxcPath = 只起程序
        if (r.ok) {
            so.ok = true;
            so.pid = r.pid;
            return so;
        }
        so.detail = r.detail.empty() ? "无法启动 PXC" : r.detail;
        return so;
    };

    const bool queued = pxc_runner::submit(job);
    pxc_log::write("INFO", "Registry", "启动 PXC：" + exePath);
    g_wb.status = queued ? "已排队：PixelComposer（等前一个启动完成）" : "正在启动 PixelComposer…";
    requestUpdate();
}

static void resumePrefLaunch(const std::string& version);

// 双击卡片启动。这个版本两个渠道都有、又没表态过 → 先问一句再起（§11.6.2 触发点二）：
// 这一次点击本身就是"我要启动这个版本"，而"用哪个 exe"正是偏好在回答的问题。
static void launchRegistryExe(const std::string& exePath) {
    for (const auto& e : g_cfg.registry) {
        if (!pxc_reg::samePath(e.exe_path, exePath)) continue;
        if (needPrefAsk(e.product_version)) {
            g_prefAsk = e.product_version;
            g_prefLaunchExe = exePath;
            g_prefDlgOpen = true;
            requestUpdate();
            return;
        }
        break;
    }
    launchExeNow(exePath);
}

// 偏好问完（选了或"稍后决定"）→ 把被拦下的那次启动补上，按偏好选 exe。
static void resumePrefLaunch(const std::string& version) {
    if (g_prefLaunchExe.empty()) return;
    const std::string pend = g_prefLaunchExe;
    g_prefLaunchExe.clear();
    std::vector<pxc_store::ExeEntry> matches;
    for (const auto& e : g_cfg.registry)
        if (pxc_reg::sameVersion(e.product_version, version)) matches.push_back(e);
    if (matches.empty()) {
        launchExeNow(pend);   // 极端情况：等待期间登记表被改了，退回用户点的那一个
        return;
    }
    const std::size_t i = pxc_reg::choosePreferred(matches, g_cfg.prefer, version);
    launchExeNow(matches[i].exe_path);
}

// 偏好落地：value 为空串 = 问过但没表态（走兜底、以后不再问）。
static void setPrefer(const std::string& version, const std::string& channel) {
    g_cfg.prefer[pxc_reg::normalizeVersion(version)] = channel;
    saveConfig();
    g_prefAsk.clear();
    pxc_log::write("INFO", "Registry",
                   "启动偏好：" + pxc_reg::normalizeVersion(version) + " -> " +
                       (channel.empty() ? "(未表态)" : channel));
    requestUpdate();
}

// ---------- 工程文件页动作 ----------
static bool isPxcFile(const std::filesystem::path& p) {
    std::wstring ext = p.extension().wstring();
    return ext == L".pxc" || ext == L".PXC";
}
static std::filesystem::path backupDirPath() {
    return g_cfg.backup.dir.empty() ? (pxc_nui::userDataDir() / "pxc_backups")
                                    : pxc_util::pathFromUtf8(g_cfg.backup.dir);
}

// 打开缺版本协商浮窗：文件识别到的版本本机没有登记，让用户从已登记版本里挑。
// registryFresh=true 表示调用方**刚刚**刷过（wbLaunchPath 判完 matches.empty() 就是这种），
// 此时再刷一遍纯属重复：那是注册表读 + Steam 库遍历 + 便携扫描 + 逐条开 exe 读版本资源。
static void openVersionPicker(const std::filesystem::path& p, bool registryFresh = false) {
    // 协商要给候选，候选来自登记表 —— 先刷新，否则"刚装的新版本"不会出现在候选里（§11.3）。
    if (!registryFresh) refreshRegistry("启动工程前");
    g_pick.path = p;
    g_pick.advice = pxc_adv::advise(g_cfg.registry, pxc::detectPxc(p));
    g_pick.sel = 0;
    g_pick.open = true;
    g_silentLaunch = false;
    // 协商窗**必须被看见**：它是唯一一个"用户不处理就永远卡在那"的浮窗。
    //
    // 以前只在静默启动时提窗口，于是"启动器已在后台（最小化在任务栏 / 被别的程序盖住）
    // + 双击一个缺版本的工程"这条路从头到尾没人把窗口叫出来 —— 用户看到的是"双击了没反应"，
    // 而实际上右下角/后台正等着他选版本（2026-11-04 用户反馈）。
    // 现在无条件提一次；提不上（窗口还没建 / 前台被全屏程序占着）就留几次重试给后面的帧。
    if (pxc_nui::showMainWindow()) {
        pxc_log::write("INFO", "Launch", "缺版本协商：已把启动器窗口提到前台");
    } else {
        g_fgRetry = 2;
        pxc_log::write("WARN", "Launch", "缺版本协商：提窗口失败，下一帧再试");
    }
    pxc_log::write("WARN", "Launch", "缺版本、进入协商：文件=" + p.u8string() +
                                        " 识别版本=" + g_pick.advice.fileVersion);
    requestUpdate();
}

static void closeVersionPicker() {
    g_pick.open = false;
    g_pick.path.clear();
    g_pick.advice = pxc_adv::Advice{};
    g_pick.sel = 0;
    requestUpdate();
}

// 协商结果落地。两种模式的差别只有一处：副本模式先复制、再打开副本，原文件全程只读。
//
// 走的是和工程双击完全同一条链（submitProjectLaunch）：右下角有阶段提示、进启动队列、
// 建副本期间能取消。以前这里直接同步调 startProject —— 于是三条路里只有这一条既没有提示、
// 也不能取消，而"创建副本并启动"恰恰是唯一会往工程目录写文件的那条。
static void submitProjectLaunch(const std::filesystem::path& p, const std::string& exe,
                                const std::string& version, pxc_project::StartMode mode);

static void pickLaunch(pxc_project::StartMode mode) {
    if (!g_pick.open) return;
    if (g_pick.sel < 0 || g_pick.sel >= (int)g_pick.advice.ordered.size()) return;
    const std::filesystem::path p = g_pick.path;
    const pxc_adv::Candidate cand = g_pick.advice.ordered[(size_t)g_pick.sel];
    // 先把浮窗收掉再提交：提交后会立刻开提示窗，两个浮层叠在右下角只会看见上面那个。
    closeVersionPicker();
    pxc_log::write("INFO", "Launch",
                   std::string("协商结果：") + (mode == pxc_project::StartMode::Copy ? "建副本启动" : "直接打开") +
                       " | " + p.u8string() + " | " + cand.version);
    submitProjectLaunch(p, cand.exe_path, cand.version, mode);
}


// ---------- 汉化页（运行期）：基于对照表生成语言文件 ----------
// 对照表是随程序分发的只读资源；骨架取目标 PXC 版本的英文语言文件；写入用户目录 Locale/<lang>。
static std::filesystem::path dataDir() { return g_exeDir / "data"; }
static std::filesystem::path dbPath() { return dataDir() / "translation_db.json"; }
// 构建器过程文件：编辑期每改动一次就落一份，关掉软件/崩溃都不丢。
// 放 userdata/.draft/（隐藏式目录名），不参与分发，也不会被误当成正式表。
static std::filesystem::path draftPath() { return pxc_nui::userDataDir() / ".draft" / "db_draft.json"; }
static std::filesystem::path fontsRoot() { return dataDir() / "fonts"; }
static std::filesystem::path notesZhRoot() { return pxc_nui::userDataDir() / "notes_zh"; }

static std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tm);
    return std::string(b);
}

// Steam zip 的解压缓存根（docs/08 §8.1.7.3）：`<启动器目录>/skeleton/<版本>/`。
// 它是**长期缓存**而不是临时目录 —— 同一个版本每次启动前都要拿一次骨架，内容没变就不该重解。
// 启动器装在 Program Files 下写不进去时退 %LOCALAPPDATA%\PixelComposer-Launcher\skeleton\。
static std::filesystem::path skeletonCacheRoot() {
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    auto writable = [](const std::filesystem::path& dir) {
        std::error_code e;
        std::filesystem::create_directories(dir, e);
        if (e) return false;
        const std::filesystem::path probe = dir / ".write_test";
        FILE* fp = _wfopen(probe.wstring().c_str(), L"wb");
        if (!fp) return false;
        fclose(fp);
        std::filesystem::remove(probe, e);
        return true;
    };
    std::filesystem::path p = pxc_nui::userDataDir() / "skeleton";
    if (!writable(p)) {
        wchar_t buf[512] = {0};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 512);
        if (n > 0 && n < 512) {
            std::filesystem::path alt =
                std::filesystem::path(std::wstring(buf, n)) / "PixelComposer-Launcher" / "skeleton";
            if (writable(alt)) p = alt;
        }
    }
    cached = p;
    return cached;
}

// ---- toast ----
// 失败不自动消失（duration 0）：用户必须看见"没成"，而不是错过一条自动滑走的浮窗。
static void showToast(const std::string& title, const std::string& msg, bool error) {
    g_toast.on = true;
    g_toast.title = title;
    g_toast.msg = msg;
    g_toast.error = error;
    requestUpdate();
}

static void ensureDb() {
    if (g_dbLoaded) return;
    auto [db, ok] = pxc_tdb::load(dbPath());
    g_db = db;
    g_dbLoaded = ok;
    if (!ok) pxc_log::write("WARN", "Gen", "对照表不存在或解析失败: " + dbPath().u8string());
}

// 对照表一行摘要（页面 C 行那句灰字）
static std::string dbLabel() {
    if (!g_dbLoaded)
        return "未找到对照表 data/translation_db.json —— 请先用构建器生成";
    return "对照表 " + (g_db.version.empty() ? std::string("(未标注版本)") : g_db.version) + " · " +
           std::to_string((int)g_db.entries.size()) + " 条";
}

static void scanFonts() {
    if (!g_fonts.empty()) return;
    std::error_code ec;
    auto addDir = [&](const std::filesystem::path& d, const std::string& tag) {
        if (d.empty() || !std::filesystem::is_directory(d, ec)) return;
        for (auto& it : std::filesystem::directory_iterator(d, ec)) {
            if (!it.is_regular_file(ec)) continue;
            std::wstring ext = it.path().extension().wstring();
            if (ext != L".ttf" && ext != L".otf" && ext != L".TTF" && ext != L".OTF") continue;
            g_fonts.emplace_back(tag + pxc_util::toUtf8(it.path().filename().wstring().c_str()), it.path());
        }
    };
    addDir(fontsRoot(), "[内置] ");
    wchar_t windir[MAX_PATH] = {0};
    GetWindowsDirectoryW(windir, MAX_PATH);
    if (windir[0]) addDir(std::filesystem::path(std::wstring(windir)) / "Fonts", "[系统] ");
}

static int fontIndex(const std::string& p) {
    if (p.empty()) return -1;
    for (int i = 0; i < (int)g_fonts.size(); ++i)
        if (g_fonts[i].second.u8string() == p) return i;
    return -1;
}

// ---- 已装版本清单（「目标版本」下拉） ----
// 只在三种时机重建：首次进页、登记表变了、用户手动点重新定位。
// 数节点要读一次各版本的 nodes.json（Steam 还要解一次 zip），所以绝不能进帧循环。
static void refreshSkelOpts() {
    if (!g_skelOptsDirty) return;
    g_skelOpts = pxc_base::scanInstalled(g_cfg.registry, skeletonCacheRoot(), true);
    g_skelOptsDirty = false;

    // 清理：skeleton/ 里版本号已不在登记表中的子目录（换版本会残留，一份约 850 KB）
    std::error_code ec;
    const std::filesystem::path root = skeletonCacheRoot();
    for (auto& it : std::filesystem::directory_iterator(root, ec)) {
        if (!it.is_directory(ec)) continue;
        const std::string name = pxc_util::toUtf8(it.path().filename().wstring().c_str());
        bool keep = false;
        for (const auto& v : g_skelOpts)
            if (pxc_reg::sameVersion(v.version, name)) keep = true;
        if (!keep) std::filesystem::remove_all(it.path(), ec);
    }
}

static int skelOptIndex(const std::string& v) {
    if (v.empty()) return -1;
    for (int i = 0; i < (int)g_skelOpts.size(); ++i)
        if (pxc_reg::sameVersion(g_skelOpts[(size_t)i].version, v)) return i;
    return -1;
}

// 这次生成用哪个版本的骨架：用户选的那个；没选 = 已装版本里最高的（§8.2.4）。
static std::string effectiveTargetVersion() {
    const std::string saved = pxc_reg::normalizeVersion(g_cfg.gen.target_version);
    if (!saved.empty() && skelOptIndex(saved) >= 0) return saved;
    for (const auto& v : g_skelOpts)
        if (v.hasSkeleton) return v.version;
    return {};
}

// 用户目录三态（§8.7.4）：0 = 可用 / 1 = 定位到了但 PXC 没跑过 / 2 = 没定位到
static int userDirState() {
    const std::filesystem::path d = pxc_util::pathFromUtf8(g_cfg.gen.user_dir);
    if (d.empty()) return 2;
    std::error_code ec;
    if (!std::filesystem::is_directory(d, ec)) return 2;
    if (!std::filesystem::is_directory(d / "Locale", ec)) return 1;
    return 0;
}

static void onAutoUserDir() {
    auto dirs = pxc_base::findUserDirs();
    if (dirs.empty()) {
        showToast("没找到 PXC 用户目录", "启动一次 PXC 后再点「重新定位」，它会建出默认目录", true);
        requestUpdate();
        return;
    }
    g_cfg.gen.user_dir = dirs[0].u8string();
    invalidateBase();
    saveConfig();
    refreshPxcPrefs(true);
    pxc_log::write("INFO", "Prefs", "定位用户目录：" + g_cfg.gen.user_dir);
    requestUpdate();
}

static void onPickUserDir() {
    std::filesystem::path p = pickFolderPath(L"选择 PXC 用户目录（含 Locale 文件夹的那一层）");
    if (p.empty()) { requestUpdate(); return; }
    std::error_code ec;
    if (std::filesystem::exists(p / "Locale", ec)) { /* 用户目录本身 */ }
    else if (p.filename().wstring() == L"Locale") p = p.parent_path();
    g_cfg.gen.user_dir = p.u8string();
    invalidateBase();
    saveConfig();
    pxc_log::write("INFO", "Prefs", "用户目录设为：" + g_cfg.gen.user_dir);
    requestUpdate();
}

static void onApplyPreset(int p) {
    pxc_gen::ExprSet ex = pxc_gen::ExprSet::preset(p);
    g_cfg.gen.preset = p;
    g_cfg.gen.ex_node_name = ex.nodeName;
    g_exNodeName.set(ex.nodeName);
    g_exNodeTip.set(ex.nodeTip);
    g_exInName.set(ex.inName);
    g_exOutName.set(ex.outName);
    g_exDd.set(ex.dd);
    g_exText.set(ex.text);
    g_preset.set(p);
    saveConfig();
    pxc_log::write("INFO", "Gen", std::string("套用预设 ") +
                                    (p == 0 ? "纯中文" : (p == 1 ? "中文在前" : "英文在前")));
    requestUpdate();
}

static pxc_gen::ExprSet currentExpr() {
    pxc_gen::ExprSet ex;
    auto pick = [](const std::string& s) { return s.empty() ? std::string("%zh") : s; };
    ex.nodeName = pick(g_exNodeName.get());
    ex.nodeTip = pick(g_exNodeTip.get());
    ex.inName = pick(g_exInName.get());
    ex.outName = pick(g_exOutName.get());
    ex.dd = pick(g_exDd.get());
    ex.text = pick(g_exText.get());
    return ex;
}

// 组一份"启动前自动生成"的请求。所有字段都在**主线程**读一次、按值带走 ——
// worker 不碰 UI 状态是硬纪律（launch_runner.h 的边界说明），而 g_cfg / g_db 都是 UI 侧变量。
// 返回 false = 开关没开，调用方直接跳过。
static bool buildAutoGenRequest(pxc_autogen::Request& r) {
    if (!g_cfg.gen.auto_gen_on_launch) return false;
    ensureDb();              // 对照表懒加载：worker 里不能触发（g_dbLoaded 是 UI 侧状态）
    refreshPxcPrefs(true);   // 取 keys.json 当前的 local，用来判断"这份包是不是我们管的"
    r.db = g_dbLoaded ? &g_db : nullptr;
    r.overrides = &g_cfg.locale.overrides;
    r.expr = currentExpr();
    const int iu = g_fontUi.get(), ic = g_fontCode.get();
    if (iu >= 0 && iu < (int)g_fonts.size()) r.font.uiFont = g_fonts[(size_t)iu].second;
    if (ic >= 0 && ic < (int)g_fonts.size()) r.font.codeFont = g_fonts[(size_t)ic].second;
    if (r.font.uiFont.empty() && !g_fonts.empty()) r.font.uiFont = g_fonts.front().second;
    r.opt.writeUi = g_cfg.gen.write_ui;
    r.opt.writeFonts = g_cfg.gen.write_fonts;
    r.opt.writeNotes = g_cfg.gen.write_notes;
    r.opt.notesZhDir = notesZhRoot();
    r.opt.overrides = &g_cfg.locale.overrides;
    r.userDir = pxc_util::pathFromUtf8(g_cfg.gen.user_dir);
    r.cacheRoot = skeletonCacheRoot();
    r.registry = g_cfg.registry;
    r.langDir = g_langDir.get();
    r.installedLang = g_pxcPrefs.current;
    return true;
}

// 同步版：版本库双击 / 缺版本协商这两条路本来就在主线程直接 ShellExecute，
// 多等这一次生成（实测 191 ms，内容没变时 ~0）比把这两条路改造成 worker 任务划算得多。
static void runAutoGenFor(const std::filesystem::path& exe, const std::string& version) {
    pxc_autogen::Request r;
    if (!buildAutoGenRequest(r)) return;
    if (exe.empty() || version.empty()) return;
    const auto gr = pxc_autogen::beforeLaunch(r, exe, version);
    if (gr.ran && !gr.ok)
        pxc_log::write("WARN", "Gen", "启动前生成失败（照常启动）：" + gr.message);
    else if (gr.ran && gr.ok && !gr.skipped && !gr.base.version.empty() &&
             !pxc_reg::sameVersion(gr.base.version, version))
        pxc_log::write("WARN", "Gen", "骨架版本对不上：目标 " + version + " 用了 " + gr.base.version);
}

// 「生成并应用」：生成 + 无条件把 PXC 界面语言切到新目录（§8.6.0）。
// 结果走 toast（§8.6.1 五行），页面不再有状态行。
static void onGenerate() {
    ensureDb();
    scanFonts();
    if (!g_dbLoaded) {
        showToast("对照表缺失", "data/translation_db.json 未找到 —— 先用构建器生成一份", true);
        requestUpdate();
        return;
    }
    const std::filesystem::path userDir = pxc_util::pathFromUtf8(g_cfg.gen.user_dir);
    const int uds = userDirState();
    if (uds == 2) {
        showToast("没定位到 PXC 用户目录", "启动一次 PXC 后再点「重新定位」", true);
        requestUpdate();
        return;
    }
    if (uds == 1) {
        showToast("PXC 还没运行过", "先启动一次 PXC，它才会建出 Locale 目录，之后整条链都通", true);
        requestUpdate();
        return;
    }

    std::string typed = g_langDir.get();
    std::string lang = pxc_util::sanitizeDirName(typed);
    if (lang.empty()) {
        showToast("语言名不能为空", "只填目录名，如 zh", true);
        requestUpdate();
        return;
    }
    if (lang != typed) g_langDir.set(lang);   // 回填清洗后的名字

    pxc_log::write("INFO", "Gen", "生成请求: lang=" + lang + " | 用户目录=" + g_cfg.gen.user_dir +
                                   " | 自定义译文 " + std::to_string((int)g_cfg.locale.overrides.size()) +
                                   " 项");

    if (!ensureBase()) {
        showToast("找不到可用的英文骨架", g_base.note.empty()
                                             ? "先去版本库登记一个 PXC"
                                             : g_base.note + "（先去版本库登记一个 PXC）",
                  true);
        requestUpdate();
        return;
    }
    const pxc_base::Baseline base = g_base;   // 与自定义译文层编辑器共用同一份缓存

    pxc_gen::ExprSet ex = currentExpr();
    pxc_gen::FontPick fp;
    const int ui = g_fontUi.get(), cf = g_fontCode.get();
    if (ui >= 0 && ui < (int)g_fonts.size()) fp.uiFont = g_fonts[(size_t)ui].second;
    if (cf >= 0 && cf < (int)g_fonts.size()) fp.codeFont = g_fonts[(size_t)cf].second;
    if (fp.uiFont.empty() && !g_fonts.empty()) fp.uiFont = g_fonts.front().second;

    pxc_gen::Options opt;
    opt.writeUi = g_cfg.gen.write_ui;
    opt.writeFonts = g_cfg.gen.write_fonts;
    opt.writeNotes = g_cfg.gen.write_notes;
    opt.notesZhDir = notesZhRoot();
    opt.overrides = &g_cfg.locale.overrides;

    pxc_gen::Stats st;
    try {
        st = pxc_gen::generate(base, g_db, ex, fp, opt, userDir / "Locale", lang);
    } catch (const std::exception& e) {
        showToast("生成失败", pxc_util::autoToUtf8(e.what()), true);
        pxc_log::write("ERROR", "Gen", std::string("生成异常: ") + pxc_util::autoToUtf8(e.what()));
        requestUpdate();
        return;
    }
    g_lastGen = st;
    g_hasGen = true;

    // 无论成败先把"这次怎么生成的"记进配置：下次进来页面要显示同一套选择
    g_cfg.gen.lang_dir = lang;
    g_cfg.gen.ex_node_name = ex.nodeName;
    g_cfg.gen.ex_node_tip = ex.nodeTip;
    g_cfg.gen.ex_in_name = ex.inName;
    g_cfg.gen.ex_out_name = ex.outName;
    g_cfg.gen.ex_dd = ex.dd;
    g_cfg.gen.ex_text = ex.text;
    if (!fp.uiFont.empty()) g_cfg.gen.ui_font = fp.uiFont.u8string();
    if (!fp.codeFont.empty()) g_cfg.gen.code_font = fp.codeFont.u8string();
    saveConfig();

    if (!st.ok) {
        showToast("生成失败", st.error, true);
        pxc_log::write("ERROR", "Gen", "生成失败: " + st.error);
        requestUpdate();
        return;
    }

    // 骨架 + 对照表都没变的话，启动前自动生成可以跳过这次活
    pxc_autogen::writeStamp(userDir / "Locale", lang, base.version, g_db.version);

    // 内容按用户 2026-11-03 给的定稿（四行，首行当标题）：
    //   下次启动 PXC 时生效 / 骨架来源：<origin>（<路径>），节点 N / 已生成：<目录> /
    //   用户偏好覆盖生效 N 处（0 时整行省略）
    // 第二行是唯一能从结果侧看出"降级了"的地方 —— 降级来源加 ⚠。
    std::string msg;
    msg += (pxc_base::isDegradedOrigin(base.origin) ? "⚠ " : "");
    msg += "骨架来源：" + base.origin + "（" +
           (base.sourcePath.empty() ? base.dir : base.sourcePath).u8string() + "），节点 " +
           std::to_string(base.nodeCount);
    msg += "\n已生成：" + st.outDir;
    if (st.ovHits > 0) msg += "\n用户偏好覆盖生效 " + std::to_string(st.ovHits) + " 处";
    // 覆盖率只写 log：界面上那行"97.7% · 399 条保留英文"对用户没有意义（2026-11-03）。
    {
        std::string line = "生成完成: " + st.outDir + " | 骨架 " + base.origin + "（" +
                           (base.version.empty() ? std::string("?") : base.version) + "）";
        if (st.fields > 0) {
            char cov[96];
            std::snprintf(cov, sizeof(cov), " | 对照表覆盖 %.1f%% · %d 条保留英文",
                          (double)st.translated * 100.0 / (double)st.fields, st.fallbackEn);
            line += cov;
        }
        if (st.normHits > 0)
            line += " | 含 " + std::to_string(st.normHits) + " 条端口归一化命中";
        pxc_log::write("INFO", "Gen", line);
    }

    // 「应用」：无条件切到这次生成的语言（不是"本来就是我们的"才切，§8.6.0）。
    // 成功 toast 与语言弹窗**不同时弹**（用户 2026-11-03）：PXC 开着且语言确实要改时，
    // toast 会盖在模态弹窗上；此时只留弹窗，生成信息并进弹窗第二段。
    applyPxcLang(lang, msg);
    if (!g_locDlg.open) showToast("下次启动 PXC 时生效", msg);
    requestUpdate();
}

// 把 PXC 的界面语言切成 lang。门序（§8.5.1）：
// ① 值本来就是这个 → 静默返回（**排在 PXC 在跑之前**：PXC 开着但语言已经是对的，
//    弹"修改失败"是在报一个没发生的错误 —— 用户 2026-11-03）；
// ② PXC 在跑 → 弹窗 + 不改（genInfo 非空时弹窗带生成信息）；③ 没有 keys.json → 弹窗 + 不改；
// ④ 写盘失败 → 红 toast + 回滚下拉；⑤ 成功。
// **回滚是重点**：下拉只在写成功后推进，失败时重新探测，显示的永远是 keys.json 的真实值。
// （默认参数 genInfo = {} 只写在前置声明上，定义处重复写会 C2572。）
static void applyPxcLang(const std::string& lang, const std::string& genInfo) {
    refreshPxcPrefs(true);
    if (!g_pxcPrefs.prefsFile.empty() && g_pxcPrefs.current == lang) {
        refreshPxcPrefs(true);   // 值本来就是这个：不写盘，也不弹任何东西
        return;
    }
    if (pxc_prefs::pxcRunning()) {
        g_locDlg.kind = 0;
        g_locDlg.want = lang;
        g_locDlg.genInfo = genInfo;   // 顶部下拉是空串 → 弹窗走"直接修改"文案
        g_locDlg.open = true;
        refreshPxcPrefs(true);   // 回滚：下拉弹回 keys.json 里的值
        pxc_log::write("WARN", "Prefs", "拒绝写入：PXC 正在运行（请求 " + lang + "）");
        return;
    }
    if (g_pxcPrefs.prefsFile.empty()) {
        g_locDlg.kind = 1;
        g_locDlg.want = lang;
        g_locDlg.genInfo.clear();
        g_locDlg.open = true;
        refreshPxcPrefs(true);
        pxc_log::write("WARN", "Prefs", "没有偏好文件（userDir=" + g_cfg.gen.user_dir + "）");
        return;
    }
    std::string err;
    if (!pxc_prefs::writeLocal(g_pxcPrefs.prefsFile, lang, err)) {
        showToast("写入失败", err, true);
        pxc_log::write("WARN", "Prefs", "写 local 失败：" + err);
        refreshPxcPrefs(true);   // 回滚
        return;
    }
    pxc_log::write("INFO", "Prefs", "local = " + lang);
    refreshPxcPrefs(true);
}

// ---------- 自定义译文层（个性化微调） ----------
// 用户只改自己不习惯的那几处；对照表本身仍然只读、随程序分发。
// 键与对照表完全同构（nodes.<id>.name / .in.<英文端口>.name / words.<k> / ui.<k> / notes.<文件>），
// 生成时在「查表」之前多查一层，不需要第二套结构。
//
// 分页：节点 / 词条 / 界面 / 注释 —— 对应四类可覆盖的文案来源。
// 性能约定（列表曾卡到不可用）：
//   1. 每页条目**懒构建一次**（切页才建），搜索用小写副本预计算，过滤不做任何字符串分配；
//   2. 右侧字段区用 virtualList 虚拟化，只渲染可见行；
//   3. 保存时只更新被编辑那一条的 zh/marked，**不重建整页** —— 曾因每次保存重建 942 条 + 重算过滤导致卡顿。
enum class OvPage { Nodes = 0, Words, Ui, Notes };
static constexpr int kOvPages = 4;
static const char* const kOvPageName[kOvPages] = {"节点", "词条", "界面", "注释"};

// 构建器（维护者用）：与自定义译文层是**同一套浮窗**，只换数据源与出口 ——
// 用户改的是 launcher.json 的 overrides（差异小集合），维护者改的是 data/translation_db.json 本身。
// 没有第二套界面：分页、搜索、虚拟列表、行编辑全部共用，避免两处各写一遍后行为漂移。
static eui::Signal<bool> g_ovOpen{false};
static eui::Signal<int> g_ovTab{0};
static eui::Signal<std::string> g_ovQuery{""};
static eui::Signal<float> g_ovScroll{0.0f};       // 左侧条目列表
static eui::Signal<float> g_ovFieldScroll{0.0f};  // 右侧字段区
// 复位计数：每次在代码里把上面两个 Signal 置 0（切条目 / 切页签 / 切筛选 / 改搜索）时一并 +1，
// 让 virtualList 用新的 id。原因：EUI 的 scrollState 一旦初始化就只由运行时自己维护，
// 之后再给组件传 offset(0) 不会把已记住的偏移清掉——于是窗口仍按旧偏移平移，字段行整片
// 移出可视区（"有标题但内容消失，滚动才出现"就是它）。换 id → 运行时重建一份全新 scroll
// state（offset 取组件传入的 0），旧的那份下一帧自动释放。这是规避 EUI 内部机制最稳的做法。
static int g_ovListGen = 0;
static int g_ovFieldGen = 0;
static eui::Signal<float> g_pageScroll{0.0f};     // 页面滚动位置（自绘下拉要据此算绝对坐标）
static std::string g_ovStatus;

// 编辑器身份：0 = 自定义译文（默认）/ 1 = 构建器。
static int g_ovMode = 0;
// 构建器筛选：0 全部 / 1 新增 / 2 未译 / 3 已译 / 4 孤儿 / 5 待翻译 / 6 待校对
// （待翻译放孤儿右边，让「待翻译 / 待校对」两个标记档相邻、排在最右）
static eui::Signal<int> g_bldFilter{0};
static const char* const kBldFilterName[7] = {"全部", "新增", "未译", "已译", "孤儿", "待翻译", "待校对"};
static eui::Signal<std::string> g_bldVer{""};   // 对照表版本号（顶栏编辑框）
static pxc_base::Baseline g_bldBase;            // 构建器当前使用的英文骨架（导入新包后替换）
static bool g_bldBaseReady = false;
static std::vector<pxc_dbe::KeyRef> g_bldKeys;  // 骨架上全部可译键
static std::set<std::string> g_bldKeySet;       // 同上（查孤儿用）
static pxc_dbe::Coverage g_bldCov;
static pxc_dbe::Conflicts g_bldConflicts;       // 冲突候选：只在内存（落盘前材质化），下次导入重建
static std::set<std::string> g_bldAddedSet;     // g_db.addedKeys 的集合形态（筛选用，避免逐条 find）
// 最近导入的两个包（只用于界面显示"我导的是哪一份"）：维护者常备着好几份包，
// 不写出来很容易在错误的包上改半天。
static std::string g_bldEnPath;
static std::string g_bldZhPath;
static bool g_confirmOverwrite = false;         // 「生成并覆盖当前对照表」的二次确认框
// 通用二次确认框（清除标签等危险动作复用）：打开时记录标题/正文/确认回调。
static bool g_confirmOpen = false;
static std::string g_confirmTitle, g_confirmMsg;
static std::function<void()> g_confirmFn;
static void openConfirm(const std::string& title, const std::string& msg,
                       std::function<void()> fn) {
    g_confirmTitle = title;
    g_confirmMsg = msg;
    g_confirmFn = std::move(fn);
    g_confirmOpen = true;
    requestUpdate();
}
static bool g_dbDirty = false;                  // 表在内存里被改过但还没落盘

// ---- 草稿（过程文件）----
// 编辑 1.7 万条的表往往跨好几天，中途关软件/断电就全没了。所以每次改动后落一份草稿，
// 下次打开构建器时问一句"接着上次的改吗"。
// 节流 2 秒：整表序列化约 1.3 MB，连打字时不能每敲一个字就写盘。
static bool g_draftPending = false;
static bool g_draftPresent = false;
static ULONGLONG g_draftLast = 0;
static void writeDraftNow();

struct OvItem {
    std::string id;      // 节点 id / words 键 / ui 键 / notes 文件名
    std::string en;      // 英文原文
    std::string zh;      // 当前生效中文（覆盖 > 对照表）
    std::string zhOrig;  // 对照表里的原中文（不含覆盖）—— 用户改过之后仍要能用原名搜到
    std::string idL, enL, zhL, zhOrigL;  // 归一化副本，供搜索用（避免每次过滤重新分配）
    bool marked = false;                 // 用户模式 = 有覆盖；构建器 = 有译文
    bool pend = false;                   // 构建器：由汉化包预填、还没人工确认（待校对）
    bool pendTr = false;                 // 构建器：标了「待翻译」（决定导出范围）
    bool isNew = false;                  // 构建器：本次导入英文包后才出现的键（meta.added_keys）
    bool orphan = false;                 // 构建器：表里有、新骨架里没有（保留且仍参与生成）
};

struct OvPageData {
    std::vector<OvItem> items;
    std::vector<int> view;   // 过滤后的下标
    std::string viewFor;     // 过滤所用的查询串
    int sel = -1;
    bool built = false;
};
static OvPageData g_ovData[kOvPages];

struct OvRow {
    std::string key;    // 对照表键
    std::string label;  // 右侧显示的分类名
    std::string en;     // 英文原文
    std::string defZh;  // 对照表里的默认译文（空 = 无译文）；保存时据此判断"是否等于默认"
    bool multi = false; // 多行（notes 整篇文档）
};
static std::vector<OvRow> g_ovRows;        // 右侧字段行
static std::vector<std::unique_ptr<eui::Signal<std::string>>> g_ovEdit;  // 每行一个编辑框
static std::string g_ovEditFor;            // g_ovEdit 当前属于哪个条目
// g_ovRows / g_ovEdit 属于**哪一条**（下标，不是 pd.sel）。
//
// 保存时必须用它定位"这一批编辑框是哪条目的"，不能用 pd.sel：ovSelect / ovSwitchTab 都是
// 先把 pd.sel 指到**新**条目、再调 ovBuildRows → 里面的 ovSaveAll 会把旧条目的编辑结果
// 汇总到新条目身上（zh / 已改 / 待翻译 / 待校对 全被写成旧条目的值）。
// 现象：给 A 打「待翻译」后点 B、再点回 A，A 的「待」标记消失，B 却顶着一个
// 打不开的「待」（右侧开关读的是表本身，当然是灭的）。
static int g_ovRowsOwner = -1;

// 页签上的数量：左侧四个页签 = 当前筛选（+搜索）下还剩几条；右侧 = 当前页签下有多少可译字段。
// 靠 ovViewTag() 缓存（查询串或筛选变了才重算），另配 g_ovDataVer：表/骨架一改动，
// 数量口径就变了（比如刚勾掉一批「待翻译」），只认 view tag 会一直显示旧数字。
// 浮窗没开时不去建分页 —— 为了显示几个数字把四个分页全建一遍不值当。
static int g_ovCounts[kOvPages] = {-1, -1, -1, -1};
// 七个筛选档各自的**字段数**（不是条目数），顺序与 kBldFilterName 一致：
// 0 全部 / 1 新增 / 2 未译 / 3 已译 / 4 孤儿 / 5 待翻译 / 6 待校对
// 左页签给的是"多少条"，chip 给的是"多少处要填" —— 同一个筛选下两个数往往差一个量级。
static int g_ovFltField[7] = {0, 0, 0, 0, 0, 0, 0};
static std::string g_ovCountsFor;
static int g_ovDataVer = 0;

// 搜索归一化与命中规则在 launcher/search_fold.h（纯函数，可被 headless 回归覆盖）。
static std::string foldSearch(const std::string& s) { return pxc_search::fold(s); }

static int ovCurPage() {
    int p = g_ovTab.get();
    return (p < 0 || p >= kOvPages) ? 0 : p;
}
static OvPageData& ovCur() { return g_ovData[ovCurPage()]; }

// 取某个键的中文：自定义译文层优先，其次对照表（空 = 无译文）。
static std::string dbZh(const std::string& key) {
    if (!g_dbLoaded) return {};
    const pxc_tdb::Entry* e = pxc_tdb::find(g_db, key);
    return (e && !e->zh.empty()) ? e->zh : std::string();
}
static std::string ovEffZh(const std::string& key) {
    if (g_ovMode == 1) return dbZh(key);  // 构建器没有自定义译文层：编辑的就是表本身
    auto it = g_cfg.locale.overrides.find(key);
    if (it != g_cfg.locale.overrides.end() && !it->second.empty()) return it->second;
    return dbZh(key);
}

// 编辑器当前用的骨架：用户模式 = 运行期解析出的骨架；构建器模式 = 构建器自己选定的那份
// （导入新英文包后列表要以新骨架为准，但运行期生成仍用原来的骨架，两者互不影响）。
static const pxc_base::Baseline& ovBase() {
    if (g_ovMode == 1 && g_bldBaseReady) return g_bldBase;
    return g_base;
}

static void ovPushItem(std::vector<OvItem>& out, const std::string& id, const std::string& en,
                       const std::string& key);  // 定义在后
static void ovAggregateEntryFlags(int page);      // 定义在后
static void ovRefreshItemFlags();                 // 定义在后
static void ovRefreshFilterFieldCounts();         // 定义在后（七个筛选档的字段数）

// 构建器：把「表里有译文、当前骨架里没有」的孤儿也列进列表。
// 保留而不删除的理由：用户机器上通常同时装着多个 PXC 版本，新版删掉的节点老版本还在；
// 生成是按目标版本骨架逐项遍历的，留着自然只在该用时用到。标记只为让人知道"新骨架里没了"。
static void ovAppendOrphans(OvPageData& pd, int page) {
    if (g_db.entries.empty()) return;
    std::set<std::string> have;
    for (const auto& it : pd.items) have.insert(it.id);
    // 同一个节点可能留下多个孤儿键，列表按节点/条目只显示一行：
    // 优先用节点名键（或词条键）当代表，其余键仍可在右侧编辑。
    struct Pick {
        const pxc_tdb::Entry* e = nullptr;
        const std::string* key = nullptr;
        int rank = 9;
    };
    std::map<std::string, Pick> pick;
    for (const auto& kv : g_db.entries) {
        if (kv.second.zh.empty()) continue;
        if (g_bldKeySet.count(kv.first)) continue;  // 骨架里有 -> 不是孤儿
        if ((int)pxc_dbe::catOf(kv.first) != page) continue;
        const std::string id = pxc_dbe::idOf(kv.first);
        if (id.empty() || have.count(id)) continue;
        const int rank = (kv.first == pxc_tdb::kNodeName(id)) ? 0
                         : (kv.first.rfind("nodes.", 0) == 0 ? 2 : 1);
        Pick& p = pick[id];
        if (!p.e || rank < p.rank) {
            p.e = &kv.second;
            p.key = &kv.first;
            p.rank = rank;
        }
    }
    for (const auto& kv : pick) {
        ovPushItem(pd.items, kv.first, kv.second.e ? kv.second.e->en : std::string(),
                   kv.second.key ? *kv.second.key : std::string());
        pd.items.back().orphan = true;
    }
}

// 骨架只解析一次；目标版本 / 用户目录 / 登记表变了要手动失效（invalidateBase）。
// 取哪一份由「目标版本」决定，不再是"登记表第一条"（docs/08 §8.1）。
static bool ensureBase() {
    if (g_baseReady) return true;
    refreshSkelOpts();
    pxc_base::Request r;
    r.registry = g_cfg.registry;
    r.userDir = pxc_util::pathFromUtf8(g_cfg.gen.user_dir);
    r.cacheRoot = skeletonCacheRoot();
    r.targetVersion = effectiveTargetVersion();
    g_base = pxc_base::resolve(r);
    g_baseReady = g_base.hasNodes || g_base.hasWords;
    pxc_log::write(g_baseReady ? "INFO" : "WARN", "Base",
                   "ensureBase: " + (g_baseReady ? ("骨架 " + g_base.version + "（" + g_base.origin +
                                                    "）节点 " + std::to_string(g_base.nodeCount))
                                                 : g_base.note));
    return g_baseReady;
}

// 骨架缓存失效。凡是改变"该用哪一份骨架"的动作都要调它：换目标版本、换用户目录、登记表变动。
static void invalidateBase() {
    g_baseReady = false;
    g_base = pxc_base::Baseline{};
}

// 一条 item = 一个可覆盖条目；key 是它在对照表里的键。
static void ovPushItem(std::vector<OvItem>& out, const std::string& id, const std::string& en,
                       const std::string& key) {
    OvItem it;
    it.id = id;
    it.en = en;
    it.zhOrig = dbZh(key);          // 对照表原值：改过之后仍要能用它搜到
    if (g_ovMode == 1) {
        it.zh = it.zhOrig;
        it.marked = !it.zh.empty();  // 构建器：mark = 有译文
        it.pend = pxc_dbe::isPendingReview(g_db, key);
        it.pendTr = pxc_dbe::isPendingTranslate(g_db, key);
        it.isNew = g_bldAddedSet.count(key) > 0;
    } else {
        auto ov = g_cfg.locale.overrides.find(key);
        if (ov != g_cfg.locale.overrides.end() && !ov->second.empty()) {
            it.marked = true;
            it.zh = ov->second;
        } else {
            it.zh = it.zhOrig;
        }
    }
    it.idL = foldSearch(id);
    it.enL = foldSearch(en);
    it.zhL = foldSearch(it.zh);
    it.zhOrigL = foldSearch(it.zhOrig);
    out.push_back(std::move(it));
}

static void ovBuildPage(int page) {
    const ULONGLONG t0 = GetTickCount64();
    OvPageData& pd = g_ovData[page];
    pd.items.clear();
    const pxc_base::Baseline& src = ovBase();
    if (page == (int)OvPage::Nodes) {
        // 用户模式：节点 -> 是否已有任一字段被覆盖（从覆盖键前缀解析，O(覆盖数)，不扫 942 节点）。
        // 构建器模式：marked 由 ovPushItem 按"有没有译文"给出，这里不能盖掉。
        std::set<std::string> marked;
        if (g_ovMode == 0) {
            for (const auto& kv : g_cfg.locale.overrides) {
                if (kv.second.empty() || kv.first.rfind("nodes.", 0) != 0) continue;
                size_t dot = kv.first.find('.', 6);
                if (dot != std::string::npos) marked.insert(kv.first.substr(6, dot - 6));
            }
        }
        if (src.hasNodes) {
            for (const auto& kv : src.nodes.obj) {
                if (!kv.second.isObj()) continue;
                const auto* n = kv.second.find("name");
                if (!n || !n->isStr() || n->str.empty()) continue;
                ovPushItem(pd.items, kv.first, n->str, pxc_tdb::kNodeName(kv.first));
                if (g_ovMode == 0) pd.items.back().marked = marked.count(kv.first) > 0;
            }
        }
    } else if (page == (int)OvPage::Words) {
        if (src.hasWords) {
            for (const auto& kv : src.words.obj) {
                if (!kv.second.isStr() || kv.second.str.empty()) continue;
                ovPushItem(pd.items, kv.first, kv.second.str, pxc_tdb::kWord(kv.first));
            }
        } else {
            for (const auto& kv : g_db.entries)
                if (kv.first.rfind("words.", 0) == 0)
                    ovPushItem(pd.items, kv.first.substr(6), kv.second.en, kv.first);
        }
    } else if (page == (int)OvPage::Ui) {
        std::set<std::string> seen;
        if (src.hasUi) {
            for (const auto& kv : src.ui.obj) {
                if (!kv.second.isStr() || kv.second.str.empty()) continue;
                ovPushItem(pd.items, kv.first, kv.second.str, pxc_tdb::kUi(kv.first));
                seen.insert(kv.first);
            }
        }
        for (const auto& kv : g_db.entries) {
            if (kv.first.rfind("ui.", 0) != 0) continue;
            std::string k = kv.first.substr(3);
            if (seen.count(k)) continue;
            ovPushItem(pd.items, k, kv.second.en, kv.first);
        }
    } else {
        std::error_code ec;
        if (!src.dir.empty()) {
            for (const auto& it : std::filesystem::directory_iterator(src.dir / "notes", ec)) {
                if (!it.is_regular_file(ec)) continue;
                std::string name = pxc_util::toUtf8(it.path().filename().wstring().c_str());
                ovPushItem(pd.items, name, name, pxc_tdb::kNotes(name));
            }
        }
    }
    if (g_ovMode == 1) ovAppendOrphans(pd, page);
    pd.built = true;
    pd.view.clear();
    pd.viewFor = "";
    // 左列表的「待翻译 / 待校对」标记由字段级标记聚合而来（见 ovAggregateEntryFlags）。
    if (g_ovMode == 1) ovAggregateEntryFlags(page);
    ++g_ovDataVer;   // 这一页的条目集合变了，页签上的数量得重算
    // 一条就够：切页是用户操作，耗时是"卡不卡"的唯一客观依据。
    pxc_log::write("INFO", "Override",
                   "构建分页「" + std::string(kOvPageName[page]) + "」：" +
                       std::to_string((int)pd.items.size()) + " 项，" +
                       std::to_string((int)(GetTickCount64() - t0)) + " ms");
}

// 过滤结果对什么失效：查询串 + 筛选值（两者任一变了都要重算）。\x01 不可能出现在搜索框里。
static std::string ovViewTag() {
    return g_ovQuery.get() + "\x01" + std::to_string((g_ovMode == 1) ? g_bldFilter.get() : 0);
}

// 过滤：四个可搜字段（ID / 英文 / 现用译文 / 对照表原名）交给 pxc_search::hit。
// 四个字段都预存了归一化副本，所以过滤时零分配 —— 942 条 + 键键输入也不抖。
// 构建器额外按状态筛选（全部 / 未译 / 已译 / 孤儿 / 待校对）。
static bool ovItemMatches(const OvItem& it, int flt, const std::string& q);

// 计数缓存的失效判据：查询串 + 筛选 + 数据版本 + **当前页签**（chip 的字段数按页签算）。
static std::string ovCountsTag() {
    return ovViewTag() + "#" + std::to_string(g_ovDataVer) + "#" + std::to_string(ovCurPage());
}

static void ovRefreshView() {
    OvPageData& pd = ovCur();
    const std::string raw = g_ovQuery.get();
    const std::string q = pxc_search::trim(foldSearch(raw));
    const int flt = (g_ovMode == 1) ? g_bldFilter.get() : 0;
    pd.view.clear();
    pd.view.reserve(pd.items.size());
    for (int i = 0; i < (int)pd.items.size(); ++i) {
        if (ovItemMatches(pd.items[(size_t)i], flt, q)) pd.view.push_back(i);
    }
    pd.viewFor = ovViewTag();
}

static void ovRefreshIfNeeded() {
    if (ovCur().viewFor != ovViewTag()) ovRefreshView();
}

// 一条条目在当前筛选 + 搜索下是否留下。**列表和页签计数必须共用它** ——
// 各写一份的话，页签上写着 12 条、点进去只列出 10 条，比不显示数字更让人怀疑。
static bool ovItemMatches(const OvItem& it, int flt, const std::string& q) {
    switch (flt) {
        case 1: if (!it.isNew) return false; break;                     // 新增
        case 2: if (!it.zh.empty()) return false; break;                // 未译
        case 3: if (it.zh.empty()) return false; break;                 // 已译
        case 4: if (!it.orphan) return false; break;                    // 孤儿
        case 5: if (!it.pendTr) return false; break;                    // 待翻译
        case 6: if (!it.pend) return false; break;                      // 待校对
        default: break;
    }
    if (q.empty()) return true;
    const std::string* fs[4] = {&it.idL, &it.enL, &it.zhL, &it.zhOrigL};
    return pxc_search::hit(fs, 4, q);
}

// 页签数量（缓存：只在查询串 / 筛选 / 数据版本变化时重算）。
// 四个分页都要建出来才数得清 —— 但只建一次，之后 pd.built 一直为真。
static void ovRefreshCounts() {
    if (!g_ovOpen.get()) return;
    const std::string q = pxc_search::trim(foldSearch(g_ovQuery.get()));
    const int flt = (g_ovMode == 1) ? g_bldFilter.get() : 0;
    for (int p = 0; p < kOvPages; ++p) {
        OvPageData& d = g_ovData[p];
        if (!d.built) ovBuildPage(p);
        int n = 0;
        for (const auto& it : d.items)
            if (ovItemMatches(it, flt, q)) ++n;
        g_ovCounts[p] = n;
    }
    ovRefreshFilterFieldCounts();
    // 建分页的过程本身会 +ver，所以 tag 必须在建完之后取，否则下一帧又白算一遍。
    // 当前页也要进 tag：chip 上的字段数是**本页签**的口径，切页后必须重算。
    g_ovCountsFor = ovCountsTag();
}

static void ovCountsIfNeeded() {
    if (!g_ovOpen.get()) return;
    if (g_ovCountsFor == ovCountsTag()) return;
    ovRefreshCounts();
}

// 七个筛选档各自「当前页签下有多少个字段」。
// 与条目数（左页签）是两回事：一个节点里十几个端口，只看条目数根本估不出工作量。
// 全集 = 骨架键 ∪ 表里的键（孤儿也在内，和左列表的口径一致），按分类过滤、去重。
static void ovRefreshFilterFieldCounts() {
    if (g_ovMode != 1) {   // 自定义译文层没有筛选 chip
        for (int i = 0; i < 7; ++i) g_ovFltField[i] = 0;
        return;
    }
    const int page = ovCurPage();
    int n[7] = {0, 0, 0, 0, 0, 0, 0};
    const std::set<std::string> pt(g_db.pendingTranslate.begin(), g_db.pendingTranslate.end());
    const std::set<std::string> pr(g_db.pendingReview.begin(), g_db.pendingReview.end());
    std::set<std::string> seen;
    auto acc = [&](const std::string& key, bool inSkeleton) {
        if ((int)pxc_dbe::catOf(key) != page) return;
        if (!seen.insert(key).second) return;   // 骨架与表都有的键只算一次
        const pxc_tdb::Entry* e = pxc_tdb::find(g_db, key);
        const bool hasZh = e && !e->zh.empty();
        ++n[0];
        if (g_bldAddedSet.count(key)) ++n[1];
        if (hasZh) ++n[3];
        else ++n[2];
        if (!inSkeleton) ++n[4];                // 表里有、骨架里没有 = 孤儿
        if (pt.count(key)) ++n[5];              // 待翻译
        if (pr.count(key)) ++n[6];              // 待校对
    };
    for (const auto& k : g_bldKeys) acc(k.key, true);
    for (const auto& kv : g_db.entries) acc(kv.first, g_bldKeySet.count(kv.first) > 0);
    for (int i = 0; i < 7; ++i) g_ovFltField[i] = n[i];   // n[0]（全部）就是本页签的可译字段总数
}

// chip 宽度按「标签 + 数字」的实际像素估：汉字 11px、ASCII 6.5px，再加左右内边距。
// 写死档位（46/58）在数字位数变化时要么挤爆、要么留一大条空白。
static float ovChipWidth(const std::string& txt) {
    float w = 0.0f;
    for (size_t i = 0; i < txt.size();) {
        const unsigned char c = (unsigned char)txt[i];
        if (c >= 0xF0) { w += 11.0f; i += 4; }
        else if (c >= 0xE0) { w += 11.0f; i += 3; }
        else if (c >= 0xC0) { w += 8.0f;  i += 2; }
        else { w += 6.5f; i += 1; }
    }
    return w + 16.0f;
}

// 标记"表被改过"并顺手落草稿（节流）。所有改动最终都要经过这里，漏一处就会丢那一处。
static void markDirty() {
    g_dbDirty = true;
    if (g_ovMode != 1) return;
    g_draftPending = true;
    if (GetTickCount64() - g_draftLast < 2000) return;  // 到期由帧回调补写
    writeDraftNow();
}

// 把当前编辑状态写进草稿。失败不打断编辑（草稿只是兜底，不是主存储）。
//
// **不能静默覆盖上一轮的草稿**：草稿不是"本次会话的镜像"，它是上一次没干完的活。
// 打开构建器时并不会自动应用草稿（那是维护者的显式选择），于是"打开 → 随手改一条 → 关掉"
// 会在第一次落草稿时把上一轮的劳动整份盖掉（实测踩到：单击一个开关就把旧草稿换了）。
// 所以本次会话第一次写草稿、而盘上那份**既没恢复也没丢弃**时，先给旧的留一代备份。
static bool g_draftGenKept = false;
static void writeDraftNow() {
    if (!g_draftPending) return;
    if (!g_draftGenKept && g_draftPresent) {
        g_draftGenKept = true;
        std::error_code ec;
        const std::filesystem::path src = draftPath();
        const std::filesystem::path prev = src.parent_path() / "db_draft.prev.json";
        if (std::filesystem::exists(src, ec))
            std::filesystem::copy_file(src, prev, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec)
            g_ovStatus += "（上一轮草稿未恢复，已另存为 data/.draft/db_draft.prev.json）";
        else
            pxc_log::write("WARN", "Draft", "上一轮草稿备份失败");
    }
    if (!pxc_dbe::saveDraft(draftPath(), g_db, g_bldConflicts)) {
        pxc_log::write("WARN", "Draft", "草稿写入失败");
        return;
    }
    g_draftLast = GetTickCount64();
    g_draftPending = false;
    g_draftPresent = true;
}

// 把当前右侧编辑框的值写回。用户模式写 launcher.json 的 overrides；构建器模式直接改内存里的
// 对照表（落盘由「生成并覆盖当前对照表」按钮负责，改一条就写 1.3 MB 文件没有意义）。
// 只更新被编辑的那一条（曾因重建整页 + 重算过滤而卡顿）。
static void ovSaveAll() {
    if (g_ovEdit.size() != g_ovRows.size() || g_ovRows.empty()) return;
    OvPageData& pd = ovCur();
    // 汇总对象由 g_ovRowsOwner 决定：切条目时 pd.sel 已经先跳到新条目了，
    // 用 pd.sel 会把旧条目的编辑结果记到新条目头上（见 g_ovRowsOwner 的说明）。
    if (g_ovRowsOwner < 0 || g_ovRowsOwner >= (int)pd.items.size()) return;
    const int owner = g_ovRowsOwner;

    if (g_ovMode == 1) {
        bool dirty = false;
        for (size_t i = 0; i < g_ovRows.size(); ++i) {
            const std::string v = g_ovEdit[i] ? g_ovEdit[i]->get() : std::string();
            // 人工编辑即视为校对完成：顺手摘掉"待校对"标记。
            if (pxc_dbe::setTranslation(g_db, g_ovRows[i].key, g_ovRows[i].en, v, true)) dirty = true;
        }
        if (!dirty) return;
        g_dbDirty = true;
        g_draftPending = true;   // 草稿在下一帧/到期时落盘（节流见 markDirty）
        OvItem& item = pd.items[(size_t)owner];
        // 列表上的中文/标记按"这个条目的代表键"刷新（左列表显示的就是它）。
        // 两个状态标记是**聚合**的：本条目下任一个字段还打着标，就继续显示。
        std::string repZh;
        bool repPend = false, repPendTr = false;
        for (const auto& r : g_ovRows) {
            const pxc_tdb::Entry* e = pxc_tdb::find(g_db, r.key);
            if (e && !e->zh.empty() && repZh.empty()) repZh = e->zh;
            if (pxc_dbe::isPendingReview(g_db, r.key)) repPend = true;
            if (pxc_dbe::isPendingTranslate(g_db, r.key)) repPendTr = true;
        }
        item.zh = repZh;
        item.zhL = foldSearch(item.zh);
        item.marked = !repZh.empty();
        item.pend = repPend;
        item.pendTr = repPendTr;
        pxc_dbe::Coverage cov = pxc_dbe::coverage(g_db, g_bldKeys);
        g_bldCov = cov;
        ++g_ovDataVer;
        return;
    }

    bool changed = false;
    for (size_t i = 0; i < g_ovRows.size(); ++i) {
        const std::string v = g_ovEdit[i] ? g_ovEdit[i]->get() : std::string();
        const std::string& key = g_ovRows[i].key;
        auto it = g_cfg.locale.overrides.find(key);
        if (v.empty() || v == g_ovRows[i].defZh) {
            if (it != g_cfg.locale.overrides.end()) {
                g_cfg.locale.overrides.erase(it);
                changed = true;
            }
        } else if (it == g_cfg.locale.overrides.end() || it->second != v) {
            g_cfg.locale.overrides[key] = v;
            changed = true;
        }
    }
    if (!changed) return;
    saveConfig();
    ++g_ovDataVer;
    // 增量更新这一条：中文取自节点名/词条本身的键，标记看本条目还有没有覆盖。
    OvItem& item = pd.items[(size_t)owner];
    const std::string itemKey = ovCurPage() == (int)OvPage::Nodes ? pxc_tdb::kNodeName(item.id)
                               : ovCurPage() == (int)OvPage::Words ? pxc_tdb::kWord(item.id)
                               : ovCurPage() == (int)OvPage::Ui    ? pxc_tdb::kUi(item.id)
                                                                  : pxc_tdb::kNotes(item.id);
    item.zh = ovEffZh(itemKey);
    item.zhL = foldSearch(item.zh);
    item.marked = false;
    for (const auto& r : g_ovRows) {
        auto oi = g_cfg.locale.overrides.find(r.key);
        if (oi != g_cfg.locale.overrides.end() && !oi->second.empty()) {
            item.marked = true;
            break;
        }
    }
}

static void ovBuildRows(int idx) {
    ovSaveAll();  // 先把上一条目的编辑落盘（按 g_ovRowsOwner 归到它自己的条目）
    g_ovRowsOwner = -1;
    g_ovRows.clear();
    g_ovEdit.clear();
    g_ovEditFor.clear();
    OvPageData& pd = ovCur();
    if (idx < 0 || idx >= (int)pd.items.size()) return;
    const OvItem& it = pd.items[(size_t)idx];

    auto add = [&](const std::string& key, std::string label, const std::string& en, bool multi = false) {
        OvRow r;
        r.key = key;
        r.label = std::move(label);
        r.en = en;
        r.defZh = dbZh(key);
        r.multi = multi;
        g_ovRows.push_back(std::move(r));
    };

    const pxc_base::Baseline& src = ovBase();
    const int page = ovCurPage();
    if (page == (int)OvPage::Nodes) {
        const std::string& id = it.id;
        const pxc_store::JsonValue* node = src.nodes.find(id);
        if (node && node->isObj()) {
            // 字段枚举与构建器统计用的是同一份定义（pxc_dbe::nodeFields），
            // 两处各写一遍会悄悄漂移（端口提示就漏过一次）。
            for (const auto& f : pxc_dbe::nodeFields(node, id)) add(f.key, f.label, f.en);
        } else if (g_ovMode == 1) {
            // 孤儿节点：新骨架里已经没有它了，字段只能从表里反推 —— 保留它正是为了老版本可用。
            for (const auto& kv : g_db.entries) {
                if (kv.second.zh.empty()) continue;
                if (kv.first.rfind("nodes." + id + ".", 0) != 0) continue;
                std::string tail = kv.first.substr(7 + id.size());
                add(kv.first, tail, kv.second.en);
            }
        }
    } else if (page == (int)OvPage::Words) {
        add(pxc_tdb::kWord(it.id), "界面词条", it.en);
    } else if (page == (int)OvPage::Ui) {
        add(pxc_tdb::kUi(it.id), "界面文本", it.en);
    } else {
        // 注释：整篇 markdown。默认值是骨架原文（保存时据此判断"改过没有"）。
        std::string def;
        if (!src.dir.empty()) {
            auto [txt, ok] = pxc_io::readText(src.dir / "notes" / pxc_util::pathFromUtf8(it.id));
            if (ok) def = txt;
        }
        add(pxc_tdb::kNotes(it.id), "整篇文档", it.id, true);
        if (!g_ovRows.empty()) g_ovRows.back().defZh = def;
    }

    for (const auto& r : g_ovRows) {
        auto s = std::make_unique<eui::Signal<std::string>>();
        s->set(ovEffZh(r.key));  // 覆盖值优先，没覆盖就看对照表
        g_ovEdit.push_back(std::move(s));
    }
    g_ovEditFor = it.id;
    g_ovRowsOwner = idx;
}

static void ovSelect(int idx) {
    OvPageData& pd = ovCur();
    if (idx < 0 || idx >= (int)pd.items.size()) return;
    if (idx == pd.sel && g_ovEditFor == pd.items[(size_t)idx].id) return;  // 已在编辑该条目，别打断输入
    pd.sel = idx;
    ovBuildRows(idx);
    g_ovFieldScroll.set(0.0f);   // 新条目从第一项看起
    ++g_ovFieldGen;
    g_ovStatus.clear();
    requestUpdate();
}

static void ovSwitchTab(int page) {
    if (page < 0 || page >= kOvPages) return;
    ovSaveAll();
    g_ovTab.set(page);
    OvPageData& pd = g_ovData[page];
    if (!pd.built) ovBuildPage(page);
    ovRefreshView();
    if (pd.sel < 0 || pd.sel >= (int)pd.items.size()) pd.sel = pd.view.empty() ? -1 : pd.view.front();
    ovBuildRows(pd.sel);
    g_ovScroll.set(0.0f);
    g_ovFieldScroll.set(0.0f);
    ++g_ovListGen;
    ++g_ovFieldGen;
    g_ovStatus.clear();
    requestUpdate();
}

// 清空当前条目的所有覆盖（恢复到对照表默认）
static void ovClearNode() {
    OvPageData& pd = ovCur();
    if (pd.sel < 0 || pd.sel >= (int)pd.items.size()) { requestUpdate(); return; }
    int n = 0;
    for (const auto& r : g_ovRows) {
        auto it = g_cfg.locale.overrides.find(r.key);
        if (it != g_cfg.locale.overrides.end()) {
            g_cfg.locale.overrides.erase(it);
            ++n;
        }
    }
    if (n > 0) saveConfig();
    // 先丢掉旧编辑框：否则 ovBuildRows 里的 ovSaveAll 会把刚清掉的值又写回来
    g_ovRowsOwner = -1;
    g_ovRows.clear();
    g_ovEdit.clear();
    g_ovEditFor.clear();
    ovBuildRows(pd.sel);  // 重建编辑框（值回到对照表默认）
    OvItem& item = pd.items[(size_t)pd.sel];
    item.marked = false;
    item.zh = dbZh(ovCurPage() == (int)OvPage::Nodes   ? pxc_tdb::kNodeName(item.id)
                   : ovCurPage() == (int)OvPage::Words ? pxc_tdb::kWord(item.id)
                   : ovCurPage() == (int)OvPage::Ui    ? pxc_tdb::kUi(item.id)
                                                       : pxc_tdb::kNotes(item.id));
    item.zhL = foldSearch(item.zh);
    g_ovStatus = n > 0 ? ("已清除 " + std::to_string(n) + " 项覆盖。") : "本条目没有覆盖。";
    pxc_log::write("INFO", "Override",
                   "清除覆盖 " + item.id + "（" + std::to_string(n) + " 项）");
    requestUpdate();
}

// 清除当前条目的某种标记（which: 0 待翻译 / 1 待校对）。只动属于该条目、且属于当前页分类的键。
// 标记是字段级的，一个条目下可以有若干个键打了标，整条一起清掉。
static void ovClearEntryPending(int which) {
    OvPageData& pd = ovCur();
    if (pd.sel < 0 || pd.sel >= (int)pd.items.size()) { requestUpdate(); return; }
    const std::string id = pd.items[(size_t)pd.sel].id;
    const int page = ovCurPage();
    auto& vec = (which == 0) ? g_db.pendingTranslate : g_db.pendingReview;
    int n = 0;
    for (size_t i = 0; i < vec.size(); ) {
        const std::string& k = vec[i];
        if ((int)pxc_dbe::catOf(k) == page && pxc_dbe::idOf(k) == id) {
            vec.erase(vec.begin() + (std::ptrdiff_t)i);
            ++n;
        } else {
            ++i;
        }
    }
    if (n == 0) { requestUpdate(); return; }
    markDirty();
    ovRefreshItemFlags();
    g_ovStatus = "已清除本条目 " + std::to_string(n) + " 处" +
                 std::string(which == 0 ? "待翻译" : "待校对") + "标记。";
    requestUpdate();
}

// 清除全部某种标记（which: 0 待翻译 / 1 待校对）。危险动作：会抹掉整张表里的标记，需二次确认。
static void ovClearAllPending(int which) {
    auto& vec = (which == 0) ? g_db.pendingTranslate : g_db.pendingReview;
    const int n = (int)vec.size();
    if (n == 0) { requestUpdate(); return; }
    vec.clear();
    markDirty();
    for (int p = 0; p < kOvPages; ++p) ovAggregateEntryFlags(p);
    g_bldCov = pxc_dbe::coverage(g_db, g_bldKeys);
    ++g_ovDataVer;
    g_ovStatus = "已清除全部 " + std::to_string(n) + " 处" +
                 std::string(which == 0 ? "待翻译" : "待校对") + "标记。";
    requestUpdate();
}

// ---------- 自定义译文层导入 / 导出（差异文件，不含对照表内容） ----------
// 只导出用户真正改过的键，所以文件很小，可以发给别人或带到另一台机器。
// 数据层在 override_io（与构建器「导出待翻译 / 导入已翻译」共用同一格式）。
// 提示写哪儿：这两个按钮现在在汉化页（不打开浮窗也能用），所以不能一律写 g_ovStatus。
static void ovSay(const std::string& msg) {
    // 浮窗开着就写它自己的状态行（那里有位置）；没开就说明是页面上那两个按钮干的，
    // 走 toast —— 页面已经没有常驻状态行可写了（docs/08 §8.6.3）。
    if (g_ovOpen.get()) g_ovStatus = msg;
    else showToast("完成", msg);
}

static std::string ovDefaultFileName() {
    std::string lang = g_langDir.get();
    if (lang.empty()) lang = "zh";
    std::string stamp = nowStamp();  // "YYYY-MM-DD HH:MM"
    for (char& c : stamp)
        if (c == ':' || c == ' ') c = '-';
    return "pxc-overrides_" + lang + "_" + stamp + ".json";
}

static void onOvExport() {
    ovSaveAll();
    if (g_cfg.locale.overrides.empty()) {
        ovSay("还没有任何覆盖项可导出。");
        requestUpdate();
        return;
    }
    std::filesystem::path out = pxc_nui::pickSave(
        L"导出自定义译文", pxc_util::pathFromUtf8(ovDefaultFileName()).wstring(),
        {{L"自定义译文文件", L"*.json"}}, L"json");
    if (out.empty()) { requestUpdate(); return; }
    if (out.extension().empty()) out += L".json";

    pxc_ovio::Bundle b;
    b.db_version = g_db.version;
    b.lang_dir = g_langDir.get();
    b.preset = g_cfg.gen.preset;
    b.slots[pxc_ovio::kSlotKeys[0]] = g_cfg.gen.ex_node_name;
    b.slots[pxc_ovio::kSlotKeys[1]] = g_cfg.gen.ex_node_tip;
    b.slots[pxc_ovio::kSlotKeys[2]] = g_cfg.gen.ex_in_name;
    b.slots[pxc_ovio::kSlotKeys[3]] = g_cfg.gen.ex_out_name;
    b.slots[pxc_ovio::kSlotKeys[4]] = g_cfg.gen.ex_dd;
    b.slots[pxc_ovio::kSlotKeys[5]] = g_cfg.gen.ex_text;
    b.overrides = g_cfg.locale.overrides;

    if (!pxc_ovio::save(out, b)) {
        ovSay("导出失败，无法写入该位置。");
        pxc_log::write("ERROR", "Override", "导出失败：" + pxc_util::toUtf8(out.wstring().c_str()));
        requestUpdate();
        return;
    }
    ovSay("已导出 " + std::to_string((int)g_cfg.locale.overrides.size()) + " 项覆盖：" +
          pxc_util::toUtf8(out.filename().wstring().c_str()));
    pxc_log::write("INFO", "Override",
                   "导出 " + std::to_string((int)g_cfg.locale.overrides.size()) + " 项 -> " +
                       pxc_util::toUtf8(out.wstring().c_str()));
    requestUpdate();
}

static void onOvImport() {
    ovSaveAll();
    std::filesystem::path in =
        pxc_nui::pickFile(L"导入自定义译文", {{L"自定义译文文件", L"*.json"}});
    if (in.empty()) { requestUpdate(); return; }

    pxc_ovio::Bundle b;
    std::string err;
    if (!pxc_ovio::load(in, b, err)) {
        ovSay("导入失败：" + err);
        pxc_log::write("WARN", "Override", "导入失败：" + err);
        requestUpdate();
        return;
    }
    const pxc_ovio::MergeStats ms = pxc_ovio::merge(b, g_cfg.locale.overrides);

    // 显示表达式与预设也算"偏好"：文件里有就一起套用（字体路径是绝对的，不导入）。
    struct SlotMap { const char* key; std::string* dst; eui::Signal<std::string>* sig; };
    SlotMap slots[6] = {{pxc_ovio::kSlotKeys[0], &g_cfg.gen.ex_node_name, &g_exNodeName},
                        {pxc_ovio::kSlotKeys[1], &g_cfg.gen.ex_node_tip, &g_exNodeTip},
                        {pxc_ovio::kSlotKeys[2], &g_cfg.gen.ex_in_name, &g_exInName},
                        {pxc_ovio::kSlotKeys[3], &g_cfg.gen.ex_out_name, &g_exOutName},
                        {pxc_ovio::kSlotKeys[4], &g_cfg.gen.ex_dd, &g_exDd},
                        {pxc_ovio::kSlotKeys[5], &g_cfg.gen.ex_text, &g_exText}};
    for (auto& s : slots) {
        auto it = b.slots.find(s.key);
        if (it == b.slots.end() || it->second.empty() || it->second == *s.dst) continue;
        *s.dst = it->second;
        s.sig->set(it->second);
    }
    if (b.preset >= 0 && b.preset <= 2) {
        g_cfg.gen.preset = b.preset;
        g_preset.set(b.preset);
    }
    saveConfig();

    // 自定义译文层全部失效重算（导入会改很多条）
    for (int i = 0; i < kOvPages; ++i) g_ovData[i] = OvPageData{};
    const int page = ovCurPage();
    ovBuildPage(page);
    ovRefreshView();
    OvPageData& pd = g_ovData[page];
    pd.sel = pd.view.empty() ? -1 : pd.view.front();
    ovBuildRows(pd.sel);

    ovSay("已导入：新增 " + std::to_string(ms.added) + " / 更新 " + std::to_string(ms.updated) +
                 " / 相同 " + std::to_string(ms.same) + "（本机现共 " +
                 std::to_string((int)g_cfg.locale.overrides.size()) + " 项）");
    pxc_log::write("INFO", "Override",
                   "导入 +" + std::to_string(ms.added) + " ~" + std::to_string(ms.updated) + " = " +
                       std::to_string((int)g_cfg.locale.overrides.size()));
    requestUpdate();
}

static void onOvOpen() {
    ensureDb();
    if (!g_dbLoaded) { showToast("对照表未加载", "无法编辑自定义译文层", true); requestUpdate(); return; }
    if (!ensureBase()) { showToast("找不到英文骨架", g_base.note, true); requestUpdate(); return; }
    g_ovMode = 0;
    g_ovQuery.set("");
    for (int i = 0; i < kOvPages; ++i) g_ovData[i] = OvPageData{};
    g_ovTab.set(0);
    ovBuildPage(0);
    ovRefreshView();
    g_ovData[0].sel = g_ovData[0].view.empty() ? -1 : g_ovData[0].view.front();
    ovBuildRows(g_ovData[0].sel);
    g_ovStatus.clear();
    g_ovOpen.set(true);
    pxc_log::write("INFO", "Override",
                   "打开自定义译文层编辑器，节点 " + std::to_string((int)g_ovData[0].items.size()) +
                       " 个，已有覆盖 " + std::to_string((int)g_cfg.locale.overrides.size()) + " 项");
    requestUpdate();
}
static void onOvClose() {
    ovSaveAll();
    if (g_ovMode == 1) writeDraftNow();  // 关窗口前把攒着的改动落进草稿
    g_ovRowsOwner = -1;
    g_ovRows.clear();
    g_ovEdit.clear();
    g_ovEditFor.clear();
    g_ovOpen.set(false);
    if (g_ovMode == 1) {
        // 构建器：编辑只落在内存里的表上，没点「保存对照表」就不算数 —— 必须说清楚。
        pxc_log::write("INFO", "Builder",
                       "关闭构建器" + std::string(g_dbDirty ? "（有未保存改动）" : "") + "，条目 " +
                           std::to_string((int)g_db.entries.size()));
        if (g_dbDirty)
            showToast("构建器已关闭",
                      "改动存进了草稿（data/.draft/db_draft.json），重新打开可恢复；"
                      "只有点「生成并覆盖当前对照表」才会真正写进表。");
        g_ovMode = 0;
        g_ovStatus.clear();
    } else {
        pxc_log::write("INFO", "Override",
                       "关闭自定义译文层编辑器，现存覆盖 " + std::to_string((int)g_cfg.locale.overrides.size()) + " 项");
    }
    requestUpdate();
}

// ---------- 构建器：动作（维护者） ----------
// 编辑对象就是运行期用的那份表（data/translation_db.json）—— 没有"表库"概念：
// 要保留旧版就「导出对照表到…」，要试别人的表就「从文件导入对照表…」。
// 构建器用的骨架 = 运行期解析到的那一份（docs/08 §8.1.4 之后不再有内置骨架）。
// 顶栏本来就把骨架来源写在上面，维护者随时能看出来是哪一版。
static bool bldEnsureBase() {
    if (g_bldBaseReady) return true;
    if (!g_baseReady && !ensureBase()) return false;
    g_bldBase = g_base;
    g_bldBaseReady = true;
    return true;
}

// 草稿：恢复 / 丢弃。打开构建器时会自动探一次，有草稿就在界面里给出这两个入口。
static void bldRebuild(const char* why);   // 定义在下面
static void onBldRestoreDraft() {
    pxc_dbe::Draft d;
    std::string err;
    if (!pxc_dbe::loadDraft(draftPath(), d, err)) {
        g_ovStatus = "恢复草稿失败：" + err;
        g_draftPresent = false;
        requestUpdate();
        return;
    }
    g_db = std::move(d.db);
    g_bldConflicts.clear();  // 候选取材于"本次导入的包"，草稿里没有，恢复后也不该凭空出现
    g_draftGenKept = true;   // 已经接着它改了，不需要再给"上一轮草稿"留备份
    markDirty();
    g_draftPending = false;
    g_bldVer.set(g_db.version);
    g_ovStatus = "已恢复草稿（" + std::to_string((int)g_db.entries.size()) +
                 " 条）—— 这只是接着编辑，点「生成并覆盖当前对照表」才真正写进表。";
    pxc_log::write("INFO", "Draft", "恢复草稿，条目 " + std::to_string((int)g_db.entries.size()));
    bldRebuild("恢复草稿");
}

static void onBldDropDraft() {
    if (!pxc_dbe::dropDraft(draftPath())) {
        g_ovStatus = "丢弃草稿失败（文件被占用？）。";
        requestUpdate();
        return;
    }
    g_draftPresent = false;
    g_draftPending = false;
    g_ovStatus = "已丢弃草稿。当前编辑内容保持不变，只是下次打开不再提示恢复。";
    pxc_log::write("INFO", "Draft", "丢弃草稿");
    requestUpdate();
}

// 结构变动后统一重建：键集合 -> 覆盖率 -> 清空并重建分页 -> 重算过滤 -> 选中第一条。
// 只在维护者的显式动作后调用（不是每帧路径）。
static void bldRebuild(const char* why) {
    g_bldKeys = pxc_dbe::enumerate(g_bldBase);
    g_bldKeySet.clear();
    for (const auto& k : g_bldKeys) g_bldKeySet.insert(k.key);
    g_bldAddedSet.clear();
    for (const auto& k : g_db.addedKeys) g_bldAddedSet.insert(k);
    g_bldCov = pxc_dbe::coverage(g_db, g_bldKeys);
    for (int i = 0; i < kOvPages; ++i) g_ovData[i] = OvPageData{};
    ovBuildPage(ovCurPage());
    ovRefreshView();
    OvPageData& pd = ovCur();
    pd.sel = pd.view.empty() ? -1 : pd.view.front();
    ovBuildRows(pd.sel);
    pxc_log::write("INFO", "Builder",
                   std::string(why) + "：可译键 " + std::to_string((int)g_bldKeys.size()) + " | 已译 " +
                       std::to_string(g_bldCov.translated) + " | 孤儿 " + std::to_string(g_bldCov.orphan));
    requestUpdate();
}

// ① 换骨架的唯一实现：**两个入口只差「骨架目录怎么来的」**（docs/01 §4.1.1）。
//    入口 A「导入新版英文包骨架」= 文件选择器给一份语言目录；
//    入口 B「选择新版 PXC 路径」= 给 exe，由它上溯安装目录再取 data/locale/en（退 sample locale）。
//    「旧键集合要在替换前取」这条顺序是本函数的全部要点：写反了就会把所有历史空缺
//    误报成「新增」（§4.2 末段的坑）。所以**不许**在调用方各自复制一份。
static void applyBldBaseline(const std::filesystem::path& lang, const char* srcLabel) {
    pxc_base::Candidate c;
    c.dir = lang;
    c.origin = "builder";
    pxc_base::Baseline nb;
    if (!pxc_base::load(c, nb)) {
        g_ovStatus = "该目录里没有可用的语言文件（nodes.json / words.json）。";
        requestUpdate();
        return;
    }
    g_bldEnPath = pxc_util::toUtf8(lang.wstring().c_str());
    // 旧骨架的键集合要在替换前取：用它算"这次新包多出来的键"（= 新增），
    // 而不是把所有未译都叫新增 —— 历史遗留的空缺和维护者判定"不翻"的都算未译。
    std::set<std::string> oldKeys;
    if (g_bldBaseReady)
        for (const auto& k : pxc_dbe::enumerate(g_bldBase)) oldKeys.insert(k.key);
    g_bldBase = nb;
    g_bldBaseReady = true;
    // 先把"新增"算出来再同步：同一个集合决定"哪些键该默认标待翻译"。
    // 老键没译文很可能是维护者取消过标记（判定不翻），重新导入同一份包不该让它复活。
    g_db.addedKeys.clear();
    if (!oldKeys.empty())
        for (const auto& k : pxc_dbe::enumerate(g_bldBase))
            if (!oldKeys.count(k.key)) g_db.addedKeys.push_back(k.key);
    const size_t ptBefore = g_db.pendingTranslate.size();
    const pxc_dbe::Diff d = pxc_dbe::syncSkeleton(
        g_db, g_bldBase, oldKeys.empty() ? nullptr : &g_db.addedKeys,
        oldKeys.empty() ? nullptr : &oldKeys);
    // 什么都没变就别标脏：维护者反复导同一份包时，不该每次都跳"有未保存改动"、也不该
    // 每次都重写一份草稿。判据是三个真实输出 —— 新增键、英文刷新、待翻译标记是否增长。
    const bool changed =
        !g_db.addedKeys.empty() || d.enRefreshed > 0 || g_db.pendingTranslate.size() != ptBefore;
    if (changed) markDirty();
    // 回执要把"新增"和"未译"分开说：这两个数以前混在一句里（"新增键 700（其中本次新出现 0）"），
    // 维护者看到 700 会以为表里凭空多了 700 条，其实那 700 条是历史空缺 / 判定不翻的，本次一条没动。
    const int fresh = (int)g_db.addedKeys.size();
    std::string msg = "英文包骨架已导入（" + std::to_string(g_bldBase.nodeCount) + " 节点，来源：" +
                      srcLabel + "）：本次新出现 " + std::to_string(fresh) +
                      " 键（进「新增」，自动标为待翻译）｜其余未译 " +
                      std::to_string(d.newKeys - fresh) + " 键（判过「不翻」的不会被重新标上）｜孤儿 " +
                      std::to_string(d.orphanKeys) + " 键";
    // 节点数只在真的变了才提：以前这里恒报"新节点 13"，而那 13 个是 FFT / FXAA 这种
    // 名字保留英文的专名节点 —— 不是新增，是"没译名"。
    if (!d.newNodes.empty() || !d.goneNodes.empty())
        msg += "｜新增节点 " + std::to_string((int)d.newNodes.size()) + " · 消失节点 " +
               std::to_string((int)d.goneNodes.size());
    if (d.enRefreshed > 0) msg += "｜英文措辞更新 " + std::to_string(d.enRefreshed) + " 条";
    if (!changed) msg += "。同一份骨架、同一份标记：本次没有任何改动。";
    if (d.orphanKeys > 0) msg += " 消失的节点标为孤儿：保留、仍参与生成（老版本还要用）。";
    g_ovStatus = msg;
    bldRebuild("导入英文包");
}

// 入口 A：直接给一份英文语言目录（官方 en 包 / 别人给的包 / 用户 Locale/en 都算）。
static void onBldPickBaseline() {
    // 先确保有"上一版骨架"可比：新节点/消失节点是**骨架 vs 骨架**的比较，
    // 拿表里的译名去判会把「名字保留英文的专名节点」每次都误报成新节点。
    bldEnsureBase();
    std::filesystem::path p = pickFolderPath(L"选择新版本的英文语言目录（含 nodes.json / words.json）");
    if (p.empty()) { requestUpdate(); return; }
    std::filesystem::path lang = pxc_tdb::locateLangDir(p, "en");
    if (lang.empty()) lang = p;
    applyBldBaseline(lang, "目录");
}

// 入口 B：只给"刚装上的新版 PXC 的 exe"，安装目录与 data/locale/en 由这里推出来。
// 为什么不复用入口 A 让人自己选目录：让他去翻 data/locale/en 是多余的一步，
// 而且同级还并排放着 sample locale / 旧版本目录，很容易翻错版本。
static void onBldPickBaselineFromExe() {
    bldEnsureBase();
    std::filesystem::path exe = pickExeFile();
    if (exe.empty()) { requestUpdate(); return; }
    const std::filesystem::path installDir = pxc_base::installDirOfExe(exe);
    // candidatesFor() 内部按 install-en → install-sample → Steam zip 的顺序给，且只认真有
    // nodes.json 的目录（Steam 版会顺手把它解到 skeleton/ 缓存里），与汉化页用的是同一套判据。
    const auto cands = pxc_base::candidatesFor(installDir, {}, skeletonCacheRoot());
    if (cands.empty()) {
        g_ovStatus = "这个 PXC 目录里没找到英文语言目录（data/locale/en 或 sample locale）。"
                     "请改用「导入新版英文包骨架」直接指向语言目录。";
        requestUpdate();
        return;
    }
    const std::string origin = cands.front().origin;
    applyBldBaseline(cands.front().dir,
                     origin == "install-en"      ? "PXC 安装目录 en"
                     : origin == "install-zip"   ? "PXC pack/locale.zip 的 en"
                     : origin == "install-zip-sample" ? "PXC pack/locale.zip 的 sample locale"
                                                      : "PXC 安装目录 sample locale");
}

// ③ 导入含汉化的包：只补空位，补上的进「待校对」；已有译文一条不动，
//    但同键不同文会记成"冲突候选"（右侧两个按钮裁决，见 §4.11）。
static void onBldImportPack() {
    if (!bldEnsureBase()) { g_ovStatus = "没有可用的英文骨架，无法对齐汉化包。"; requestUpdate(); return; }
    std::filesystem::path p = pickFolderPath(L"选择含汉化的语言目录（可含 Locale/zh 子目录）");
    if (p.empty()) { requestUpdate(); return; }
    std::filesystem::path lang = pxc_tdb::locateLangDir(p, "zh");
    if (lang.empty()) lang = p;
    g_bldZhPath = pxc_util::toUtf8(lang.wstring().c_str());
    g_bldConflicts.clear();   // 候选是"这次整合的中间状态"：每次导入整体重建，不累积
    const pxc_dbe::PrefillStats ps = pxc_dbe::importPack(g_db, g_bldBase.dir, lang, g_bldConflicts);
    if (ps.filled > 0 || ps.conflicts > 0) markDirty();
    char buf[512];
    sprintf_s(buf,
              "汉化包预填 %d 条（已进「待校对」），冲突 %d 条（表里原有译文没动，"
              "在右侧条目上选用哪一段）；已有译文一条没被覆盖。",
              ps.filled, ps.conflicts);
    g_ovStatus = (ps.filled > 0 || ps.conflicts > 0)
                     ? std::string(buf)
                     : "这个包里没有能对上当前骨架的内容（可能版本差太远）。";
    bldRebuild("导入汉化包");
}

// §4.9 切表：先严校验（半个表比没有表更危险），再把旧表备份到 userdata/.db_backup/。
static void onBldImportDb() {
    std::filesystem::path in =
        pxc_nui::pickFile(L"从文件导入对照表", {{L"对照表", L"*.json"}});
    if (in.empty()) { requestUpdate(); return; }
    std::string bak, err;
    if (!pxc_dbe::replaceDbFile(dbPath(), in, bak, err)) {
        g_ovStatus = "导入失败：" + err;
        pxc_log::write("WARN", "Builder", "导入对照表失败：" + err);
        requestUpdate();
        return;
    }
    auto [db, ok] = pxc_tdb::load(dbPath());
    if (!ok) {
        g_ovStatus = "导入后读回失败（文件可能已被占用）。";
        requestUpdate();
        return;
    }
    g_db = db;
    g_dbLoaded = true;
    g_dbDirty = false;
    g_bldVer.set(g_db.version);
    g_ovStatus = "已切换对照表：版本 " + (g_db.version.empty() ? std::string("(未标版本)") : g_db.version) +
                 "，条目 " + std::to_string((int)g_db.entries.size()) +
                 (bak.empty() ? "。原表不存在，未备份。" : "。旧表已备份到 userdata/.db_backup/。") +
                 " 若这份表对应别的 PXC 版本，再「导入英文包」把骨架换成同一版本。";
    bldRebuild("导入对照表");
}

static void onBldExportDb() {
    std::string name = "translation_db";
    if (!g_db.version.empty()) {
        std::string v = g_db.version;
        for (char& c : v)
            if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>') c = '_';
        name += "_" + v;
    }
    name += ".json";
    std::filesystem::path out = pxc_nui::pickSave(
        L"导出对照表", pxc_util::pathFromUtf8(name).wstring(), {{L"对照表", L"*.json"}}, L"json");
    if (out.empty()) { requestUpdate(); return; }
    if (out.extension().empty()) out += L".json";
    std::string err;
    if (!pxc_dbe::exportDbFile(dbPath(), out, err)) {
        g_ovStatus = "导出失败：" + err;
        requestUpdate();
        return;
    }
    g_ovStatus = "已导出对照表：" + pxc_util::toUtf8(out.filename().wstring().c_str());
    requestUpdate();
}

// ④ 补译的两端：导出「待翻译」队列给 AI，翻完导回。格式与用户偏好文件同构。
// 只导标记过的键 —— 语言文件里大量条目本就不该翻（单字母轴、Phong/SDF 这类专名、
// 纯 %s 占位），全导出去等于把几千条噪音丢给翻译方。
static void onBldExportQueue() {
    ovSaveAll();
    std::string name = "pxc-translate-queue";
    if (!g_db.version.empty()) name += "_" + g_db.version;
    name += ".json";
    std::filesystem::path out = pxc_nui::pickSave(
        L"导出待翻译条目", pxc_util::pathFromUtf8(name).wstring(), {{L"待翻译条目", L"*.json"}}, L"json");
    if (out.empty()) { requestUpdate(); return; }
    if (out.extension().empty()) out += L".json";
    std::string err;
    if (!pxc_dbe::exportTranslateQueue(out, g_db, g_bldKeys, err)) {
        g_ovStatus = "导出待翻译失败：" + err;
        requestUpdate();
        return;
    }
    g_ovStatus = "已导出 " + std::to_string((int)g_db.pendingTranslate.size()) + " 条待翻译：" +
                 pxc_util::toUtf8(out.filename().wstring().c_str()) +
                 "（值是现有译文或英文原文，翻译方看得到源文）";
    requestUpdate();
}

static void onBldImportTranslations() {
    ovSaveAll();
    std::filesystem::path in =
        pxc_nui::pickFile(L"导入译文", {{L"译文文件", L"*.json"}});
    if (in.empty()) { requestUpdate(); return; }
    pxc_ovio::Bundle b;
    std::string err;
    if (!pxc_ovio::load(in, b, err)) {
        g_ovStatus = "导入失败：" + err;
        requestUpdate();
        return;
    }
    // 导入译文是**维护者主动的补译动作**：允许按 key 覆盖草稿（这和"导入汉化包"不同，
    // 后者是顺手拿别人的包，绝不能冲掉人工成果）。覆盖后：待翻译 → 待校对。
    const pxc_dbe::ImportStats st = pxc_dbe::importTranslations(g_db, b, g_bldKeys, true);
    if (st.added + st.updated + st.kept > 0) markDirty();
    char buf[512];
    sprintf_s(buf,
              "导入译文：新增 %d / 覆盖 %d / 相同 %d / 骨架里没有而忽略 %d。"
              "导入的条目已从「待翻译」转入「待校对」——机翻产物得人过一眼才算完。",
              st.added, st.updated, st.kept, st.ignored);
    g_ovStatus = buf;
    bldRebuild("导入译文");
}

// ⑤ 发布：把编辑结果写回运行期那张表（危险动作，界面侧有二次确认）。
static void onBldSave() {
    ovSaveAll();
    const std::string ver = g_bldVer.get();
    if (!ver.empty()) g_db.version = ver;
    // 候选先材质化：维护者选了"用新包文本"的，就在这一刻真正写进 zh。
    pxc_dbe::materialize(g_db, g_bldConflicts);
    // 覆盖前备份旧表：二次确认里承诺了"旧表会备份到 userdata/.db_backup/"，那句话必须真。
    // 之前只有「修改当前对照表」（replaceDbFile）做备份，「生成并覆盖」是裸写 ——
    // 提示说备份了、实际只是留了份草稿，真出事时找不到能回滚的那一份。
    const std::string bak = pxc_dbe::backupDbFile(dbPath());
    std::string err;
    if (!pxc_dbe::saveDb(dbPath(), g_db, err)) {
        g_ovStatus = "保存失败：" + err;
        pxc_log::write("ERROR", "Builder", "保存对照表失败：" + err);
        requestUpdate();
        return;
    }
    g_dbDirty = false;
    g_draftPending = false;
    pxc_dbe::dropDraft(draftPath());   // 已经落进正式表了，草稿没有存在意义
    g_draftPresent = false;
    g_db.addedKeys.clear();            // 已经覆盖进表了，它们不再"新增"
    g_bldAddedSet.clear();
    g_bldCov = pxc_dbe::coverage(g_db, g_bldKeys);
    char buf[640];
    sprintf_s(buf,
              "已生成并覆盖当前对照表：版本 %s｜条目 %d｜待校对 %d｜待翻译 %d｜孤儿 %d。"
              "旧表备份：%s。回到汉化页即可用它生成。",
              g_db.version.empty() ? "(未标版本)" : g_db.version.c_str(), (int)g_db.entries.size(),
              g_bldCov.pending, g_bldCov.pendingTrans, g_bldCov.orphan,
              bak.empty() ? "无（盘上没有旧表）" : bak.c_str());
    g_ovStatus = buf;
    pxc_log::write("INFO", "Builder",
                   "覆盖对照表 " + g_db.version + "（条目 " + std::to_string((int)g_db.entries.size()) + "）");
    requestUpdate();
}

// 另一个出口：「生成对照表文件…」= 表 + 本次改动合并后的新版本，另存到任选位置。
// 与标题行「导出当前对照表」的区别：那个导出的是**磁盘上那份原样**（备份点），
// 这个导出的是**我正在编辑的内容**（含未保存改动，版本取底栏的输出版本）。
static void onBldExportMerged() {
    ovSaveAll();
    std::string ver = g_bldVer.get();
    if (ver.empty()) ver = g_db.version;
    std::string name = "translation_db";
    if (!ver.empty()) {
        std::string v = ver;
        for (char& c : v)
            if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                c == '>')
                c = '_';
        name += "_" + v;
    }
    name += ".json";
    std::filesystem::path out = pxc_nui::pickSave(
        L"生成对照表文件（表 + 本次改动）", pxc_util::pathFromUtf8(name).wstring(),
        {{L"对照表", L"*.json"}}, L"json");
    if (out.empty()) { requestUpdate(); return; }
    if (out.extension().empty()) out += L".json";

    pxc_tdb::Db copy = g_db;
    if (!ver.empty()) copy.version = ver;
    pxc_dbe::materialize(copy, g_bldConflicts);  // 导出的是"合并后"的结果，候选要落地
    std::string err;
    if (!pxc_dbe::saveDb(out, copy, err)) {
        g_ovStatus = "生成失败：" + err;
        requestUpdate();
        return;
    }
    // 候选已写进这次导出，但内存里还留着（没覆盖当前表，候选不该被清）——重新按 use 重建一次即可。
    g_ovStatus = "已生成对照表文件：" + pxc_util::toUtf8(out.filename().wstring().c_str()) +
                 "（版本 " + (ver.empty() ? std::string("(未标)") : ver) + "，含本次未保存的改动）";
    requestUpdate();
}

// 骨架来源的人话：维护者要一眼看出"这次是拿谁当参照比的"——
// 装多版本时运行期解析到的骨架未必是建表那份，孤儿数会跟着变，不写出来会以为是表坏了。
static std::string ovBaseLabel() {
    if (!g_bldBaseReady) return "骨架：（未加载）";
    const std::string& o = g_bldBase.origin;
    // 内置骨架已经没有了（§8.1.4）：骨架永远来自用户已装的那个 PXC 版本，
    // 所以认不出的 origin 也不能再退化成"内置骨架"这种会让人误会的说法。
    std::string who = "已装版本";
    if (o == "install-en") who = "安装目录 en";
    else if (o == "install-sample") who = "安装目录 sample";
    else if (o == "user-en") who = "用户目录 en";
    else if (o == "user-sample") who = "用户目录 sample";
    else if (o == "builder" || o == "builder-drop") who = "手动导入的英文包";
    else if (o != "builtin" && !o.empty()) who = o;
    return "骨架：" + who + " · " + std::to_string(g_bldBase.nodeCount) + " 节点";
}

// 顶栏第三行：骨架来源 + 英文包路径 + 汉化包路径，**同一行**。
// 以前骨架单独一行、两个包路径又一行，三条都短却各占一行，白吃掉 30px 纵向空间，
// 而纵向正是构建器最缺的（列表和字段区都靠它）。
static std::string ovMetaLine() {
    std::string s = ovBaseLabel();
    s += "    ｜    英文包：";
    // "英文包"这一栏指的是维护者额外指定的对比源；不填就是拿上面那层骨架当英文侧。
    s += g_bldEnPath.empty() ? std::string("未指定（用上面的骨架）") : g_bldEnPath;
    if (!g_bldZhPath.empty()) {
        s += "    ｜    汉化包：";
        s += g_bldZhPath;
    }
    return s;
}

// 汉化页角落按钮：维护者入口。与自定义译文层共用同一套浮窗，只换数据源。
static void onBldOpen() {
    ensureDb();
    if (!g_dbLoaded) { showToast("对照表未加载", "无法进入构建器", true); requestUpdate(); return; }
    g_ovMode = 1;
    g_ovTab.set(0);
    g_bldFilter.set(0);
    g_bldVer.set(g_db.version);
    g_ovQuery.set("");
    g_ovRowsOwner = -1;
    g_ovRows.clear();
    g_ovEdit.clear();
    g_ovEditFor.clear();
    for (int i = 0; i < kOvPages; ++i) g_ovData[i] = OvPageData{};
    if (!bldEnsureBase()) {
        // 没有骨架也要能进：否则连"导入英文包"这一步都做不了。
        g_bldBase = pxc_base::Baseline{};
        g_bldBaseReady = false;
        g_bldKeys.clear();
        g_bldKeySet.clear();
        g_bldCov = pxc_dbe::Coverage{};
        g_ovStatus = "还没有英文骨架 —— 先点「导入英文包」。";
        g_ovOpen.set(true);
        requestUpdate();
        return;
    }
    g_ovStatus.clear();
    // 有草稿就在状态行点出来：跨天维护时"上次还剩一半"是最容易丢的东西。
    {
        std::error_code ec;
        g_draftPresent = std::filesystem::exists(draftPath(), ec);
        if (g_draftPresent)
            g_ovStatus = "发现上次未完成的过程文件（data/.draft/db_draft.json）—— 可用下方「恢复草稿」接着改，"
                         "或「丢弃草稿」。";
    }
    g_ovOpen.set(true);
    bldRebuild("打开构建器");  // 键集合 / 覆盖率 / 分页 / 过滤 / 选中共用同一条路径
}


// ---------- 拖放：按当前页分发 ----------
static void onDropPathsImpl(std::vector<std::filesystem::path> paths) {
    pxc_log::write("INFO", "Drop", "分发 " + std::to_string(paths.size()) + " 个拖放项");
    if (g_page == Page::Registry) {
        int n = 0;
        for (const auto& p : paths) {
            std::wstring ext = p.extension().wstring();
            if (ext == L".exe" || ext == L".EXE") { registerExe(p); ++n; }
            else pxc_log::write("WARN", "Drop", "版本库页忽略非 exe: " + p.u8string());
        }
        // 页面上不再有状态行（§11.0 第 7 条），反馈只落日志。
        if (n == 0) pxc_log::write("WARN", "Drop", "拖入的不是 .exe，已忽略");
    } else if (g_page == Page::Projects) {
        // 拖入即导入（§7.12 P3）：目录进导入列表、点开才递归扫；.pxc 直接进列表。
        wbImportPaths(paths);
    } else if (g_page == Page::Locale) {
        if (g_ovOpen.get() && g_ovMode == 1) {
            // 构建器浮窗开着时拖目录进来 = 当包导入。英文骨架目录名一般是 en / sample locale，
            // 其余按汉化包处理 —— 顶栏按钮永远可用，拖放只是快捷方式。
            int nEn = 0, nZh = 0;
            for (const auto& p : paths) {
                std::error_code ec;
                if (!std::filesystem::is_directory(p, ec)) continue;
                std::filesystem::path lang = pxc_tdb::locateLangDir(p, "zh");
                if (lang.empty()) lang = p;
                const std::string leaf = pxc_util::toUtf8(lang.filename().wstring().c_str());
                const bool isEn = leaf == "en" || leaf == "sample locale" || leaf.rfind("en_", 0) == 0;
                if (isEn) {
                    pxc_base::Candidate c;
                    c.dir = lang;
                    c.origin = "builder-drop";
                    pxc_base::Baseline nb;
                    if (!pxc_base::load(c, nb)) continue;
                    // 与按钮路径同一条规则：只有"旧骨架里没有的键"才算新增、才默认标待翻译，
                    // 且新节点/消失节点要比**骨架 vs 骨架**（不能比译名）。
                    // oldKeys 必须在覆盖 g_bldBase 之前取 —— 反过来取到的就是新骨架自己。
                    bldEnsureBase();
                    std::set<std::string> oldKeys;
                    if (g_bldBaseReady)
                        for (const auto& k : pxc_dbe::enumerate(g_bldBase)) oldKeys.insert(k.key);
                    g_bldBase = nb;
                    g_bldBaseReady = true;
                    g_bldEnPath = pxc_util::toUtf8(lang.wstring().c_str());
                    g_db.addedKeys.clear();
                    if (!oldKeys.empty())
                        for (const auto& k : pxc_dbe::enumerate(g_bldBase))
                            if (!oldKeys.count(k.key)) g_db.addedKeys.push_back(k.key);
                    pxc_dbe::syncSkeleton(g_db, g_bldBase,
                                          oldKeys.empty() ? nullptr : &g_db.addedKeys,
                                          oldKeys.empty() ? nullptr : &oldKeys);
                    markDirty();
                    ++nEn;
                } else {
                    if (!bldEnsureBase()) continue;
                    pxc_dbe::importPack(g_db, g_bldBase.dir, lang, g_bldConflicts);
                    markDirty();
                    ++nZh;
                }
            }
            g_ovStatus = "拖入：" + std::to_string(nEn) + " 个英文包 / " + std::to_string(nZh) +
                         " 个汉化包。";
            if (nEn + nZh > 0) bldRebuild("拖放导入");
        } else {
            bool handled = false;
            for (const auto& p : paths) {
                std::error_code ec;
                if (!std::filesystem::is_directory(p, ec)) continue;
                std::filesystem::path root = p;
                if (p.filename().wstring() == L"Locale") root = p.parent_path();
                g_cfg.gen.user_dir = root.u8string();
                saveConfig();
                pxc_log::write("INFO", "Prefs", "拖入设为用户目录：" + root.u8string());
                handled = true;
                break;
            }
            if (!handled) showToast("汉化页只接受目录", "拖进来的应该是 PXC 用户目录", true);
        }
    }
    requestUpdate();
}

static void onDropPaths(std::vector<std::filesystem::path> paths) {
    guarded("拖放导入", [&] { onDropPathsImpl(std::move(paths)); });
}

// ---------- 版本库：卡片（v5，docs/07 §11.4） ----------
// 尺寸口径：参考图按界面缩放 110% 画的，所以逻辑值 = 图像 px ÷ 1.10（卡片 133×144 → 120×130）。
namespace regv {
constexpr float kPad = 28.0f;                 // 页边距（跨页一致）
// 固定头部的高度：标题行 + 添加/刷新按钮行。这个数**同时**是 page.host 的下移量与高度扣减量，
// 是唯一来源（正文 y 从 0 起）。版本库里任何"页面坐标 → 屏幕坐标"的换算都要加它。
constexpr float kHeaderH = 126.0f;
constexpr float kCardW = 120.0f;
constexpr float kCardH = 130.0f;
constexpr float kGap = 18.0f;                 // 卡片间距（参考图 20 ÷ 1.10）
constexpr float kGroupGap = 28.0f;            // 组间距（上一组最后一行 → 下一组标题）
constexpr float kIcon = 56.0f;                // 卡片图标 / 水印同尺寸
constexpr float kBadge = 16.0f;               // 变化角标直径（18 ÷ 1.10）
const core::Color cardBg = {53.0f / 255.0f, 57.0f / 255.0f, 67.0f / 255.0f, 1.0f};      // #353943
const core::Color cardBorder = {83.0f / 255.0f, 86.0f / 255.0f, 92.0f / 255.0f, 1.0f}; // #53565C
const core::Color metaFg = {0.55f, 0.60f, 0.68f, 1.0f};     // 全页唯一一档灰（§11.9）
const core::Color labelFg = {0.42f, 0.47f, 0.55f, 1.0f};    // 渠道标签（比 meta 再暗一档）
const core::Color lostFg = {0.88f, 0.52f, 0.48f, 1.0f};     // 「丢失」
} // namespace regv

static double regNowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static bool regIsSteam(const std::string& exePath) {
    for (const auto& e : g_cfg.registry)
        if (pxc_reg::samePath(e.exe_path, exePath)) return e.channel == "steam";
    return false;
}

// 这条是不是「便携版自动发现」管的（exe 落在启动器目录 / 父级目录及其一层子目录内）。
static bool regIsPortable(const std::string& exePath) {
    return pxc_reg::isPortablePath(exePath, g_exeDir);
}

// 卡片图标：素材 SVG 只读一次盘，之后每帧只传字符串（§11.9 的实现要点）。
static void loadRegIcons() {
    if (g_regIconsTried) return;
    g_regIconsTried = true;
    auto slurp = [](const char* name) {
        const std::filesystem::path p = dataDir() / "icons" / name;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(p, ec)) {
            pxc_log::write("WARN", "Registry", "图标素材缺失：" + p.u8string());
            return std::string();
        }
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    g_iconPxc = slurp("pxc.svg");
    g_iconItch = slurp("itch.svg");
    g_iconSteam = slurp("steam.svg");
}

// 侧栏顶部的品牌图标。
//
// 侧栏品牌区：navbar 的 `.brand` 只负责占位（图标传空格 0x20、标题传空串），
// 真正的 logo 与「PXC 启动器」字样都画在这里，盖在那块矩形上——图标尺寸、字样字号
// 都能自己定，不必去 patch vendored navbar.h（和当年不碰 `contextmenu.h` 同一条理由）。
//
// 矩形与 navbar 对齐（不算死数）：navbar 列从 `spacing.header` 起，先排图标行
// (`control.menuItem`)，再隔 `brandGap`(=typography.label) 排标题行 (`control.segmented`)。
// 图标行中线 = header + menuItem/2；标题行顶 = header + menuItem + label，高 = segmented。
// 换主题/改框架度量时两边一起变，不会错位。
static void composeBrand(core::dsl::Ui& ui) {
    const auto th = uiTheme();
    const auto& m = th.metrics;
    const float kBrandLogo = 44.0f;       // 图标比 navbar 默认 34 大一圈
    const float kBrandText = 24.0f;       // 字样比 navbar 默认 30 小一圈
    const float iconRowCenterY = m.spacing.header + m.control.menuItem * 0.5f;

    // 图标：**预渲染的 44px PNG**，不用 `ui.svg`。
    //
    // 原因：EUI 把任何 SVG 固定栅格化成 512×512（image_source.cpp 的 kSvgRasterSize），
    // 再用 GL_LINEAR、**无 mipmap** 缩到 44 px —— 11 倍缩小只采样到很小的邻域，圆角/描边
    // 就是硬锯齿（用户看到的就是"没抗锯齿"）。换一张与显示尺寸 1:1 的位图后没有缩小采样，
    // 边缘是素材本身的高质量抗锯齿（mkicon.py 线性光 + 预乘 alpha 面积平均缩出来的）。
    // 改 kBrandLogo 时要同步改 tmp/mkicon.py 的 BRAND_PX。
    const std::filesystem::path logoPng = dataDir() / "icons" / "launcher_44.png";
    std::error_code logoEc;
    if (std::filesystem::is_regular_file(logoPng, logoEc)) {
        const float lx = (kNavW - kBrandLogo) * 0.5f;
        const float ly = iconRowCenterY - kBrandLogo * 0.5f;
        ui.image("nav.brand.logo")
            .position(lx, ly)
            .size(kBrandLogo, kBrandLogo)
            .zIndex(2)                    // 压住 navbar 底与那行空格，但低于浮层(900+)
            .stretch()                    // 方图入方框：1:1，不拉伸
            .source(logoPng.u8string())
            .build();
    }

    // 字样：盖在 navbar 标题行上，与 navbar 同构（字重/居中对齐一样）只是更小，
    // 这样「图标放大、字号缩小」的视觉比例才站得住。
    ui.text("nav.brand.title.ov")
        .position(0.0f, m.spacing.header + m.control.menuItem + m.typography.label)
        .size(kNavW, m.control.segmented)
        .text("PXC 启动器")
        .fontSize(kBrandText)
        .lineHeight(kBrandText + m.typography.lineGap)
        .fontWeight(820)
        .color(th.text)
        .horizontalAlign(core::HorizontalAlign::Center)
        .zIndex(2)
        .build();
}

static void openRegMenu(const std::string& exePath, float cx, float cy) {
    g_regMenu.open = true;
    g_regMenu.exe = exePath;
    g_regMenu.version.clear();
    g_regMenu.sub = false;
    for (const auto& e : g_cfg.registry)
        if (pxc_reg::samePath(e.exe_path, exePath)) { g_regMenu.version = e.product_version; break; }
    // 菜单位置**自己算**：右键事件给的 e.x/e.y 在被缩放过的坐标系里（工作台的教训）。
    // cy 是 canvas 坐标（正文视口内），屏幕 y = cy - scroll + 头部高度。
    const float menuH = 5 * 30.0f + 12.0f;
    g_regMenu.x = std::min(kNavW + cx + 24.0f, std::max(kNavW + 40.0f, g_scrW - 220.0f));
    g_regMenu.y = std::max(8.0f, std::min(cy - g_pageScroll.get() + regv::kHeaderH + 24.0f,
                                         g_scrH - menuH - 8.0f));
    requestUpdate();
}

static void composeRegCard(core::dsl::Ui& ui, const std::string& id, const pxc_store::ExeEntry& e,
                           float cx, float cy) {
    using core::HorizontalAlign;
    using core::VerticalAlign;
    const std::string exePath = e.exe_path;
    const bool sel = !g_regSel.empty() && pxc_reg::samePath(g_regSel, exePath);
    const bool lost = g_regMissing.count(exePath) > 0;
    const bool steam = e.channel == "steam";
    // 角标条件就这一个表达式，不需要额外布尔位（§11.6）：当前版本 != 已确认版本。
    const bool changed = steam && !e.acked_version.empty() &&
                         !pxc_reg::sameVersion(e.acked_version, e.product_version);

    ui.rect(id + ".bg").position(cx, cy).size(regv::kCardW, regv::kCardH)
        .color(regv::cardBg).radius(12.0f)
        .border(sel ? 1.5f : 1.0f, sel ? g_accent : regv::cardBorder)
        .onClick([exePath] {
            // 双击启动靠自己记时间：EUI 没有 doubleClick 事件。
            // 420ms 的依据：真机实测两次按下相隔 92ms 都能各自触发 onClick（OS 的双击消息没被吞），
            // 所以这里只是"把两次相邻的单击认成一次双击"，不是等系统发 DBLCLK。
            const double t = regNowMs();
            if (pxc_reg::samePath(g_regSel, exePath) && g_regClickMs > 0.0 && t - g_regClickMs < 420.0) {
                g_regClickMs = 0.0;
                launchRegistryExe(exePath);
                return;
            }
            g_regClickMs = t;
            g_regSel = exePath;
            requestUpdate();
        })
        .onContextMenu([exePath, cx, cy](const core::PointerEvent&, const core::Rect&) {
            g_regSel = exePath;
            openRegMenu(exePath, cx, cy);
        })
        .onHover([exePath, cx, cy](bool on) {
            if (on) {
                g_regHover = exePath;
                g_regHoverX = kNavW + cx;
                // 存**页面坐标**（卡片下缘 +6），不存屏幕坐标：滚轮滚动后鼠标不动，
                // onHover 不会重发，存屏幕坐标的话提示条会停在旧位置、挂到别的卡片下面。
                // 真正换算成屏幕坐标在绘制处做（`pageY - g_pageScroll + kHeaderH`）。
                g_regHoverY = cy + regv::kCardH + 6.0f;
                g_regHoverScroll = g_pageScroll.get();
            } else if (pxc_reg::samePath(g_regHover, exePath)) {
                g_regHover.clear();
            }
            requestUpdate();
        })
        .build();

    // 图标：`data/icons/pxc.svg`（与工作台同一份素材）。素材缺失时留一个空槽，不画破图。
    if (!g_iconPxc.empty()) {
        ui.svg(id + ".ic").position(cx + 10.0f, cy + 10.0f).size(regv::kIcon, regv::kIcon)
            .source(g_iconPxc).opacity(lost ? 0.4f : 1.0f).build();
    }

    // 版本号：本页主信息，16/600（比设置页多这一档，§11.9）
    // 可携带副本（落在扫描范围内的那条）在版本号下面多一行「便携」。
    // **版本号基线对所有卡片统一**（cy+68）：便携卡片只是多画一行，不是把版本号往上挤 ——
    // 否则同一行的卡片里就它高一截，看起来像渲染错位。
    const bool portable = isPortableEntry(exePath);
    const float verW = regv::kCardW - 24.0f;
    const std::string ver = e.product_version.empty() ? std::string("—") : e.product_version;
    ui.text(id + ".ver").position(cx + 12.0f, cy + 68.0f).size(verW, 20.0f)
        .text(wbFit(ver, verW, 16.0f)).fontSize(16.0f).lineHeight(20.0f).fontWeight(600)
        .color(lost ? regv::metaFg : core::Color{0.95f, 0.97f, 1.0f, 1.0f}).build();

    // 「便携」标签：弱底 + 主题色字，和渠道标签区分开（那行是灰色小写，这行是形态说明）。
    // 非便携卡片这一块留白，但版本号与渠道行的基线仍是统一的。
    //
    // **尺寸必须按实际渲染字号算**：dsl 的 fontSize(10) 在渲染时会乘全局系数（基准 1.4），
    // 真正画出来是 14px 的字。早先底子硬编码 32×14，两个字撑到贴右边缘、底边压线 ——
    // 看着就像文字跑出了标签。宽度量一次渲染宽度再加左右内边距，高度按字号比例给。
    if (portable) {
        const float pbFs = 10.0f;                                  // 逻辑字号（渲染时会再乘 textScale）
        const float pbW = wbRealTextW("便携", pbFs, 600) + 12.0f;   // 左右各 6
        const float pbH = pbFs * textScale() * 1.42f;
        const float pbY = cy + 89.0f;                              // 下沿 ≈109，渠道行在 112，留 3px
        ui.rect(id + ".pbg").position(cx + 12.0f, pbY).size(pbW, pbH)
            .color(core::Color{g_accent.r, g_accent.g, g_accent.b, 0.18f}).radius(pbH * 0.5f).build();
        ui.text(id + ".pbt").position(cx + 12.0f, pbY).size(pbW, pbH).text("便携")
            .fontSize(pbFs).lineHeight(pbFs * 1.2f).fontWeight(600).color(g_accent)
            .horizontalAlign(HorizontalAlign::Center).verticalAlign(VerticalAlign::Center).build();
    }

    // 渠道标签：照参考图小写。丢失时这一行改成「丢失」，比另起一行更省空间也更好读。
    ui.text(id + ".ch").position(cx + 12.0f, cy + 112.0f).size(verW, 16.0f)
        .text(lost ? "丢失" : (steam ? "steam" : "itch"))
        .fontSize(12.0f).lineHeight(16.0f)
        .color(lost ? regv::lostFg : regv::labelFg).build();

    // 水印：右下角 5% 不 tint —— 素材 fill 是 #C5C3C0，5% 叠在 #353943 上约 +7/255，正是参考图的效果。
    const std::string& wm = steam ? g_iconSteam : g_iconItch;
    if (!wm.empty()) {
        ui.svg(id + ".wm")
            .position(cx + regv::kCardW - 10.0f - regv::kIcon, cy + regv::kCardH - 10.0f - regv::kIcon)
            .size(regv::kIcon, regv::kIcon).source(wm).opacity(0.05f).build();
    }

    // 变化角标：点它看说明（卡片单击仍是"选中"，两个动作各归各的）
    if (changed) {
        const float bx = cx + regv::kCardW - 9.0f - regv::kBadge;
        const float by = cy + 9.0f;
        ui.rect(id + ".bd").position(bx, by).size(regv::kBadge, regv::kBadge)
            .color(g_accent).radius(regv::kBadge * 0.5f).build();
        ui.text(id + ".bdt").position(bx, by).size(regv::kBadge, regv::kBadge).text("!")
            .fontSize(10.0f).lineHeight(regv::kBadge).fontWeight(700).color({1, 1, 1, 1})
            .horizontalAlign(HorizontalAlign::Center).verticalAlign(VerticalAlign::Center).build();
        components::mouseArea(ui, id + ".bdhit").position(bx - 2.0f, by - 2.0f)
            .size(regv::kBadge + 4.0f, regv::kBadge + 4.0f)
            .onTap([e, steam] {
                g_changeDlg.open = true;
                g_changeDlg.exe = e.exe_path;
                g_changeDlg.from = e.acked_version;
                g_changeDlg.to = e.product_version;
                pxc_log::write("INFO", "Registry",
                               "查看版本变化：" + e.exe_path + " " + e.acked_version + " -> " +
                                   e.product_version);
                requestUpdate();
            }).build();
    }
}


// 自定义译文层浮窗：左侧条目行（中文名 / 文件名 + 已改标记）
static void composeOvItemRow(core::dsl::Ui& ui, const std::string& id, std::int64_t index,
                             float width, float height) {
    OvPageData& pd = ovCur();
    if (index < 0 || index >= (std::int64_t)pd.view.size()) return;
    const int ni = pd.view[(size_t)index];
    if (ni < 0 || ni >= (int)pd.items.size()) return;
    const OvItem& it = pd.items[(size_t)ni];
    const bool sel = (ni == pd.sel);
    components::mouseArea(ui, id + ".hit").position(0.0f, 0.0f).size(width, height - 2.0f).radius(6.0f)
        .color(sel ? core::Color{0.23f, 0.42f, 0.62f, 0.95f} : core::Color{0.17f, 0.19f, 0.23f, 0.75f})
        .onTap([ni] { ovSelect(ni); }).build();
    // 注释页显示文件名（整篇正文塞不进行内）；其余页 中文优先、无中文回退英文
    const bool isNotes = ovCurPage() == (int)OvPage::Notes;
    const std::string shown = isNotes ? it.id : (it.zh.empty() ? it.en : it.zh);
    ui.text(id + ".zh").position(8.0f, 0.0f).size(width - 30.0f, height - 2.0f).text(shown)
        .fontSize(13.0f).lineHeight(13.0f).color({0.93f, 0.95f, 1.0f, 1.0f})
        .verticalAlign(core::VerticalAlign::Center).build();
    // 右端状态标记（构建器，一个字符位置只能放一个，按"最该看的"排优先级）：
    //   孤 = 孤儿（新骨架里没了） 校 = 待校对（包预填/冲突未裁决）
    //   待 = 待翻译（人工标记要翻） 新 = 本次新增  ○ = 还没译文
    const float dotW = 22.0f;
    if (g_ovMode == 1) {
        const char* mark = nullptr;
        core::Color mc{0.52f, 0.56f, 0.64f, 1.0f};
        if (it.orphan) { mark = "孤"; mc = {0.92f, 0.45f, 0.42f, 1.0f}; }
        else if (it.pend) { mark = "校"; mc = {0.95f, 0.78f, 0.36f, 1.0f}; }
        else if (it.pendTr) { mark = "待"; mc = {0.55f, 0.75f, 1.0f, 1.0f}; }
        else if (it.isNew) { mark = "新"; mc = {0.62f, 0.82f, 0.58f, 1.0f}; }
        else if (it.zh.empty()) { mark = "○"; mc = {0.52f, 0.56f, 0.64f, 1.0f}; }
        if (mark)
            ui.text(id + ".dot").position(width - dotW, 0.0f).size(dotW - 2.0f, height - 2.0f).text(mark)
                .fontSize(11.0f).lineHeight(11.0f).color(mc)
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center).build();
    } else if (it.marked) {
        ui.text(id + ".dot").position(width - 20.0f, 0.0f).size(14.0f, height - 2.0f).text("●")
            .fontSize(11.0f).lineHeight(11.0f).color({0.95f, 0.78f, 0.36f, 1.0f})
            .verticalAlign(core::VerticalAlign::Center).build();
    }
}

// 右侧字段行高：构建器多一行（两个开关 + 冲突裁决按钮），注释页是多行整篇编辑。
static float ovFieldRowHeight() {
    if (ovCurPage() == (int)OvPage::Notes) return 300.0f;
    return (g_ovMode == 1) ? 78.0f : 58.0f;
}

// 条目级标记是从字段级**聚合**出来的：该条目下**任一个键**打了标，左列表就显示标记、
// 「待翻译/待校对」筛选就能筛到它。反过来讲，一个节点里可以只有节点名要翻、端口提示不翻 ——
// 这正是不能用条目级开关一刀切的原因（早期做过一版条目级，整条一起标，不想翻的也会进导出队列）。
//
// 聚合范围：骨架上的全部键 + 表里的全部键（孤儿也在内）。按 id 归并一次即可，
// 不要"每个条目各扫一遍全量键"（942 × 17000 是三次方级的浪费）。
static void ovAggregateEntryFlags(int page) {
    OvPageData& pd = g_ovData[page];
    std::set<std::string> ptSet(g_db.pendingTranslate.begin(), g_db.pendingTranslate.end());
    std::set<std::string> prSet(g_db.pendingReview.begin(), g_db.pendingReview.end());
    struct Flags { bool pt = false; bool pr = false; };
    std::unordered_map<std::string, Flags> agg;
    auto acc = [&](const std::string& key) {
        if ((int)pxc_dbe::catOf(key) != page) return;
        const std::string id = pxc_dbe::idOf(key);
        if (id.empty()) return;
        Flags& f = agg[id];
        if (ptSet.count(key)) f.pt = true;
        if (prSet.count(key)) f.pr = true;
    };
    for (const auto& k : g_bldKeys) acc(k.key);
    for (const auto& kv : g_db.entries) acc(kv.first);
    for (auto& it : pd.items) {
        auto f = agg.find(it.id);
        it.pendTr = (f != agg.end()) && f->second.pt;
        it.pend = (f != agg.end()) && f->second.pr;
    }
}

// 打标/摘标后**只**刷新当前条目在左列表里的两个标记（不重建 view）。
// 聚合口径与 ovAggregateEntryFlags 一致：本条目下任一个键打了标就算。
static void ovRefreshItemFlags() {
    OvPageData& pd = ovCur();
    if (pd.sel < 0 || pd.sel >= (int)pd.items.size()) return;
    bool pt = false, pr = false;
    for (const auto& r : g_ovRows) {
        if (pxc_dbe::isPendingTranslate(g_db, r.key)) pt = true;
        if (pxc_dbe::isPendingReview(g_db, r.key)) pr = true;
    }
    pd.items[(size_t)pd.sel].pendTr = pt;
    pd.items[(size_t)pd.sel].pend = pr;
    g_bldCov = pxc_dbe::coverage(g_db, g_bldKeys);
    ++g_ovDataVer;   // 「待翻译 / 待校对」筛选下的条目数变了
}

// 迷你复选框（不引组件：14px 方块 + 文字，整块可点）。
// 返回下一个控件的 x，方便一行里排两个。
static float ovToggle(core::dsl::Ui& ui, const std::string& id, float x, float y, float w,
                      const std::string& label, bool on, std::function<void(bool)> fn) {
    ui.rect(id).position(x, y).size(w, 18.0f)
        .states(core::Color{0.0f, 0.0f, 0.0f, 0.0f}, core::Color{0.24f, 0.27f, 0.33f, 0.55f},
                core::Color{0.30f, 0.34f, 0.40f, 0.75f})
        .radius(4.0f).instantStates()
        .onClick([fn, on] { fn(!on); }).build();
    const float box = 13.0f;
    ui.rect(id + ".box").position(x + 2.0f, y + 2.5f).size(box, box)
        .color(on ? core::Color{0.26f, 0.52f, 0.86f, 1.0f} : core::Color{0.20f, 0.23f, 0.28f, 1.0f})
        .radius(3.0f)
        .border(1.0f, on ? core::Color{0.48f, 0.72f, 1.0f, 0.95f}
                         : core::Color{0.38f, 0.42f, 0.50f, 1.0f})
        .build();
    if (on)
        ui.text(id + ".chk").position(x + 2.0f, y + 2.0f).size(box, box).icon(0xF00C)
            .fontSize(8.0f).lineHeight(box).color({1.0f, 1.0f, 1.0f, 1.0f})
            .horizontalAlign(core::HorizontalAlign::Center)
            .verticalAlign(core::VerticalAlign::Center).build();
    ui.text(id + ".lab").position(x + box + 7.0f, y).size(std::max(10.0f, w - box - 9.0f), 18.0f)
        .text(label).fontSize(11.0f).lineHeight(18.0f)
        .color(on ? core::Color{0.86f, 0.91f, 1.0f, 1.0f} : core::Color{0.62f, 0.68f, 0.78f, 1.0f})
        .verticalAlign(core::VerticalAlign::Center).build();
    return x + w + 8.0f;
}

// 自定义译文层浮窗：右侧字段（分类名 + 英文锚 + 编辑框）。虚拟列表逐行渲染。
static void composeOvFieldRow(core::dsl::Ui& ui, const std::string& id, std::int64_t index,
                              float width, float height) {
    if (index < 0 || index >= (std::int64_t)g_ovRows.size()) return;
    if (index >= (int64_t)g_ovEdit.size()) return;
    const OvRow& r = g_ovRows[(size_t)index];
    const std::string sid = id;
    const std::string cur = g_ovEdit[(size_t)index] ? g_ovEdit[(size_t)index]->get() : std::string();
    const bool builder = (g_ovMode == 1);
    const bool overridden = !cur.empty() && cur != r.defZh;
    // 冲突候选：zh 一个字都没动，所以"表内原文"永远在；两个按钮只是切 use。
    const pxc_dbe::Conflict* cf = nullptr;
    if (builder) {
        auto it = g_bldConflicts.find(r.key);
        if (it != g_bldConflicts.end()) cf = &it->second;
    }
    // 标题行：分类名 + 英文锚 + （改过时）改之前的那个值 —— 要能看到"原来是什么"。
    // 用户模式的"原来"是对照表译文；构建器模式的"原来"是表里当前存的值（还没保存的改动）。
    std::string head = r.label + "    " + r.en;
    if (overridden && !r.multi) {
        if (builder)
            head += r.defZh.empty() ? "    表内：无" : ("    表内：" + r.defZh);
        else
            head += r.defZh.empty() ? "    原文：（对照表无译文）" : ("    原文：" + r.defZh);
    }
    // 标题行右侧的排布（构建器）：[冲突候选文本 / 已改] —— [待翻译][待校对]。
    // 两个开关是**字段级**的：同一个节点里"节点名要翻、端口提示不翻"是常态，
    // 条目级开关会把这个差异抹平（整条一起标 → 不想翻的也跟着进导出队列）。
    const float tw = 58.0f;                       // 单个开关宽（13 方块 + 7 间距 + 3 汉字）
    const float togW = tw * 2.0f + 8.0f;          // 两个开关 + 中间 8px
    const float togX = width - togW;
    const float headW = builder ? (cf ? width - togW - 248.0f : width - togW - 76.0f)
                                : width - 80.0f;
    ui.text(sid + ".lab").position(0.0f, 0.0f).size(std::max(80.0f, headW), 16.0f)
        .text(head).fontSize(11.0f).lineHeight(14.0f)
        .color(overridden ? core::Color{0.78f, 0.70f, 0.50f, 1.0f} : core::Color{0.62f, 0.68f, 0.78f, 1.0f})
        .build();
    if (overridden && !cf)
        ui.text(sid + ".flag").position(width - togW - 70.0f, 0.0f).size(62.0f, 16.0f)
            .text(builder ? "已改" : "已自定义")
            .fontSize(11.0f).lineHeight(14.0f).color({0.95f, 0.78f, 0.36f, 1.0f})
            .horizontalAlign(core::HorizontalAlign::Right).build();
    if (cf)
        ui.text(sid + ".cf").position(width - togW - 240.0f, 0.0f).size(232.0f, 16.0f)
            .text((cf->use ? "新包文本已选用：" : "新包另有译文：") + cf->pack)
            .fontSize(11.0f).lineHeight(14.0f).color({0.95f, 0.72f, 0.34f, 1.0f})
            .horizontalAlign(core::HorizontalAlign::Right).build();
    if (builder) {
        // 只对**这个键**打标/摘标，不重建列表（否则在「待翻译」筛选下取消标记，
        // 整行会从列表里消失，维护者还得重新搜回来）。
        const std::string key = r.key;
        const bool pt = pxc_dbe::isPendingTranslate(g_db, key);
        const bool pr = pxc_dbe::isPendingReview(g_db, key);
        float tx = ovToggle(ui, sid + ".pt", togX, 0.0f, tw, "待翻译", pt, [key](bool v) {
            pxc_dbe::setPending(g_db, key, v, pxc_dbe::isPendingReview(g_db, key));
            markDirty();
            ovRefreshItemFlags();
            requestUpdate();
        });
        ovToggle(ui, sid + ".pr", tx, 0.0f, tw, "待校对", pr, [key](bool v) {
            pxc_dbe::setPending(g_db, key, pxc_dbe::isPendingTranslate(g_db, key), v);
            markDirty();
            ovRefreshItemFlags();
            requestUpdate();
        });
    }

    const std::string ph = builder
        ? (r.multi ? std::string("留空 = 不覆盖英文原文档")
                   : std::string("留空 = 不翻译（生成时回退英文）"))
        : (r.multi ? std::string("留空 = 使用原版文档")
                   : (r.defZh.empty() ? ("留空 = 用英文：" + r.en) : ("留空 = 默认：" + r.defZh)));
    const bool multi = r.multi;
    // 行高恒定（虚拟列表要求），内容自适应：只有存在冲突候选的那一行才在底部留一条
    // 裁决按钮带（24px），其余行把这 24px 让给输入框 —— 中文译文换行时更易读。
    const float inH = height - ((builder && cf) ? 46.0f : (builder ? 26.0f : 24.0f));
    ui.stack(sid + ".wrap").position(0.0f, 19.0f).size(width, inH).content([&, index, ph, multi, inH] {
        components::input(ui, sid + ".in").theme(uiTheme()).size(width, inH)
            .fontSize(12.0f).placeholder(ph).multiline(multi)
            .bind(*g_ovEdit[(size_t)index]).build();
    }).build();

    if (!builder || !cf) return;   // 两个标记开关在标题行右端，不需要额外的行

    // 冲突裁决：两个按钮都只是切 use，zh 不动 —— 所以能来回点，原文永远丢不了。
    const float cy = height - 24.0f;
    {
        const std::string packText = cf->pack;
        const std::string key = r.key;
        const float bw = 96.0f;
        const float bx = width - bw * 2.0f - 8.0f;
        ui.stack(sid + ".usepack.wrap").position(bx, cy - 1.0f).size(bw, 22.0f).content([&, key, packText] {
            components::button(ui, sid + ".usepack").theme(uiTheme()).size(bw, 22.0f).fontSize(10.0f)
                .text("替换为新包文本")
                .onClick([key, packText] {
                    g_bldConflicts[key].use = true;
                    g_bldConflicts[key].pack = packText;
                    markDirty();
                    requestUpdate();
                }).build();
        }).build();
        ui.stack(sid + ".usebase.wrap").position(bx + bw + 8.0f, cy - 1.0f).size(bw + 12.0f, 22.0f)
            .content([&, key] {
                components::button(ui, sid + ".usebase").theme(uiTheme()).size(bw + 12.0f, 22.0f)
                    .fontSize(10.0f).text("恢复对照表原文")
                    .onClick([key] {
                        g_bldConflicts[key].use = false;
                        markDirty();
                        requestUpdate();
                    }).build();
            }).build();
    }
}

// ---------- 各页内容 ----------
static void composeRegDialogs(core::dsl::Ui& ui, const eui::Screen& screen);

// 版本库：标题 + 添加/刷新固定在顶部，只有下面的卡片列表滚动（与设置页同一套做法，docs/06 §10.12）。
// 头部高度见 regv::kHeaderH；正文 y 从 0 起。
// 注意 g_pageScroll 仍是"视口内的滚动量"，版本库里任何"页面坐标 → 屏幕坐标"的换算
// 都要再加 regv::kHeaderH（右键菜单、hover 浮窗），否则会整体偏一个头部的高度。
static void composeRegistryHeader(core::dsl::Ui& ui, float width) {
    const float pad = regv::kPad;
    const float availW = width - pad * 2.0f;
    float x = pad, y = 24.0f;

    ui.text("reg.title").position(x, y).size(240.0f, 40.0f).text("版本库")
        .fontSize(28.0f).lineHeight(34.0f).fontWeight(800).color({0.95f, 0.97f, 1.0f, 1.0f}).build();
    // 标题旁提示：参考图里就有这一行，保留；其余说明文字一概不显示（§11.0 第 7 条）。
    // 位置按**量出来的标题宽**排，不写死偏移 —— 字号有 1.4 基准 + 用户倍率，写死一定会撞。
    const float titleW = wbRealTextW("版本库", 28.0f, 800);
    ui.text("reg.hint").position(x + titleW + 14.0f, y + 10.0f)
        .size(std::max(80.0f, availW - titleW - 14.0f), 24.0f)
        .text("可以把 PXC 的 exe 直接拖到窗口里").fontSize(12.0f).lineHeight(24.0f)
        .color(regv::metaFg).verticalAlign(core::VerticalAlign::Center).build();
    y += 56.0f;

    // 按钮行：全页**唯一**一个主色按钮是「添加 exe」——两个主色等于没指示（docs/06 §10.10）。
    ui.stack("reg.add.wrap").position(x, y).size(118.0f, 34.0f).content([&] {
        components::button(ui, "reg.add").theme(uiTheme()).size(118.0f, 34.0f).text("添加 exe").fontSize(13.0f)
            .onClick([] { guarded("添加 exe", onAddExe); }).build();
    }).build();
    // 刷新按钮：图标 + 文本。只放一个图标时，FA 字形在 em 框里的留白本来就不匀，
    // 又没有文本作参照，看起来就是"歪的"；带上「刷新」两个字，基线一对就正了。
    // 宽度 = 左右 section 内边距(2×16) + 图标框(fontSize×1.15) + 行内 gap(6) + 文字实宽。
    const float refreshW = 32.0f + 13.0f * 1.15f + 6.0f + wbRealTextW("刷新", 13.0f, 400);
    ui.stack("reg.refresh.wrap").position(x + 128.0f, y).size(refreshW, 34.0f).content([&] {
        components::button(ui, "reg.refresh").theme(uiTheme(), false).size(refreshW, 34.0f)
            .icon(0xF021).iconSize(13.0f).text("刷新").fontSize(13.0f)
            .onClick([] {
                guarded("刷新版本库", [] {
                    refreshRegistry("刷新按钮");
                    requestUpdate();
                });
            }).build();
    }).build();
    y += 46.0f;
}

// 正文：分组标题 + 卡片。视口内坐标从 0 起 —— 头部由 page.header 画在视口上方。
static void composeRegistry(core::dsl::Ui& ui, float width) {
    const float pad = regv::kPad;
    const float availW = width - pad * 2.0f;
    const float x = pad;
    float y = 0.0f;

    int cols = (int)std::floor((availW + regv::kGap) / (regv::kCardW + regv::kGap));
    if (cols < 1) cols = 1;

    // 两组固定顺序：itch 在上、Steam 在下；某组为空则整组（含标题）不画（§11.4 空态）。
    for (int g = 0; g < 2; ++g) {
        const char* ch = (g == 0) ? "itch" : "steam";
        std::vector<const pxc_store::ExeEntry*> items;
        for (const auto& e : g_cfg.registry) {
            const std::string ec = e.channel.empty() ? "itch" : e.channel;
            if (ec == ch) items.push_back(&e);
        }
        if (items.empty()) continue;
        // 组内按版本降序，同版本按登记时间降序（不提供手动排序，§11.6）。
        std::sort(items.begin(), items.end(), [](const pxc_store::ExeEntry* a, const pxc_store::ExeEntry* b) {
            const int c = pxc_reg::compareVersion(a->product_version, b->product_version);
            if (c != 0) return c > 0;
            return a->registered_at > b->registered_at;
        });

        ui.text(std::string("reg.g.") + ch).position(x, y).size(availW, 16.0f).text(ch)
            .fontSize(12.0f).lineHeight(16.0f).color({0.50f, 0.55f, 0.63f, 1.0f}).build();
        y += 16.0f + 16.0f;   // 组标题 + 与卡片间距 16

        for (std::size_t i = 0; i < items.size(); ++i) {
            const int col = (int)(i % (std::size_t)cols);
            const int row = (int)(i / (std::size_t)cols);
            composeRegCard(ui, "reg.c" + std::to_string(g) + "." + std::to_string(i), *items[i],
                           x + (float)col * (regv::kCardW + regv::kGap),
                           y + (float)row * (regv::kCardH + regv::kGap));
        }
        const int rows = (int)((items.size() + (std::size_t)cols - 1) / (std::size_t)cols);
        y += (float)rows * (regv::kCardH + regv::kGap) - regv::kGap;
        y += regv::kGroupGap;
    }

    g_pageH = std::max(y + 40.0f, g_scrH * 0.6f);

    // 跨渠道同版本、还没问过偏好 → 进了这页就问一次（刷新时只置 pending，§11.6.2 触发点一）。
    if (!g_dbgNoPrefDlg && !g_prefAsk.empty() && !g_prefDlgOpen && !g_verDlgOpen && !g_dup.open &&
        !g_changeDlg.open) {
        g_prefDlgOpen = true;
    }
}

// ---------- 版本库：右键菜单 / 悬停路径 / 三类弹窗 ----------
static std::string preferOf(const std::string& version) {
    auto it = g_cfg.prefer.find(pxc_reg::normalizeVersion(version));
    if (it == g_cfg.prefer.end()) it = g_cfg.prefer.find(version);
    return it == g_cfg.prefer.end() ? std::string() : it->second;
}

static void regMenuAction(int action) {
    const std::string exe = g_regMenu.exe;
    g_regMenu.open = false;
    g_regMenu.sub = false;
    switch (action) {
    case 1:   // 打开所在文件夹
        pxc_nui::revealInExplorer(pxc_util::pathFromUtf8(exe), true);
        break;
    case 2:   // 设置版本号（已登记过也能改，弹窗标题会跟着变）
        g_pendingExe = pxc_util::pathFromUtf8(exe);
        g_manualVersion.set(g_regMenu.version);
        g_verDlgOpen = true;
        break;
    case 3:   // 重新读取版本
        onRereadVersion(exe);
        break;
    case 5:   // 删除（只对 itch 卡出现）
        onRemoveExe(exe);
        break;
    default:
        break;
    }
    requestUpdate();
}

static void composeRegMenu(core::dsl::Ui& ui, const eui::Screen& screen) {
    if (!g_regMenu.open) return;
    const bool cross = pxc_reg::hasCrossChannel(g_cfg.registry, g_regMenu.version);
    const bool steam = regIsSteam(g_regMenu.exe);

    struct Item {
        std::string label;
        int action = 0;
        bool arrow = false;
    };
    std::vector<Item> items = {
        {"打开所在文件夹", 1, false},
        {"设置版本号", 2, false},
        {"重新读取版本", 3, false},
    };
    // 仅当这张卡的版本**同时**有 itch 与 Steam 时才画偏好项（没同版本时右键没有这一项，§11.6.2）。
    if (cross) items.push_back({"设为默认启动", 4, true});
    // 「删除」的出现条件，一条判据管两类托管条目（§11.5）：
    //   由自动发现托管的卡（Steam 卡 / 落在便携扫描范围内的卡），**只要文件还在**就不给删 ——
    //   它由扫描维护，删掉后下一次刷新（启动就刷新）照样被找回来，点了没反应的菜单项比没有更糟。
    //   一旦**丢失**，这个前提就不成立了：Steam 重发现只收磁盘上存在的命中，便携发现也只扫存在的文件，
    //   两边都不会把它加回来 —— 此时「删除」是用户清掉这张卡的**唯一**手段，必须给，
    //   否则卸载一个 Steam 版本就会永久留一张删不掉的红色丢失卡。
    const bool lost = g_regMissing.count(g_regMenu.exe) > 0;
    const bool managed = (steam || regIsPortable(g_regMenu.exe)) && !lost;
    if (!managed) items.push_back({"删除", 5, false});

    const float w = 168.0f, rowH = 30.0f;
    const float h = (float)items.size() * rowH + 12.0f;
    const float mx = g_regMenu.x, my = g_regMenu.y;
    const float subW = 96.0f, subH = 2.0f * rowH + 12.0f;
    const float sx = mx + w + 2.0f;
    const float sy = std::min(my, std::max(8.0f, g_scrH - subH - 8.0f));

    ui.stack("reg.menu.host").position(0.0f, 0.0f).size(screen.width, screen.height)
        .zIndex(1200).content([&] {
            // 点空白关菜单。必须画在**第一句**，后画的元素才会盖在它上面。
            components::mouseArea(ui, "reg.menu.mask").position(0.0f, 0.0f)
                .size(screen.width, screen.height)
                .onTap([] {
                    g_regMenu.open = false;
                    g_regMenu.sub = false;
                    requestUpdate();
                }).build();
            ui.rect("reg.menu.bg").position(mx, my).size(w, h)
                .color({0.115f, 0.13f, 0.16f, 0.99f}).radius(10.0f)
                .border(1.0f, {0.26f, 0.29f, 0.35f, 1.0f}).build();
            for (int i = 0; i < (int)items.size(); ++i) {
                const std::string rid = "reg.menu.i" + std::to_string(i);
                const Item it = items[(size_t)i];
                const float ry = my + 6.0f + (float)i * rowH;
                ui.rect(rid).position(mx + 6.0f, ry).size(w - 12.0f, rowH)
                    .states({0, 0, 0, 0}, core::Color{0.20f, 0.23f, 0.29f, 1.0f},
                            core::Color{0.24f, 0.28f, 0.35f, 1.0f})
                    .radius(6.0f)
                    .onClick([action = it.action] {
                        if (action == 4) {          // 展开/收起子项，不动其他状态
                            g_regMenu.sub = !g_regMenu.sub;
                            requestUpdate();
                        } else {
                            regMenuAction(action);
                        }
                    }).build();
                ui.text(rid + ".t").position(mx + 16.0f, ry).size(w - 46.0f, rowH)
                    .text(it.label).fontSize(12.0f).lineHeight(rowH)
                    .color({0.88f, 0.92f, 0.98f, 1.0f})
                    .verticalAlign(core::VerticalAlign::Center).build();
                if (it.arrow) {
                    ui.text(rid + ".a").position(mx + w - 26.0f, ry).size(16.0f, rowH)
                        .icon(0xF054).fontSize(9.0f).lineHeight(rowH)
                        .color({0.62f, 0.67f, 0.75f, 1.0f})
                        .horizontalAlign(core::HorizontalAlign::Center)
                        .verticalAlign(core::VerticalAlign::Center).build();
                }
            }
            if (!g_regMenu.sub) return;
            ui.rect("reg.menu.sub").position(sx, sy).size(subW, subH)
                .color({0.115f, 0.13f, 0.16f, 0.99f}).radius(10.0f)
                .border(1.0f, {0.26f, 0.29f, 0.35f, 1.0f}).build();
            const std::string cur = preferOf(g_regMenu.version);
            for (int i = 0; i < 2; ++i) {
                const std::string ch = (i == 0) ? "itch" : "steam";
                const bool on = (cur == ch);
                const std::string rid = "reg.menu.s" + std::to_string(i);
                const float ry = sy + 6.0f + (float)i * rowH;
                ui.rect(rid).position(sx + 6.0f, ry).size(subW - 12.0f, rowH)
                    .states({0, 0, 0, 0}, core::Color{0.20f, 0.23f, 0.29f, 1.0f},
                            core::Color{0.24f, 0.28f, 0.35f, 1.0f})
                    .radius(6.0f)
                    .onClick([ch, ver = g_regMenu.version] {
                        setPrefer(ver, ch);
                        g_regMenu.open = false;
                        g_regMenu.sub = false;
                        requestUpdate();
                    }).build();
                ui.text(rid + ".t").position(sx + 16.0f, ry).size(subW - 44.0f, rowH)
                    .text(ch).fontSize(12.0f).lineHeight(rowH)
                    .color({0.88f, 0.92f, 0.98f, 1.0f})
                    .verticalAlign(core::VerticalAlign::Center).build();
                if (on) {
                    ui.text(rid + ".c").position(sx + subW - 30.0f, ry).size(18.0f, rowH)
                        .icon(0xF00C).fontSize(10.0f).lineHeight(rowH).color(g_accent)
                        .horizontalAlign(core::HorizontalAlign::Center)
                        .verticalAlign(core::VerticalAlign::Center).build();
                }
            }
        }).build();
}

// 悬停显示完整路径：参考图不显示路径，这是"两张卡片长得一样"时唯一的区分手段（§11.5）。
static void composeRegTooltip(core::dsl::Ui& ui, const eui::Screen& screen) {
    if (g_regHover.empty() || g_regMenu.open) return;
    // 滚轮一动，鼠标下面就已经不是原来那张卡了，但框架不会因为滚动重发 onHover ——
    // 不清掉的话提示条会挂在别人的卡片下面。清掉后鼠标一动就会重新判定、重新出现。
    if (g_regHoverScroll >= 0.0f && std::abs(g_pageScroll.get() - g_regHoverScroll) > 0.5f) {
        g_regHover.clear();
        return;
    }
    const float fs = 11.5f;
    const float w = std::min(screen.width - 40.0f, std::max(140.0f, wbRealTextW(g_regHover, fs, 400) + 20.0f));
    float x = g_regHoverX;
    if (x + w > screen.width - 12.0f) x = std::max(12.0f, screen.width - 12.0f - w);
    // g_regHoverY 是**页面坐标**（卡片下缘 +6）：视口起点在 kHeaderH，内容按 g_pageScroll 滚动。
    // 每帧现算，滚轮滚动后提示条就跟着卡片走，不会停在旧位置挂到别的卡片下面。
    float y = g_regHoverY - g_pageScroll.get() + regv::kHeaderH;
    if (y < regv::kHeaderH) return;                          // 卡片已滚出视口顶端
    if (y + 26.0f > screen.height - 12.0f) {
        y -= 42.0f;                                          // 贴底时翻到卡片上方
        if (y < regv::kHeaderH) return;
    }
    ui.rect("reg.tip.bg").position(x, y).size(w, 26.0f)
        .color({0.10f, 0.11f, 0.14f, 0.97f}).radius(7.0f)
        .border(1.0f, {0.28f, 0.31f, 0.37f, 1.0f}).zIndex(1190).build();
    ui.text("reg.tip.t").position(x + 10.0f, y).size(w - 20.0f, 26.0f).text(g_regHover)
        .fontSize(fs).lineHeight(26.0f).color({0.86f, 0.90f, 0.96f, 1.0f})
        .verticalAlign(core::VerticalAlign::Center).zIndex(1191).build();
}

// 「知道了」：acked 推进到当前版本，角标消失、重启不复现（§11.7）。
static void ackVersionChange() {
    for (auto& e : g_cfg.registry) {
        if (!pxc_reg::samePath(e.exe_path, g_changeDlg.exe)) continue;
        e.acked_version = e.product_version;
        pxc_log::write("INFO", "Registry", "确认版本变化：" + e.exe_path + " = " + e.product_version);
        break;
    }
    g_changeDlg = RegChangeDlg{};
    saveConfig();
    requestUpdate();
}

static void composeRegDialogs(core::dsl::Ui& ui, const eui::Screen& screen) {
    auto dlgBtn = [&](const char* rid, float bx, float by, float bw, const std::string& label,
                      std::function<void()> fn, bool primary) {
        ui.stack(std::string(rid) + ".w").position(bx, by).size(bw, 34.0f)
            .content([&, rid, bw, label, fn, primary] {
                components::button(ui, rid).theme(uiTheme(), primary).size(bw, 34.0f)
                    .text(label).fontSize(12.0f).onClick(fn).build();
            }).build();
    };

    // ① 无法识别版本 / 设置版本号（§11.8）——全页唯一的"拦路"弹窗
    if (g_verDlgOpen) {
        bool hasVer = false;
        for (const auto& e : g_cfg.registry)
            if (pxc_reg::samePath(e.exe_path, g_pendingExe.u8string())) hasVer = !e.product_version.empty();
        components::dialog(ui, "reg.verdlg").theme(uiTheme())
            .screen(screen.width, screen.height).size(360.0f, 208.0f).zIndex(430).open(true)
            .onOpenChange([](bool v) {
                if (!v && g_verDlgOpen) onCancelManual();
            })
            .content([&] {
                ui.text("rvd.t").position(24.0f, 18.0f).size(312.0f, 24.0f)
                    .text(hasVer ? "设置 PXC 版本号" : "无法识别当前 PXC 版本")
                    .fontSize(16.0f).lineHeight(22.0f).fontWeight(700)
                    .color({0.96f, 0.97f, 1.0f, 1.0f}).build();
                ui.text("rvd.n").position(24.0f, 46.0f).size(312.0f, 18.0f)
                    .text(g_pendingExe.filename().u8string()).fontSize(12.0f).lineHeight(16.0f)
                    .color(regv::metaFg).build();
                ui.text("rvd.b").position(24.0f, 70.0f).size(312.0f, 18.0f).text("请设置版本号")
                    .fontSize(13.0f).lineHeight(18.0f).color({0.80f, 0.85f, 0.92f, 1.0f}).build();
                ui.stack("rvd.in.w").position(24.0f, 98.0f).size(312.0f, 34.0f).content([&] {
                    components::input(ui, "rvd.in").theme(uiTheme()).size(312.0f, 34.0f).fontSize(13.0f)
                        .placeholder("例如 1.22.0.5").bind(g_manualVersion).build();
                }).build();
                dlgBtn("rvd.no", 172.0f, 152.0f, 76.0f, "取消", [] { onCancelManual(); }, false);
                dlgBtn("rvd.ok", 256.0f, 152.0f, 80.0f, "确定",
                       [] { guarded("确认版本号", onConfirmManual); }, true);
            }).build();
    }

    // ② 同渠道同版本撞车（§11.6.1 A）：两份一样的 PXC 没有意义，二选一
    if (g_dup.open) {
        components::dialog(ui, "reg.dupdlg").theme(uiTheme())
            .screen(screen.width, screen.height).size(420.0f, 224.0f).zIndex(431).open(true)
            .onOpenChange([](bool v) {
                if (!v && g_dup.open) {
                    g_dup = RegDup{};   // 关掉 = 保留现状（不动登记表）
                    requestUpdate();
                }
            })
            .content([&] {
                ui.text("rdu.t").position(24.0f, 18.0f).size(372.0f, 24.0f)
                    .text(std::string("本机已有同版本的 ") + g_dup.channel + " 版")
                    .fontSize(16.0f).lineHeight(22.0f).fontWeight(700)
                    .color({0.96f, 0.97f, 1.0f, 1.0f}).build();
                ui.text("rdu.m").position(24.0f, 50.0f).size(372.0f, 76.0f)
                    .text("版本 " + g_dup.version + "：" + g_dup.existing +
                          "\n两份相同的 PXC 没有意义，选择保留哪一个？")
                    .fontSize(11.5f).lineHeight(17.0f).color({0.72f, 0.77f, 0.86f, 1.0f})
                    .wrap(true).build();
                dlgBtn("rdu.keep", 24.0f, 170.0f, 100.0f, "保留现有", [] { resolveDup(false); }, true);
                dlgBtn("rdu.new", 132.0f, 170.0f, 116.0f, "用新的替换", [] { resolveDup(true); }, false);
            }).build();
    }

    // ③ Steam 版本变化说明（§11.7）：角标点出来的，关掉不等于知情，所以只有「知道了」推进 acked
    if (g_changeDlg.open) {
        components::dialog(ui, "reg.chgdlg").theme(uiTheme())
            .screen(screen.width, screen.height).size(400.0f, 208.0f).zIndex(432).open(true)
            .onOpenChange([](bool v) {
                if (!v && g_changeDlg.open) {
                    g_changeDlg = RegChangeDlg{};   // 角标保留：没确认就等于没看到
                    requestUpdate();
                }
            })
            .content([&] {
                ui.text("rch.t").position(24.0f, 18.0f).size(352.0f, 24.0f).text("Steam 版版本已变化")
                    .fontSize(16.0f).lineHeight(22.0f).fontWeight(700)
                    .color({0.96f, 0.97f, 1.0f, 1.0f}).build();
                ui.text("rch.m1").position(24.0f, 52.0f).size(352.0f, 20.0f)
                    .text("已从 " + g_changeDlg.from + " 变为 " + g_changeDlg.to + "。")
                    .fontSize(13.0f).lineHeight(20.0f).color({0.88f, 0.92f, 0.99f, 1.0f}).build();
                ui.text("rch.m2").position(24.0f, 78.0f).size(352.0f, 60.0f)
                    .text("Steam 自动更新，或你在 Steam 里切换了版本。工程文件的版本匹配、本机汉化骨架都按新版本计算。")
                    .fontSize(12.0f).lineHeight(18.0f).color({0.72f, 0.77f, 0.86f, 1.0f})
                    .wrap(true).build();
                dlgBtn("rch.ok", 24.0f, 154.0f, 92.0f, "知道了", [] { ackVersionChange(); }, true);
            }).build();
    }

    // ④ 启动偏好询问（§11.6.2）：进版本库页才问，且只问一次
    if (g_prefDlgOpen) {
        const std::string ver = g_prefAsk;
        components::dialog(ui, "reg.prefdlg").theme(uiTheme())
            .screen(screen.width, screen.height).size(400.0f, 200.0f).zIndex(433).open(true)
            .onOpenChange([ver](bool v) {
                if (!v && g_prefDlgOpen) {
                    // 关掉 = 问过了但没表态：写空串，以后不再问，启动走兜底（Steam 优先）
                    g_prefDlgOpen = false;
                    if (!g_prefAsk.empty()) setPrefer(g_prefAsk, "");
                    resumePrefLaunch(ver);   // 若这次是被"双击启动"拦下的，照样起
                    requestUpdate();
                }
            })
            .content([&] {
                ui.text("rpf.t").position(24.0f, 18.0f).size(352.0f, 24.0f)
                    .text(ver + " 同时有 itch 版与 Steam 版")
                    .fontSize(16.0f).lineHeight(22.0f).fontWeight(700)
                    .color({0.96f, 0.97f, 1.0f, 1.0f}).build();
                ui.text("rpf.m").position(24.0f, 52.0f).size(352.0f, 36.0f)
                    .text("默认用哪一个？启动这个版本、打开工程都会按它来，之后能在卡片上右键改。")
                    .fontSize(12.0f).lineHeight(18.0f).color({0.72f, 0.77f, 0.86f, 1.0f})
                    .wrap(true).build();
                dlgBtn("rpf.itch", 24.0f, 112.0f, 100.0f, "itch",
                       [ver] { g_prefDlgOpen = false; setPrefer(ver, "itch"); resumePrefLaunch(ver); }, false);
                dlgBtn("rpf.steam", 132.0f, 112.0f, 100.0f, "Steam",
                       [ver] { g_prefDlgOpen = false; setPrefer(ver, "steam"); resumePrefLaunch(ver); }, false);
                dlgBtn("rpf.later", 240.0f, 112.0f, 96.0f, "稍后决定", [ver] {
                    g_prefDlgOpen = false;
                    if (!g_prefAsk.empty()) setPrefer(g_prefAsk, "");
                    resumePrefLaunch(ver);
                    requestUpdate();
                }, false);
            }).build();
    }
}

// ---------- 页面级下拉（自绘） ----------
// 为什么不用 components::dropdown：它把弹层画在自己的 stack 里，而页面内容装在
// components::scrollView 的裁剪容器里（scrollview.h 里 `.clip()`），弹层一伸出就没了；
// 更致命的是它需要外部的 open 状态，只在 onChange 上挂回调时点了根本没反应（三个下拉全是这样）。
// 所以这里自绘 field，弹层用 contextMenu 画在**根级**（全屏 + zIndex 1050），不受裁剪影响。

struct DdField {
    std::string id;
    float ax = 0.0f, ay = 0.0f;               // 屏幕绝对坐标（弹层定位用）
    float w = 200.0f;
    std::vector<std::string> items;
    int sel = -1;
    std::function<void(int)> onPick;
    // 字体下拉：收起的当前值与展开的每一项都用**该项自己的字体**渲染（docs/08 §8.3）。
    // 项目里没有"预注册字体"这条路，但 TextPrimitive 的 fontFamily 含 "." 就按文件路径找，
    // 所以直接把 ttf 绝对路径当族名传即可。
    bool fonts = false;
    // 每项右侧的灰色小字（字体下拉用来标「内置 / 系统」）。空串表示该项不画。
    std::vector<std::string> tags;
    // 每项自己的渲染字体文件（绝对路径）。**不能再用 g_fonts[下标]**：字体下拉带
    // 「更多…」折叠，列表下标与 g_fonts 下标不是一回事。
    std::vector<std::string> paths;
};
static std::map<std::string, DdField> g_ddFields;  // 每帧重填（滚动/窗口变化都要跟）
static std::string g_ddOpenId;
static eui::Signal<float> g_ddScroll{0.0f};        // 弹层内部滚动位置
static std::set<std::string> g_ddMore;             // 点过「更多…」的下拉 id（字体表用）
static bool g_ddKeep = false;                      // onPick 里置真 = 别关菜单（「更多…」就地展开）

static void ddField(core::dsl::Ui& ui, const std::string& id, float px, float py, float w, float h,
                    std::vector<std::string> items, int sel, std::function<void(int)> onPick,
                    const std::string& placeholder = "", bool fonts = false, bool disabled = false,
                    std::vector<std::string> tags = {}, std::vector<std::string> paths = {}) {
    DdField f;
    f.id = id;
    f.ax = kNavW + px;
    f.ay = py - g_pageScroll.get();
    f.w = w;
    f.items = std::move(items);
    f.sel = sel;
    f.onPick = std::move(onPick);
    f.fonts = fonts;
    f.tags = std::move(tags);
    f.paths = std::move(paths);
    // 置灰的下拉不进登记表：登记了就能被弹层打开，"置灰还能展开"是最糟的一种态。
    // 位置仍每帧刷新（否则滚动后弹层会飘在旧位置）—— 这里只跳过"可展开"这件事本身。
    if (!disabled)
        g_ddFields[id] = f;
    else
        g_ddFields.erase(id);

    const bool open = !disabled && (g_ddOpenId == id);
    const std::string label = (sel >= 0 && sel < (int)f.items.size()) ? f.items[(size_t)sel] : placeholder;
    const auto& th = uiTheme();
    // 字体下拉收起的值也要用该字体渲染：选了「更纱黑体」却用默认字体显示它的名字，
    // 等于没提供预览 —— 这正是这一项存在的理由。
    std::string labelFont;
    if (fonts && sel >= 0 && sel < (int)f.paths.size()) labelFont = f.paths[(size_t)sel];
    const core::Color fg = disabled ? components::theme::withOpacity(th.text, 0.55f) : th.text;
    // 字体名长起来没边（`[内置] LXGWWenKaiMonoLite-Bold.ttf` 有 30 多字符），而 `ui.text` **不裁剪** ——
    // 超出宽度的部分会直接盖到右边的标签上（照片上就是"ttf代码字体"粘成一串）。
    // 其它下拉的值是版本号 / 语言名，长度可控，不为此多做一次量宽。
    const std::string shown = fonts ? wbFitHead(label, w - 40.0f, 13.0f) : label;
    ui.stack(id + ".wrap").position(px, py).size(w, h).zIndex(8)
        .content([&, id, w, h, shown, labelFont, open, disabled, fg] {
            ui.rect(id + ".f").size(w, h)
                .states(th.surface, disabled ? th.surface : th.surfaceHover,
                        disabled ? th.surface : th.surfaceActive)
                .radius(10.0f)
                .border(1.0f, components::theme::withOpacity(th.border, disabled ? 0.5f : 0.78f))
                .onClick([id, disabled] {
                    if (disabled) return;
                    g_ddOpenId = (g_ddOpenId == id) ? std::string() : id;
                    g_ddScroll.set(0.0f);  // 每次展开从第一项看起
                    requestUpdate();
                })
                .build();
            ui.text(id + ".l").x(12.0f).size(std::max(20.0f, w - 40.0f), h).text(shown)
                .fontFamily(labelFont).fontSize(13.0f).lineHeight(h).color(fg)
                .verticalAlign(core::VerticalAlign::Center).build();
            ui.text(id + ".c").x(std::max(0.0f, w - 26.0f)).size(18.0f, h)
                .icon(open ? 0xF077 : 0xF078).fontSize(11.0f).lineHeight(h)
                .color(disabled ? components::theme::withOpacity(th.primary, 0.45f) : th.primary)
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center).build();
        }).build();
}

// 弹层：在根级（ui.row 之后）绘制，位置取本帧记录的绝对坐标；下方放不下就翻到上方。
// 冒烟钩子 PXC_LAUNCHER_DEBUG_DD=<下拉 id>：启动后直接把某个下拉展开，便于自动截图核对
// （合成鼠标点击不可靠，会点到别的窗口上）。
static std::string g_ddBoot = [] {
    wchar_t b[64] = {0};
    if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_DD", b, 64) == 0) return std::string();
    return pxc_util::toUtf8(b);
}();

static void composeDropdownOverlay(core::dsl::Ui& ui, const eui::Screen& screen) {
    if (g_ddOpenId.empty() && !g_ddBoot.empty() && g_ddFields.count(g_ddBoot)) {
        g_ddOpenId = g_ddBoot;
        g_ddBoot.clear();
    }
    if (g_ddOpenId.empty()) return;
    auto it = g_ddFields.find(g_ddOpenId);
    if (it == g_ddFields.end() || it->second.items.empty()) { g_ddOpenId.clear(); return; }
    const DdField f = it->second;  // 拷一份：回调里会改 g_ddFields / g_ddOpenId
    const auto& th = uiTheme();
    const float itemH = 28.0f, inset = 6.0f, lineH = 12.0f;
    const int n = (int)f.items.size();
    // 字体名长（`[内置] LXGWWenKaiMonoLite-Bold.ttf` 折合 300 多像素），菜单按 200 宽画
    // 几乎每项都要截成省略号 —— 那这个列表就没法选了。字体下拉单独放宽。
    const float w = std::max(160.0f, f.fonts ? std::max(f.w, 360.0f) : f.w);
    // 字体表有三十多项，弹层必须能滚：高度封顶 + 内部 scrollView，
    // 否则列表会直接顶出屏幕（contextMenu 不支持滚动，所以这里自绘）。
    const float contentH = itemH * (float)n + inset * 2.0f;
    const float menuH = std::min(contentH, std::min(420.0f, screen.height - 48.0f));
    float y = f.ay + 42.0f;
    if (y + menuH > screen.height - 8.0f) y = std::max(8.0f, f.ay - menuH - 4.0f);
    const float x = std::min(f.ax, std::max(8.0f, screen.width - w - 8.0f));

    // 点空白关闭：透明背板铺满，压在弹层下面（zIndex 低一档）。
    ui.stack("dd.dismiss").size(screen.width, screen.height).zIndex(900).content([&] {
        ui.rect("dd.dismiss.bg").size(screen.width, screen.height)
            .states(components::theme::color(0.0f, 0.0f, 0.0f, 0.0f), components::theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                    components::theme::color(0.0f, 0.0f, 0.0f, 0.0f))
            .onClick([] {
                g_ddOpenId.clear();
                requestUpdate();
            })
            .build();
    }).build();

    ui.stack("dd.menu").position(x, y).size(w, menuH).zIndex(901).content([&, f, n, w, menuH, itemH, inset, lineH] {
        ui.rect("dd.menu.bg").size(w, menuH).color(th.surface)
            .radius(10.0f).border(1.0f, components::theme::withOpacity(th.border, 0.82f))
            .shadow(components::theme::popupShadow(th)).build();
        components::scrollView(ui, "dd.menu.scroll").size(w, menuH)
            .step(itemH).scrollbarWidth(6.0f).scrollbarGap(4.0f)
            .bind(g_ddScroll)
            .content([&, f, n, itemH, inset, lineH](eui::Ui& c, float cw, float ch) {
                const float h = itemH * (float)n + inset * 2.0f;
                c.stack("dd.menu.canvas").size(cw, h).content([&, f, n, itemH, inset, lineH, cw] {
                    for (int i = 0; i < n; ++i) {
                        const float iy = inset + (float)i * itemH;
                        const int idx = i;
                        c.rect("dd.menu.i." + std::to_string(i))
                            .x(inset).y(iy).size(std::max(0.0f, cw - inset * 2.0f), itemH)
                            .states(components::theme::color(0.0f, 0.0f, 0.0f, 0.0f), th.surfaceHover, th.surfaceActive)
                            .radius(6.0f).instantStates()
                            .onClick([f, idx] {
                                auto cur = g_ddFields.find(f.id);
                                if (cur != g_ddFields.end() && cur->second.onPick) cur->second.onPick(idx);
                                // 「更多…」是就地展开，不是选中：列表变了但菜单要留着，
                                // 否则用户刚点开"更多"就被关掉，等于点了个寂寞。
                                if (g_ddKeep) g_ddKeep = false;
                                else g_ddOpenId.clear();
                                requestUpdate();
                            })
                            .build();
                        // 右侧小字（「内置 / 系统」）先把宽度占掉，主文本再按剩下的宽度截。
                        const std::string tag = (i < (int)f.tags.size()) ? f.tags[(size_t)i] : std::string();
                        const float tagW = tag.empty() ? 0.0f : wbRealTextW(tag, 11.5f, 400) + 12.0f;
                        const float textW = std::max(40.0f, cw - inset * 2.0f - 16.0f - tagW);
                        c.text("dd.menu.t." + std::to_string(i))
                            .x(inset + 10.0f).y(iy).size(textW, itemH)
                            // 字体名会超出菜单宽度（`ui.text` 不裁剪，超出的字直接画到菜单外），
                            // 按实际宽度截一次；其它下拉的项短，不动。
                            .text(f.fonts ? wbFitHead(f.items[(size_t)i], textW, 14.0f)
                                          : f.items[(size_t)i])
                            .fontFamily(f.fonts && idx < (int)f.paths.size()
                                            ? f.paths[(size_t)idx]
                                            : std::string())
                            .fontSize(f.fonts ? 14.0f : 12.5f).lineHeight(itemH)
                            .color(i == f.sel ? th.primary : th.text)
                            .verticalAlign(core::VerticalAlign::Center).build();
                        if (!tag.empty())
                            c.text("dd.menu.tag." + std::to_string(i))
                                .x(inset + 10.0f + textW).y(iy).size(std::max(20.0f, tagW - 6.0f), itemH)
                                .text(tag).fontSize(11.5f).lineHeight(itemH)
                                .color(components::theme::withOpacity(th.text, 0.45f))
                                .horizontalAlign(core::HorizontalAlign::Right)
                                .verticalAlign(core::VerticalAlign::Center).build();
                    }
                }).build();
            })
            .build();
    }).build();
}

// ---------- 工程文件工作台（v3）：数据与动作 ----------
// 五页签的数据来源都不归启动器所有，所以这里的纪律是：别人的数据只读、列表先出、版本列后填。
static std::filesystem::path wbProjectsPath() { return pxc_nui::userDataDir() / "projects.json"; }
static std::filesystem::path wbCachePath() { return pxc_nui::userDataDir() / ".cache" / "detect_cache.json"; }
static std::filesystem::path wbRecentJson() { return g_wbUserDir / "recent.json"; }

// 版本列的字体分流：纯 ASCII（版本串）走等宽字体，否则（「识别中」「识别不出」「未知版本」）
// 交回默认中文字体 —— 等宽字体没有汉字，硬套会渲染成豆腐块。
// 路径必须是**绝对路径**：EUI 的 resolveFontPath 对带 "." 的 family 按文件找，但只在
// <exe>/assets 下找；我们的字体在 data/fonts，给相对文件名是找不到的（会静默退回默认字体，
// 界面上看不出来，只是小数点又被衬线吃掉）。
static std::string wbMonoFont() {
    static std::string cached;
    static bool tried = false;
    if (!tried) {
        tried = true;
        std::error_code ec;
        const std::filesystem::path f = fontsRoot() / wb::kVerMonoFile;
        if (std::filesystem::exists(f, ec)) cached = pxc_util::toUtf8(f.wstring().c_str());
    }
    return cached;
}

static std::string wbVerFontOf(const std::string& s) {
    for (unsigned char c : s)
        if (c >= 0x80) return std::string();
    return wbMonoFont();
}

static std::string wbFmtTime(std::int64_t unixSec) {
    if (unixSec <= 0) return "—";
    std::tm tm{};
    time_t t = (time_t)unixSec;
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof(b), "%Y/%m/%d %H:%M", &tm);
    return b;
}

// 粗估文本宽度：汉字/全角按 1em、ASCII 按 0.56em。
// 用于按内容算 pill / 页签宽度 —— 定宽会把「本机可启动」压成竖条（构建器的 7 段 segmented 就踩过）。
// EUI 的真实字形宽度（走 HarfBuzz 排版 + 自带字体，内部有按内容缓存的 TextSizeCache）。
// 只在需要"精确到像素"的地方用 —— 目前只有路径的点击区与悬浮下划线。
// 为什么不用 wbTextW：那是按 em 估的（ASCII 记 0.56em），纯 ASCII 路径会被高估三四成，
// 于是"点击区域 / 下划线比文字宽一截"，鼠标一悬停就能看出不对。
static float wbRealTextW(const std::string& s, float fs, int weight = 400) {
    if (s.empty()) return 0.0f;
    // 量的是**渲染时**的字号：dsl 的 fontSize() 会乘全局系数（基准 1.4 × 用户倍率），
    // 这里不跟着乘，量出来的宽度就比实际窄一大截 —— 页签下划线短一截、点击区盖不住字。
    // 这条在 100% 时是 1.4 倍的误差，尤其明显。
    const float scaled = fs * textScale();
    core::TextStyle st;
    st.text = s;
    st.fontSize = scaled;
    st.fontWeight = weight;
    st.lineHeight = scaled * 1.4f;
    return core::TextPrimitive::measureTextSize(st).x;
}

static float wbTextW(const std::string& s, float fs) {
    // 与 wbRealTextW 同一个口径：量"渲染后"的宽度，所以先乘全局字号系数（基准 1.4 × 用户倍率）。
    // 这里全给 em 比例（ASCII 0.56em）本来就是估，再漏乘系数就是"估小了 40%"——
    // 页签 chip、面包屑、按宽裁字全都按它算，漏乘就会文字撑出圆角底。
    fs *= textScale();
    float w = 0.0f;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { w += fs * 0.56f; i += 1; }
        else if (c < 0xE0) { w += fs * 0.32f; i += 2; }
        else if (c < 0xF0) { w += fs * 1.00f; i += 3; }
        else { w += fs * 1.00f; i += 4; }
    }
    return w;
}

// 注：A1 最早用内联 SVG（ui.svg().source）画漏斗 / 刷新箭头，现已改为 EUI 自带的
// Font Awesome 7 Free 图标字体（ui.text().icon(码位)）。换掉的两个理由：
//   1. 不必再拼 SVG 字符串 —— 原始字符串 R"(...)" 里的 )" 是闭合符，会把紧跟的 fill="
//      的开引号吃掉，生成 fill=#RRGGBB" 这种残串，nsvgParse 静默失败、按钮一片空白（踩过）；
//   2. 图标字体跟着字号走，和同排文字同高，不用再手算尺寸。
static std::vector<std::string> wbRegVersions() {
    std::vector<std::string> v;
    for (const auto& e : g_cfg.registry)
        if (!e.product_version.empty()) v.push_back(e.product_version);
    return v;
}

static void wbBoot() {
    if (g_wbBooted) return;
    g_wbBooted = true;
    g_wbUserDir = pxc_index::resolveUserDir();
    bool ok = false;
    g_wbStore = pxc_index::loadStore(wbProjectsPath(), &ok);
    g_wbCache.load(wbCachePath());
    for (int i = 0; i < pxc_index::kTabCount; ++i) g_wbSort[i] = pxc_index::defaultSort(i);
    g_wbAssoc = pxc_assoc::query();
    g_wbAssocAt = (double)pxc_index::nowUnix();
    pxc_log::write("INFO", "Workbench",
                   "用户目录=" + g_wbUserDir.u8string() + " | 识别缓存 " +
                       std::to_string(g_wbCache.size()) + " 条 | 历史 " +
                       std::to_string(g_wbStore.history.size()) + " | 导入 " +
                       std::to_string(g_wbStore.imported.size()) + " | 收藏 " +
                       std::to_string(g_wbStore.favorites.size()));
}

static void wbSaveSoon() { g_wbStoreDirty = true; }
static void wbSaveStoreNow() {
    pxc_index::saveStore(wbProjectsPath(), g_wbStore);
    g_wbStoreDirty = false;
}

// 缓存里已经有识别结果的，直接套上（第二次打开秒出版本列）。
// 命中缓存的公共动作。版本串读出来再规范化一次：detect_cache.json 里可能存着
// 规范化之前写进去的三段版本（1.20.1），补成四段才不会让卡片和筛选各说一套。
static bool wbTakeCached(pxc_index::Item& it) {
    pxc_index::CacheEntry ce;
    if (!g_wbCache.get(it.path, it.size, it.mtime, ce)) return false;
    it.detected = true;
    it.version = pxc_reg::normalizeVersion(ce.version);
    it.format = ce.format;
    it.conf = ce.conf;
    return true;
}

static void wbApplyCached(pxc_index::Item& it) {
    if (it.folder || it.detected) return;
    wbTakeCached(it);
}

// 真识别一次（均值 9.5ms，最大 ~40ms）。结果同时进缓存。
static void wbDetectItem(pxc_index::Item& it) {
    if (it.folder || it.detected) return;
    if (wbTakeCached(it)) return;
    pxc_index::CacheEntry ce;
    const pxc::DetectionResult r = pxc::detectPxc(pxc_util::pathFromUtf8(it.path));
    pxc_index::applyDetect(it, r);
    ce.size = it.size;
    ce.mtime = it.mtime;
    ce.version = it.version;
    ce.format = it.format;
    ce.conf = it.conf;
    g_wbCache.set(it.path, ce);
}

// 行号 -> 条目。v4 的 rows 就是 all 的下标，没有"子行"这种第二形态了：
// 进入文件夹是把 all 换成下一层，不是往列表里插行。
static pxc_index::Item* wbItemAt(int row) {
    if (row < 0 || row >= (int)g_wb.rows.size()) return nullptr;
    const int idx = g_wb.rows[(size_t)row];
    if (idx < 0 || idx >= (int)g_wb.all.size()) return nullptr;
    return &g_wb.all[(size_t)idx];
}

static void wbBuildRows() {
    const std::string q = pxc_index::foldQuery(g_wbQuery.get());
    const auto regs = wbRegVersions();
    g_wb.rows.clear();
    g_wb.rows.reserve(g_wb.all.size());
    for (int i = 0; i < (int)g_wb.all.size(); ++i) {
        const auto& it = g_wb.all[(size_t)i];
        if (!pxc_index::matchQuery(it, q)) continue;
        if (!pxc_index::passFilter(it, g_wb.filter, regs, pxc_index::isFavorite(g_wbStore, it.path)))
            continue;
        g_wb.rows.push_back(i);
    }
    pxc_index::sortIndices(g_wb.rows, g_wb.all, g_wbSort[g_wb.tab]);
    g_wbAppliedQuery = q;
}

// 滚动复位要换「代次 id」：virtualList 一旦 initialized，组件再传 offset(0) 就不生效了
// （运行时被 syncOwnedScrollState 接管），列表会「标题在、内容跑出可视区」。
static void wbResetScroll() {
    ++g_wbListGen;
    g_wbScroll.set(0.0f);
}

static void wbUpdateStatus();

// 素材里的 PXC 图标：SVG **文本**一次读进内存（`ui.svg().source()` 要的是标记本身，不是路径）。
// 只在第一次进工作台读一次盘 —— dslAppConfig 每帧被调 20+ 次，那里读盘会炸（见 §7.5 的教训）。
static void wbLoadIcon() {
    if (g_wb.iconTried) return;
    g_wb.iconTried = true;
    const std::filesystem::path p = dataDir() / "icons" / "pxc.svg";
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return;
    std::string txt;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) txt.append(buf, n);
    fclose(f);
    if (!txt.empty() && txt.size() < 512u * 1024u) g_wb.iconSvg = std::move(txt);
}

// 把当前层的识别结果存回层缓存（切层 / 刷新前调用）。
static void wbSaveLayer() { g_wb.layers[g_wb.dir] = g_wb.all; }

// 换层：命中缓存就直接换，没命中就扫一层（子文件夹 + .pxc，不递归）。
static void wbLoadLayer(const std::string& dirUtf8) {
    g_wb.dir = dirUtf8;
    auto it = g_wb.layers.find(dirUtf8);
    if (it == g_wb.layers.end()) {
        if (dirUtf8.empty()) {   // 根层没缓存 = 还没加载过页签
            g_wb.all.clear();
            g_wb.rows.clear();
            wbUpdateStatus();
            return;
        }
        std::vector<pxc_index::Item> kids = pxc_index::scanLayer(pxc_util::pathFromUtf8(dirUtf8));
        for (auto& k : kids) wbApplyCached(k);
        it = g_wb.layers.emplace(dirUtf8, std::move(kids)).first;
    }
    g_wb.all = it->second;
    wbBuildRows();
    wbResetScroll();
    wbUpdateStatus();
}

// 根层条目（按页签）。「全部」= 工程库 ∪ 启动历史，按路径去重，**不标来源**（用户拍板）。
static std::vector<pxc_index::Item> wbRootItems(int tab) {
    std::vector<pxc_index::Item> out;
    auto pushFile = [&](const std::string& pathUtf8) {
        pxc_index::Item it;
        if (pxc_index::statPath(pxc_util::pathFromUtf8(pathUtf8), it)) out.push_back(std::move(it));
    };
    switch ((pxc_index::Tab)tab) {
        case pxc_index::Tab::Recent: {
            std::vector<std::string> missing;
            out = pxc_index::readRecent(g_wbUserDir, &missing);
            for (const auto& m : missing) pxc_index::hideRecent(g_wbStore, m);
            if (!missing.empty()) wbSaveSoon();
            // 失效路径与用户手动隐藏的都不显示（§7.8：直接不显示，不置灰占位）
            out.erase(std::remove_if(out.begin(), out.end(),
                                     [](const pxc_index::Item& it) {
                                         return std::find(g_wbStore.recentHidden.begin(),
                                                          g_wbStore.recentHidden.end(),
                                                          it.path) != g_wbStore.recentHidden.end();
                                     }),
                      out.end());
            g_wb.emptyText = "PXC 还没有近期记录。用它打开过一次工程后就会出现在这里。";
            break;
        }
        case pxc_index::Tab::All: {
            // 两边的数组都是插表头（最近在前），所以先 history 后 imported 就是"最近活动在前"。
            std::set<std::string> seen;
            for (const auto& h : g_wbStore.history)
                if (seen.insert(h.path).second) pushFile(h.path);
            for (const auto& im : g_wbStore.imported)
                if (seen.insert(im.path).second) pushFile(im.path);
            g_wb.emptyText = "还没有内容。用启动器打开一个工程，或把工程加入工程库。";
            break;
        }
        case pxc_index::Tab::Library:
            for (const auto& im : g_wbStore.imported) {
                pxc_index::Item it;
                if (!pxc_index::statPath(pxc_util::pathFromUtf8(im.path), it)) continue;
                it.addedAt = im.addedAt;
                out.push_back(std::move(it));
            }
            g_wb.emptyText = "点右上「导入工程文件夹 / 导入 PXC 文件」，或在「近期文件」里右键加入。";
            break;
        case pxc_index::Tab::History:
            for (const auto& h : g_wbStore.history) pushFile(h.path);
            g_wb.emptyText = "还没通过启动器打开过工程。";
            break;
        case pxc_index::Tab::Favorite:
            for (const auto& f : g_wbStore.favorites) pushFile(f.path);
            g_wb.emptyText = "点行左侧的星标就能收藏。";
            break;
        default:
            // 自动恢复：不折叠、不去重（用户找的是"内容对得上"的那份，未必是最新那份）。
            out = pxc_index::scanPxcDir(g_wbUserDir / "Autosave", false);
            g_wb.emptyText = "没有自动保存文件。";
            break;
    }
    return out;
}

static void wbLoadTab(int tab, bool force) {
    wbBoot();
    if (!force && g_wb.loaded[tab] && g_wb.tab == tab) return;
    wbLoadIcon();
    g_wb.tab = tab;
    g_wb.layers.clear();
    g_wb.dir.clear();
    g_wb.crumbs.clear();
    g_wb.all = wbRootItems(tab);
    for (auto& it : g_wb.all) wbApplyCached(it);
    g_wb.layers[""] = g_wb.all;
    g_wb.loaded[tab] = true;
    wbBuildRows();
    wbResetScroll();
    wbUpdateStatus();
}

static void wbSetTab(int tab) {
    if (tab < 0 || tab >= pxc_index::kTabCount) return;
    wbLoadTab(tab, false);
    requestUpdate();
}

static void wbRefresh() {
    wbLoadTab(g_wb.tab, true);
    requestUpdate();
}

static void wbRequery() {
    // 搜索一有内容就退出进入态：打字是为了"找东西"，不是为了"在这一层找"。
    // 范围悄悄缩成一层，最常见的后果是"我明明有这个东西，怎么搜不到"。
    if (!g_wb.dir.empty() && !pxc_index::foldQuery(g_wbQuery.get()).empty()) {
        wbSaveLayer();
        g_wb.crumbs.clear();
        wbLoadLayer("");
    }
    wbBuildRows();
    wbResetScroll();
    wbUpdateStatus();
    requestUpdate();
}

static void wbCycleSort(pxc_index::SortKey key) {
    auto& s = g_wbSort[g_wb.tab];
    if (s.key == key) s.asc = !s.asc;
    else {
        s.key = key;
        s.asc = (key != pxc_index::SortKey::Mtime);   // 时间默认"最近在前"
    }
    wbBuildRows();
    wbResetScroll();
    wbUpdateStatus();
    requestUpdate();
}

// （A2：版本 / 修改时间 拆成两枚独立排序 chip，由 wbCycleSort 分别处理；旧的跨列轮换 wbCycleVerTime 已删除）

static void wbUpdateStatus() {
    const int total = (int)g_wb.rows.size();
    const int loaded = (int)g_wb.all.size();
    int pending = 0;
    for (int idx : g_wb.rows) {
        const auto& it = g_wb.all[(size_t)idx];
        if (!it.folder && !it.detected) ++pending;
    }
    std::string s = "共 " + std::to_string(total) + " 个";
    // 被搜索/筛选藏起来的数目要写出来：否则用户看到"共 3 个"会以为数据丢了。
    if (total != loaded)
        s += "（这一层 " + std::to_string(loaded) + " 个，筛掉 " + std::to_string(loaded - total) + " 个）";
    if (pending) s += " · " + std::to_string(pending) + " 个版本待识别";
    if (g_wbSort[g_wb.tab].key != pxc_index::SortKey::None)
        s += "  ·  " + std::string(pxc_index::sortLabel(g_wbSort[g_wb.tab]));
    g_wb.status = s;
}

// 懒识别：每帧最多 12ms。按**时间**限流而不是按个数 —— 单个文件 12KB 到 474KB 差 30 倍，
// 固定"每帧 4 个"在遇到大文件时会突然出现 40ms 的帧，滚轮一卡一卡。
static void wbTickDetect() {
    if (g_wb.all.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    int done = 0;
    // 识别必须遍历**整层**（g_wb.all），不能只遍历 g_wb.rows：
    // 筛选一旦生效，当前"不合格"的条目就不在 rows 里了。若只扫 rows，它们永远不会被识别，
    // 就一直停在「识别中」；于是「只看识别不出」永远筛出 0 条 —— 死循环。
    // 已识别的会被 continue 跳过，所以每帧从 0 重扫也不会慢。
    for (size_t i = 0; i < g_wb.all.size(); ++i) {
        pxc_index::Item& it = g_wb.all[i];
        if (it.folder || it.detected) continue;
        wbDetectItem(it);
        ++done;
        const auto used = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        if (used >= 12000) break;
    }
    if (done) {
        // 识别结果改变条目在「状态」这一维上的归属，所以有筛选时必须重算列表，
        // 否则列表停在"全部还是识别中"那次构建上（表现为筛选不生效、条数不涨）。
        if (g_wb.filter.active()) wbBuildRows();
        // 识别出结果就重算一次状态行 —— 否则「N 个版本待识别」会一直停在建列表那刻的数字，
        // 版本列都已经填满了它还在说待识别。
        wbUpdateStatus();
        // 本函数只在 compose 里被调，compose 又只在 requestUpdate 后重跑。
        // 不自己续帧的话，鼠标不动 = 不再重绘 = 识别停在半路（实测 4 秒后还剩 16 个没识别）。
        requestUpdate();
    }
}

static void wbTick() {
    static auto lastStoreSave = std::chrono::steady_clock::now();
    static auto lastCacheSave = std::chrono::steady_clock::now();
    static auto lastAssoc = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (g_wbStoreDirty &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStoreSave).count() > 500) {
        wbSaveStoreNow();
        lastStoreSave = now;
    }
    if (g_wbCache.dirty() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCacheSave).count() > 5000) {
        g_wbCache.save(wbCachePath());
        lastCacheSave = now;
    }
    // 关联状态：注册表读很便宜，但没必要每帧读，2 秒一次。
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAssoc).count() > 2000) {
        g_wbAssoc = pxc_assoc::query();
        lastAssoc = now;
    }
}

// ---- 启动 ----
static void wbReportStart(const pxc_project::StartOutcome& out, const std::string& path) {
    if (out.result == pxc_project::StartResult::NoVersion) {
        openVersionPicker(pxc_util::pathFromUtf8(path));
        return;
    }
    g_lastLaunch = out.message;
    g_wb.status = out.message;
    if (out.result == pxc_project::StartResult::Ok) {
        pxc_index::noteLaunch(g_wbStore, path, out.detection.version, out.exeVersion,
                              formatStr(out.detection.format), confidenceStr(out.detection.confidence));
        wbSaveSoon();
        g_wb.loaded[(int)pxc_index::Tab::All] = false;   // 「全部」页签要用到新历史
    }
    requestUpdate();
}

// worker 的结果落到主线程：改状态、记历史、**同步写盘**。
//
// 三条都必须在窗口过程里做，不能等下一帧：
//   ① 最小化态没有下一帧；② wbSaveSoon() 的 500ms 节流靠后续帧触发，等不到就永远不落盘；
//   ③ 这些字段（g_wbStore / g_wb.status）是主线程状态，worker 直接写会撕裂。
// 前置声明：wbProcessInbox 要调 wbLaunchPath / wbImportPaths，而它们定义在本文件更靠后。
static void wbLaunchPath(const std::string& pathUtf8, const std::string& version,
                         const std::string& format, const std::string& conf);
// 处理待启动的路径（第二实例转发 / 命令行 / 拖放共用一个入口）。
// 为什么从 compose 里提出来：主窗最小化后 compose 不跑，而转发恰恰在那时到达 ——
// 留在 wbTick 里就等于"第二次双击永远没反应"（docs/05 §9.8.3 ①）。
static void wbProcessInbox() {
    if (g_wbInbox.empty()) return;
    std::vector<std::filesystem::path> paths = g_wbInbox;
    g_wbInbox.clear();
    std::vector<std::filesystem::path> imports;
    for (const auto& p : paths) {
        std::error_code ec;
        if (isPxcFile(p)) {
            const pxc::DetectionResult d = pxc::detectPxc(p);
            wbLaunchPath(p.u8string(), d.version, formatStr(d.format), confidenceStr(d.confidence));
        } else if (std::filesystem::is_directory(p, ec)) {
            imports.push_back(p);
        }
    }
    if (!imports.empty()) wbImportPaths(imports, false);
}

static void onLaunchDone();

// 启动前自动生成的结果：worker 里算出来，主线程取一次变成 toast（docs/08 §8.2.5）。
// 为什么不让 worker 直接 toast：worker 不碰 UI 状态是硬纪律（launch_runner.h 的边界说明）。
struct AutoGenMsg {
    bool has = false;
    std::string title, msg;
    bool error = false;
};
static std::mutex g_agMu;
static AutoGenMsg g_agMsg;
static void postAutoGenMsg(const std::string& title, const std::string& msg, bool error) {
    std::lock_guard<std::mutex> lk(g_agMu);
    g_agMsg.has = true;
    g_agMsg.title = title;
    g_agMsg.msg = msg;
    g_agMsg.error = error;
}
static AutoGenMsg takeAutoGenMsg() {
    std::lock_guard<std::mutex> lk(g_agMu);
    AutoGenMsg m = g_agMsg;
    g_agMsg = AutoGenMsg{};
    return m;
}

// 主窗窗口过程收到唤醒消息：先接单，再收结果。
// 这个函数跑在窗口过程中（主线程），不在 compose 内 —— 所以它必须做完所有事，
// 不能"等下一帧"。最小化时根本没有下一帧。
static void wbOnWake() {
    wbProcessInbox();
    onLaunchDone();
}

static void onLaunchDone() {
    // 一次醒来看**全部**结果：连开两个工程时两个任务会先后结束，
    // 只取一个的话另一个的"记启动历史 / 自动生成结论"就丢了（结果也早已改成队列）。
    bool any = false;
    for (;;) {
        pxc_runner::Result r;
        if (!pxc_runner::takeResult(r)) break;
        any = true;

        pxc_log::write("INFO", "Launch", "启动结果 | " + r.message);
        g_wb.status = r.message;
        g_lastLaunch = r.message;

        // 启动前自动生成的结论（有才提示）：失败照常启动，所以这里只是"多说一句"。
        {
            const AutoGenMsg m = takeAutoGenMsg();
            if (m.has) showToast(m.title, m.msg, m.error);
        }

        if (r.kind == pxc_runner::Kind::NeedsPicker) {
            // worker 不替用户决定用哪个版本 —— 把窗口还给用户，弹协商浮窗。
            // 提窗口与"静默启动"收尾都由 openVersionPicker 自己做（一个地方负责一件事）。
            openVersionPicker(pxc_util::pathFromUtf8(r.path));
            return;
        }

        // r.path 空 = 版本库卡片那种空启动，没有工程可记。
        if ((r.kind == pxc_runner::Kind::OkReuse || r.kind == pxc_runner::Kind::OkNew) &&
            !r.path.empty()) {
            pxc_index::noteLaunch(g_wbStore, r.path, "", "", "", "");
            g_wb.loaded[(int)pxc_index::Tab::All] = false;
            g_wb.loaded[(int)pxc_index::Tab::History] = false;
        }
    }
    if (!any) return;
    // 同步落盘：这里没有下一帧可用。失败也照写 —— 上面可能已经改了别的待存内容。
    wbSaveStoreNow();
    if (g_wbCache.dirty()) g_wbCache.save(wbCachePath());
    requestUpdate();
}

// 启动工程前把版本弄新鲜（§11.3 的第三处触发）。分两级，别一上来就全量：
//   ① 只刷"版本号等于目标版本"的那几条 —— 抓的是"这几条其实已经被换掉了"（Steam 更新）；
//   ② 还是没匹配上，说明磁盘上可能刚出现一个**没登记**的版本（刚装 / Steam 刚落完更新），
//      这时才值得全量：Steam 重发现 + 便携扫描 + 每条都重读。
static std::vector<pxc_store::ExeEntry> freshMatches(const std::string& version) {
    if (version.empty()) {
        refreshRegistry("启动工程前");   // 没版本号 → 候选要全，直接全量
        return {};
    }
    for (const auto& h : pxc_reg::findMatching(g_cfg.registry, version))
        refreshRegistryEntry(h.exe_path, "启动工程前");
    auto now = pxc_reg::findMatching(g_cfg.registry, version);
    if (!now.empty()) return now;
    refreshRegistry("启动工程前");
    return pxc_reg::findMatching(g_cfg.registry, version);
}

// 组 job 并交给 worker。工程双击（Direct）与缺版本协商（Direct / Copy）两条路共用 ——
// 差别只有 mode 一个参数：Copy 多一步"先复制再打开副本"，原文件全程只读。
//
// 为什么抽成一个函数：这两条路以前各写一套，协商那条还同步在主线程跑（没有提示窗、
// 不进队列、点了取消也没处响应）。分叉久了必然出现"从列表启动有提示、从协商窗启动没有"
// 这种不一致，而它们本该是同一件事。
static void submitProjectLaunch(const std::filesystem::path& p, const std::string& exe,
                                const std::string& version, pxc_project::StartMode mode) {
    const std::string pathUtf8 = p.u8string();

    pxc_runner::Job job;
    job.pxcPath = p;
    job.exePath = pxc_util::pathFromUtf8(exe);
    job.recentFile = wbRecentJson();
    job.version = version;
    job.fileName = pxc_stage::fileNameOf(pathUtf8);
    // Copy 模式**也复用**：副本先建出来，复用打开的就是副本，原文件全程只读。
    // （旧判断是"副本模式不复用"，理由是"复用会让那个实例打开原文件" —— 那个理由是错的：
    //  复用链拿到哪个路径就打开哪个路径。代价是跨版本每次都要多开一个 PXC 进程，
    //  而用户明明已经开着同版本的窗口 —— 2026-11-04 用户实测到的就是这个。）
    job.allowReuse = g_wbStore.assoc.reuseInstance;

    // 同版本可能登记了多份（Steam + 便携；手动登记还会再多）。复用只认"同一个 exe 路径"，
    // 而用户正在跑的那份未必是偏好选中的那份 —— 把其余候选一并带进 job，让 worker 谁在跑就用谁。
    // 不带的话后果很具体：偏好指向 A、实际跑的是 B → 判成"没有实例" → 静默再开一个 B。
    if (job.allowReuse && !version.empty()) {
        for (const auto& e : pxc_reg::findMatching(g_cfg.registry, version)) {
            if (e.exe_path.empty() || pxc_reg::samePath(e.exe_path, exe)) continue;
            job.altExes.push_back(pxc_util::pathFromUtf8(e.exe_path));
        }
        if (!job.altExes.empty())
            pxc_log::write("INFO", "Launch", "同版本另有 " + std::to_string(job.altExes.size()) +
                                                 " 份安装，一并作为复用候选");
    }

    // 真正拉起交给 pxc_project::startProject：跨版本建副本、备份、ExeMissing 判定都在里面，
    // 绕过去就等于绕过"不写坏原文件"的唯一屏障。
    const std::string verCopy = version;
    const std::string exeCopy = exe;
    const std::string pathCopy = pathUtf8;

    // Copy 模式：副本要在**复用之前**建出来 —— 复用链要打开的就是副本。
    // 建副本只有一处实现（`pxc_project::createCopy`，headless 测试直接覆盖它），这里只负责
    // 把它接到 worker 的阶段上报与取消上；删不删由 worker 按"最终用没用上"决定。
    if (mode == pxc_project::StartMode::Copy) {
        job.buildCopy = [pathCopy, verCopy](const pxc_runner::SpawnHooks& h,
                                            std::filesystem::path& outCopy, std::string& detail,
                                            bool& canceled) -> bool {
            // 报 Copy 阶段：提示窗上「取消」此时是可点的，用户能在复制开始前就停下。
            if (h.stage) h.stage(pxc_stage::Stage::Copy);
            if (h.canceled && h.canceled()) {
                canceled = true;
                detail = "已取消（未创建副本）";
                return false;
            }
            std::string err;
            const std::filesystem::path src = pxc_util::pathFromUtf8(pathCopy);
            const std::filesystem::path dst = pxc_project::createCopy(src, verCopy, err);
            if (dst.empty()) {
                detail = "创建副本失败（" + err + "）：" + src.parent_path().u8string();
                pxc_log::write("ERROR", "Project", detail);
                return false;
            }
            pxc_log::write("INFO", "Project", "已建副本（供复用）: " + dst.u8string());
            outCopy = dst;
            return true;
        };
    }

    // 启动前自动生成（docs/08 §8.2.5）：请求在**主线程**组好再按值带进 worker ——
    // worker 不碰 UI 状态是硬纪律。只在新起进程那条路上做（复用已有实例不写盘：
    // 语言包只在进程启动时读一次，复用时写了也没人读）。
    pxc_autogen::Request genReq;
    const bool genAuto = buildAutoGenRequest(genReq);

    job.spawn = [verCopy, exeCopy, pathCopy, genAuto, genReq, mode](
                    const pxc_runner::SpawnHooks& h,
                    const std::filesystem::path& prebuiltCopy) -> pxc_runner::SpawnOutcome {
        pxc_runner::SpawnOutcome so;
        // 先写语言包、再拉进程：PXC 只在启动时读一次语言目录，顺序反了这次就白改。
        // 任何失败都不拦启动 —— 汉化不对是次要问题，拦住启动是灾难。
        if (genAuto) {
            const auto gr = pxc_autogen::beforeLaunch(genReq, pxc_util::pathFromUtf8(exeCopy), verCopy);
            if (gr.ran && !gr.ok) {
                postAutoGenMsg("启动前生成失败", gr.message + "\n照常启动，这次用的还是旧语言文件", true);
            } else if (gr.ran && gr.ok && !gr.skipped && !gr.base.version.empty() &&
                       !pxc_reg::sameVersion(gr.base.version, verCopy)) {
                postAutoGenMsg("骨架版本对不上",
                               "目标 " + verCopy + " 取不到骨架，用了 " + gr.base.version +
                                   "，端口可能对不上",
                               false);
            }
        }
        pxc_project::StartRequest sr;
        sr.pxcPath = pxc_util::pathFromUtf8(pathCopy);
        sr.targetVersion = verCopy;
        sr.exePath = exeCopy;
        sr.mode = mode;
        // 非空 = worker 已经建好副本（Copy 模式先建再复用），这里别再复制一份。
        sr.prebuiltCopy = prebuiltCopy;
        // 取消：建副本 / 备份期间问一句；拉起之后 startProject 不再问（那已经不可撤销）。
        sr.canceled = [&h] { return h.canceled && h.canceled(); };
        sr.onStep = [&h](pxc_project::Step st) {
            if (!h.stage) return;
            switch (st) {
            case pxc_project::Step::Copy: h.stage(pxc_stage::Stage::Copy); break;
            case pxc_project::Step::Backup: h.stage(pxc_stage::Stage::Backup); break;
            case pxc_project::Step::Launch: h.stage(pxc_stage::Stage::Spawn); break;
            }
        };
        const auto out = pxc_project::startProject(g_cfg.registry, g_cfg.backup, backupDirPath(), sr);
        if (out.result == pxc_project::StartResult::Ok) {
            so.ok = true;
            so.pid = out.pid;
            return so;
        }
        if (out.result == pxc_project::StartResult::NoVersion) {
            so.needsPicker = true;
            return so;
        }
        if (out.result == pxc_project::StartResult::Canceled) {
            // 副本已经由 startProject 自己删掉（它建的它清），这里只管终态。
            so.canceled = true;
            so.detail = out.message;
            return so;
        }
        so.detail = out.message;
        return so;
    };

    // 排队是正常路径：连开两个工程时第二个要等第一个把 PXC 窗口等出来（否则两个进程抢 autosave）。
    // 状态行与时序都按"收下/开跑"分开写，别让用户以为点丢了。
    const bool queued = pxc_runner::submit(job);
    g_wb.status = queued ? ("已排队：" + job.fileName + "（等前一个启动完成）") : "正在启动…";
    requestUpdate();
}

static void wbLaunchPath(const std::string& pathUtf8, const std::string& version,
                         const std::string& format, const std::string& conf) {
    const std::filesystem::path p = pxc_util::pathFromUtf8(pathUtf8);

    // 版本号是跨版本判定的**输入**，用它之前必须是新鲜的（§11.3 的第三处触发）。
    const auto matches = freshMatches(version);
    if (matches.empty()) {
        openVersionPicker(p, /*registryFresh=*/true);   // freshMatches 里已经刷过了
        return;
    }
    // 同版本多条（跨渠道各一张）时按偏好挑：偏好命中的渠道 → Steam（地址稳定）→ 第一条。
    const std::size_t mi = pxc_reg::choosePreferred(matches, g_cfg.prefer, version);
    const std::string exe = matches[mi].exe_path;
    const std::string ver = matches[mi].product_version.empty() ? version : matches[mi].product_version;

    // 同 exe 已经在跑 → 复用它的进程（新增标签），不另开一个。
    // 复用不做备份：exe 与文件版本一致，不存在"跨版本重写"的风险。
    //
    // ---- 交给 worker：复用 → （失败就问用户）→ 首启 → 等主窗，全程右下角滚阶段 ----
    //
    // 为什么不在主线程同步做（这里以前是同步复用 + 直接 startProject）：
    //   · 复用要阻塞 3–11 秒，塞进窗口过程会丢第二实例的转发（3 秒同步消息超时）；
    //   · 主窗最小化后 compose 不跑，挂在帧上的状态机必死（docs/05 §9.1）。
    submitProjectLaunch(p, exe, ver, pxc_project::StartMode::Direct);
}

static void wbLaunchRow(int row);

static void wbToggleStar(const std::string& pathUtf8) {
    const bool fav = pxc_index::toggleFavorite(g_wbStore, pathUtf8);
    g_wb.status = fav ? "已收藏。" : "已取消收藏。";
    wbSaveSoon();
    if (g_wb.tab == (int)pxc_index::Tab::Favorite) wbLoadTab(g_wb.tab, true);
    requestUpdate();
}

// v4：双击文件夹 = **进入**（把 all 换成下一层），不再就地展开。
// 展开式在多层目录下会撑成一条长垂直带，而且"我在哪一层"没有位置感。
static void wbEnter(int row) {
    pxc_index::Item* ip = wbItemAt(row);
    if (!ip || !ip->folder) return;
    const std::string path = ip->path;
    const std::string name = ip->name;
    wbSaveLayer();
    g_wb.crumbs.emplace_back(name, path);
    wbLoadLayer(path);
    g_wbSel = -1;
    requestUpdate();
}

// 「← 返回」：回到上一层；已在根层时什么都不做（不退出页签 —— 误触把人弹到别处更烦）。
static void wbUp() {
    if (g_wb.dir.empty()) return;
    wbSaveLayer();
    if (!g_wb.crumbs.empty()) g_wb.crumbs.pop_back();
    wbLoadLayer(g_wb.crumbs.empty() ? std::string() : g_wb.crumbs.back().second);
    g_wbSel = -1;
    requestUpdate();
}

// 面包屑里点第 seg 段（seg = -1 是根段 = 页签名）
static void wbCrumbTo(int seg) {
    if (seg < -1 || seg >= (int)g_wb.crumbs.size()) return;
    if (seg == (int)g_wb.crumbs.size() - 1) return;   // 已经在这一层
    wbSaveLayer();
    g_wb.crumbs.resize((size_t)(seg + 1));
    wbLoadLayer(g_wb.crumbs.empty() ? std::string() : g_wb.crumbs.back().second);
    g_wbSel = -1;
    requestUpdate();
}

static bool wbInLibrary(const std::string& path) {
    return std::any_of(g_wbStore.imported.begin(), g_wbStore.imported.end(),
                       [&](const pxc_index::ImportEntry& e) { return e.path == path; });
}

// 加入工程库：wholeFolder 时加"它所在的文件夹"，否则加它自己（文件夹行加的就是它自己）。
// **不切页签** —— 用户是在「近期文件」里顺手归档，把他弹到「工程库」等于抢走当前位置。
static void wbAddToLibrary(const pxc_index::Item& it, bool wholeFolder) {
    const std::string target = wholeFolder ? it.parentDir : it.path;
    if (target.empty()) return;
    const bool folder = wholeFolder || it.folder;
    pxc_index::addImported(g_wbStore, target, folder);
    wbSaveSoon();
    g_wb.loaded[(int)pxc_index::Tab::Library] = false;
    g_wb.loaded[(int)pxc_index::Tab::All] = false;
    g_wb.status = (wholeFolder ? "已把所在文件夹加入工程库：" : "已加入工程库：") + target;
    requestUpdate();
}

static void wbCopyToClipboard(const std::string& utf8) {
    if (utf8.empty()) return;
    const std::wstring w = pxc_util::pathFromUtf8(utf8).wstring();
    if (!OpenClipboard((HWND)pxc_nui::mainWindow())) return;
    EmptyClipboard();
    const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* dst = GlobalLock(h)) {
            memcpy(dst, w.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
    }
    CloseClipboard();
    g_wb.status = "已复制路径：" + utf8;
    requestUpdate();
}

// 用默认浏览器打开一个 http(s) 链接（ShellExecute "open" + URL）。
static void wbOpenUrl(const std::string& url) {
    if (url.empty()) return;
    const std::wstring w = pxc_util::pathFromUtf8(url).wstring();
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void wbOpenFolderOf(const pxc_index::Item& it) {
    pxc_nui::revealInExplorer(pxc_util::pathFromUtf8(it.path), !it.folder);
}

static void wbRowActivate(int row) {
    pxc_index::Item* ip = wbItemAt(row);
    if (!ip) return;
    if (ip->folder) { wbEnter(row); return; }   // 双击文件夹 = 进入
    if (!ip->detected) wbDetectItem(*ip);        // 点启动时补一次同步识别（均值 9.5ms，感知不到）
    wbLaunchPath(ip->path, ip->version, ip->format, pxc_index::confLabel(ip->conf));
}

static void wbLaunchRow(int row) { wbRowActivate(row); }

// 双击：EUI 没有 onDoubleClick，用"同一行 400ms 内两次点击"自己判。
static int g_wbLastClickRow = -1;
static double g_wbLastClickMs = 0.0;
static double wbNowMs() {
    return (double)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
static void wbRowClicked(int row) {
    g_wbSel = row;
    const double now = wbNowMs();
    const bool dbl = (row == g_wbLastClickRow) && (now - g_wbLastClickMs < 400.0);
    g_wbLastClickRow = dbl ? -1 : row;
    g_wbLastClickMs = now;
    if (dbl) wbRowActivate(row);
    requestUpdate();
}

// ---- 导入 ----
static void wbImportPaths(const std::vector<std::filesystem::path>& paths, bool switchTab) {
    wbBoot();
    int nFile = 0, nDir = 0;
    for (const auto& p : paths) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            pxc_index::addImported(g_wbStore, p.u8string(), true);
            ++nDir;
        } else if (isPxcFile(p)) {
            pxc_index::addImported(g_wbStore, p.u8string(), false);
            ++nFile;
        } else {
            pxc_log::write("WARN", "Import", "忽略非 .pxc / 非目录：" + p.u8string());
        }
    }
    pxc_index::pruneStore(g_wbStore);
    wbSaveSoon();
    g_wb.loaded[(int)pxc_index::Tab::Library] = false;
    g_wb.loaded[(int)pxc_index::Tab::All] = false;
    // 导入按钮是显式动作，切到工程库让用户马上看到结果；拖入/右键加入则不切（见 wbAddToLibrary）。
    if (switchTab) wbLoadTab((int)pxc_index::Tab::Library, true);
    g_wb.status = "已导入 " + std::to_string(nDir) + " 个文件夹 / " + std::to_string(nFile) + " 个文件。";
    requestUpdate();
}

static void wbOnImportFolder() {
    std::filesystem::path p = pickFolderPath(L"导入工程文件夹");
    if (p.empty()) { requestUpdate(); return; }
    wbImportPaths({p});
}

static void wbOnImportFile() {
    std::filesystem::path p = pickPxcFile();
    if (p.empty()) { requestUpdate(); return; }
    wbImportPaths({p});
}

// ---- 右键菜单 ----
static void wbOpenMenu(int row) {
    g_wbMenu.open = true;
    g_wbMenu.row = row;
    g_wbMenu.items.clear();
    pxc_index::Item* ip = wbItemAt(row);
    if (!ip) {
        g_wbMenu.items.push_back("刷新");
    } else {
        g_wbMenu.items.push_back(ip->folder ? "进入文件夹" : "启动");
        g_wbMenu.items.push_back("打开所在文件夹");
        g_wbMenu.items.push_back("复制路径");
        g_wbMenu.items.push_back(pxc_index::isFavorite(g_wbStore, ip->path) ? "取消收藏" : "收藏");
        const bool inLib = wbInLibrary(ip->path);
        if (!inLib) {
            g_wbMenu.items.push_back("加入工程库");
            if (!ip->folder) g_wbMenu.items.push_back("所在文件夹加入工程库");
        }
        if (inLib && g_wb.tab == (int)pxc_index::Tab::Library) g_wbMenu.items.push_back("从工程库移出");
        if (g_wb.tab == (int)pxc_index::Tab::Favorite) g_wbMenu.items.push_back("从收藏移出");
        if (g_wb.tab == (int)pxc_index::Tab::Recent) g_wbMenu.items.push_back("从近期文件里隐藏");
    }
    // 菜单坐标**自己算**，不用指针事件给的 e.x/e.y。
    // 那条路径上拿到的是被缩放过的值（实测点在 420,215，菜单却弹到 855,435 —— 正好约 2 倍），
    // 而行的位置是确定的：用行算，菜单位置就与 DPI 无关了。
    const float menuH = (float)g_wbMenu.items.size() * 30.0f + 12.0f;
    const int r = std::max(0, row);
    float my = g_wb.listTop + (float)r * wb::kRowH - g_wbScroll.get() + 28.0f;
    my = std::max(8.0f, std::min(my, g_scrH - wb::kPageY - menuH - 8.0f));
    g_wbMenu.x = wb::kPageX + wb::kPad + 140.0f;
    g_wbMenu.y = wb::kPageY + my;
    requestUpdate();
}

static void wbMenuPick(int item) {
    const int row = g_wbMenu.row;
    pxc_index::Item* ip = wbItemAt(row);
    if (!ip) { wbRefresh(); return; }
    const std::string& label = (item >= 0 && item < (int)g_wbMenu.items.size())
                                   ? g_wbMenu.items[(size_t)item]
                                   : std::string();
    const std::string path = ip->path;
    const pxc_index::Item snapshot = *ip;   // 后面的动作会重建 all，先把要用的字段拷出来
    if (label == "刷新") wbRefresh();
    else if (label == "进入文件夹") wbEnter(row);
    else if (label == "启动") wbRowActivate(row);
    else if (label == "打开所在文件夹") wbOpenFolderOf(snapshot);
    else if (label == "复制路径") wbCopyToClipboard(path);
    else if (label == "收藏" || label == "取消收藏") wbToggleStar(path);
    else if (label == "加入工程库") wbAddToLibrary(snapshot, false);
    else if (label == "所在文件夹加入工程库") wbAddToLibrary(snapshot, true);
    else if (label == "从工程库移出") {
        pxc_index::removeImported(g_wbStore, path);
        wbSaveSoon();
        g_wb.loaded[(int)pxc_index::Tab::All] = false;
        wbLoadTab(g_wb.tab, true);
    } else if (label == "从收藏移出") {
        pxc_index::removeFavorite(g_wbStore, path);
        wbSaveSoon();
        wbLoadTab(g_wb.tab, true);
    } else if (label == "从近期文件里隐藏") {
        pxc_index::hideRecent(g_wbStore, path);
        wbSaveSoon();
        wbLoadTab(g_wb.tab, true);
    }
    requestUpdate();
}

static void composeWbMenu(core::dsl::Ui& ui, const eui::Screen& screen) {
    if (!g_wbMenu.open || g_wbMenu.items.empty()) return;
    // 给菜单一个自己定好位的全屏宿主：contextMenu 的最外层只是个 size(全屏) 的 stack，
    // 菜单面板按传入的 x/y 在它内部定位，所以宿主的原点就是菜单坐标系的原点。
    // 用 .position(0,0) 让宿主原点对齐客户区原点 —— 右键事件给的 e.x/e.y 正是客户区坐标。
    ui.stack("wb.menu.host").position(0.0f, 0.0f).size(screen.width, screen.height)
        .zIndex(1100).content([&] {
            components::contextMenu(ui, "wb.menu")
                .theme(uiTheme())
                .screen(screen.width, screen.height)
                .open(true)
                .position(g_wbMenu.x, g_wbMenu.y)
                .size(190.0f, 30.0f)
                .zIndex(1101)
                .items(g_wbMenu.items)
                .onSelect([](int i) { wbMenuPick(i); })
                .onOpenChange([](bool v) {
                    if (!v) {
                        g_wbMenu.open = false;
                        requestUpdate();
                    }
                })
                .build();
        }).build();
}

// ---------- 工程文件工作台（v3）：绘制 ----------
// 文本不裁剪（EUI 的 ui.text 给了 size() 也照样往右溢出），靠右的列要先按宽度把文案裁短。
static std::string wbFit(const std::string& s, float maxW, float fs) {
    if (maxW <= 0.0f) return s;
    // 先用**真实字形宽度**判一次能不能放下：wbTextW 是按 em 估的，纯 ASCII 会高估三成
    // （"1.20.1.0" 实测 56 px，估算 69 px）—— 字号放大后版本号就被这一个误判截成 "…20.1.0"。
    // 放得下就整串留着，放不下才进下面按字符回退的截断循环（那里仍是估算，图快）。
    if (wbRealTextW(s, fs) <= maxW) return s;
    std::string tail;
    size_t i = s.size();
    while (i > 0) {
        size_t j = i - 1;
        while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) --j;   // 回退一个 UTF-8 字符
        i = j;
        tail = s.substr(i);
        if (wbTextW(tail, fs) + fs * 1.2f * textScale() > maxW) break;
    }
    if (tail.size() >= s.size()) return s;
    return "…" + tail;
}

// wbFit 是**保尾**（"…-Bold.ttf"）：适合路径、适合"差异在末尾"的串。
// 字体名正相反 —— 区分度全在前缀（`LXGWWenKai…` / `NotoSansSC…`），保尾等于把名字抹掉，
// 只剩一个谁都一样的后缀。所以这里单独做一个保头的版本。
static std::string wbFitHead(const std::string& s, float maxW, float fs) {
    if (maxW <= 0.0f) return s;
    const float em = fs * textScale();
    if (wbTextW(s, fs) <= maxW) return s;
    const float limit = maxW - em;   // 省略号占一个汉字宽
    std::string out;
    float w = 0.0f;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        float adv = em * 0.56f;
        if (c >= 0x80 && c < 0xE0) { len = 2; adv = em * 0.32f; }
        else if (c >= 0xE0 && c < 0xF0) { len = 3; adv = em; }
        else if (c >= 0xF0) { len = 4; adv = em; }
        if (w + adv > limit) break;
        out.append(s, i, len);
        w += adv;
        i += len;
    }
    return out + "…";
}

static bool wbVersionRegistered(const std::string& version) {
    if (version.empty()) return false;
    for (const auto& e : g_cfg.registry)
        if (pxc_reg::sameVersion(e.product_version, version)) return true;
    return false;
}

static pxc_index::VerState wbStateOf(const pxc_index::Item& it) {
    // 直接查登记表，不构造版本数组：这段跑在每一行、每一帧上，一次 vector 分配 × 行数不划算。
    if (it.folder || !it.detected) return pxc_index::VerState::Pending;
    if (it.version.empty()) return pxc_index::VerState::Unreadable;
    for (const auto& e : g_cfg.registry)
        if (pxc_reg::sameVersion(e.product_version, it.version)) return pxc_index::VerState::Ok;
    return pxc_index::VerState::Missing;
}

static int g_wbHoverSub = -1;   // B4：当前悬浮的工程路径行 index（-1 表示无）

static void composeWbRow(core::dsl::Ui& ui, const std::string& id, std::int64_t index, float width,
                         float height) {
    if (index < 0 || index >= (std::int64_t)g_wb.rows.size()) return;
    pxc_index::Item* ip = wbItemAt((int)index);
    if (!ip) return;
    const pxc_index::Item it = *ip;      // 拷一份：后面的回调会改容器
    const bool sel = ((int)index == g_wbSel);
    // width 已经是 virtualList 算好的内容宽（视口减滚动条与间隙），这里**不再**减 8。
    const float cardW = width;
    const float cy = 4.0f;
    const wb::Tokens T = wb::make(uiTheme());

    ui.rect(id + ".bg").position(0.0f, cy).size(cardW, wb::kCardH)
        .states(sel ? T.rowSel : T.rowBg, T.rowHover, T.rowHover)
        .radius(wb::kCardRadius).border(1.0f, T.rowBorder)
        .onClick([row = (int)index] { wbRowClicked(row); })
        .onContextMenu([row = (int)index](const core::PointerEvent&, const core::Rect&) {
            wbOpenMenu(row);
        })
        .build();

    // 图标：.pxc 用素材的 SVG（文本已读进内存），文件夹用图标字体。
    // 找不到图标文件时退回自绘色块 + 字体图标，不留破图。
    if (!it.folder && !g_wb.iconSvg.empty()) {
        ui.svg(id + ".ic").position(wb::kIconX, cy + wb::kIconY)
            .size(wb::kIconSize, wb::kIconSize).source(g_wb.iconSvg).build();
    } else {
        ui.rect(id + ".ic").position(wb::kIconX, cy + wb::kIconY)
            .size(wb::kIconSize, wb::kIconSize)
            .color(it.folder ? core::Color{0.22f, 0.25f, 0.31f, 1.0f}
                             : core::Color{0.42f, 0.23f, 0.19f, 1.0f})
            .radius(9.0f).build();
        ui.text(id + ".ict").position(wb::kIconX, cy + wb::kIconY)
            .size(wb::kIconSize, wb::kIconSize).icon(it.folder ? 0xF07B : 0xF15B)
            .fontSize(17.0f).lineHeight(wb::kIconSize)
            .color(it.folder ? core::Color{0.72f, 0.78f, 0.88f, 1.0f}
                             : core::Color{0.95f, 0.72f, 0.60f, 1.0f})
            .horizontalAlign(core::HorizontalAlign::Center)
            .verticalAlign(core::VerticalAlign::Center).build();
    }

    // 标题 + 副行（路径）：工程卡与文件夹卡**同一套位置**（用户明确：文件夹卡片标题要改回与工程卡一致）。
    const float nameW = std::max(80.0f, (cardW - wb::kVerColRight) - wb::kNameX - 16.0f);
    const float nameFont = 17.0f;
    const float nameTop = cy + wb::kNameY;   // kNameY = 14
    const float subTop = cy + wb::kSubY;     // kSubY = 42
    // 星标：按标题行框（22 高）居中，再叠一个实测出来的上提量 —— FA 的星形在行框里天然偏下。
    const bool fav = pxc_index::isFavorite(g_wbStore, it.path);
    const float starY = nameTop + (22.0f - wb::kStarSize) * 0.5f + wb::kStarNudgeY;
    ui.text(id + ".star").position(wb::kStarX, starY)
        .size(wb::kStarSize, wb::kStarSize).icon(fav ? 0xF005 : 0xF006)
        .fontSize(14.0f).lineHeight(wb::kStarSize)
        .color(fav ? T.starOn : T.starOff)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center).build();
    components::mouseArea(ui, id + ".starhit")
        .position(wb::kStarX - 3.0f, starY - 5.0f).size(24.0f, 26.0f)
        .onTap([path = it.path] { wbToggleStar(path); }).build();

    ui.text(id + ".name").position(wb::kNameX, nameTop).size(nameW, 22.0f)
        .text(wbFit(it.name, nameW, nameFont)).fontSize(nameFont).lineHeight(22.0f)
        .fontWeight(600).color(T.titleFg).build();

    // 副行：只有文字部分可点（B4），悬浮时变色 + 下划线强调
    const bool hasPath = !it.parentDir.empty();
    const std::string sub = hasPath ? it.parentDir : std::string("—");
    const float subFont = 11.0f;
    // 路径左对齐到收藏（星标）按钮的左边，而不是标题左边；宽度顺延到版本列前。
    const float subX = wb::kStarX;
    const float subMaxW = nameW + (wb::kNameX - wb::kStarX);
    // 裁剪仍用 wbFit（估算，偏保守），但**命中区与下划线用真实字宽** —— 这样
    // "能点的范围"和"看得见的字"一致，不会出现点空白也进文件夹、悬停在空处也划下划线。
    const std::string subFit = wbFit(sub, subMaxW, subFont);
    const float subW = wbRealTextW(subFit, subFont);
    const bool subHover = hasPath && (g_wbHoverSub == (int)index);
    const core::Color subColor = subHover ? T.titleFg : T.metaFg;
    ui.text(id + ".sub").position(subX, subTop).size(subMaxW, 16.0f)
        .text(subFit).fontSize(subFont).lineHeight(16.0f).color(subColor).build();
    if (subHover) {
        ui.rect(id + ".subul").position(subX, subTop + 15.0f).size(subW, 1.0f)
            .color(subColor).build();
    }
    if (hasPath) {
        components::mouseArea(ui, id + ".subhit")
            .position(subX, subTop - 1.0f).size(subW, 18.0f)
            .onTap([row = (int)index] {
                pxc_index::Item* p2 = wbItemAt(row);
                if (p2) wbOpenFolderOf(*p2);
            })
            .onHover([row = (int)index](bool h) {
                if (h) { if (g_wbHoverSub != row) { g_wbHoverSub = row; requestUpdate(); } }
                else   { if (g_wbHoverSub == row) { g_wbHoverSub = -1; requestUpdate(); } }
            }).build();
    }

    // 版本徽章 + 修改时间：竖排同一列，列左 = 卡片右 - 208（A2：表头拆开，卡片内不动）
    const float vx = cardW - wb::kVerColRight;
    if (!it.folder) {
        core::Color pbg = T.pillUnkBg, pfg = T.pillUnkFg;
        std::string pill = "识别中";
        switch (wbStateOf(it)) {
            case pxc_index::VerState::Ok:
                pbg = T.pillOkBg; pfg = T.pillOkFg; pill = it.version; break;
            case pxc_index::VerState::Missing:
                pbg = T.pillWarnBg; pfg = T.pillWarnFg; pill = it.version; break;
            case pxc_index::VerState::Unreadable:
                pbg = T.pillBadBg; pfg = T.pillBadFg; pill = "识别不出"; break;
            default:
                pbg = T.pillUnkBg; pfg = T.pillUnkFg; pill = "识别中"; break;
        }
        ui.rect(id + ".pill").position(vx, cy + wb::kVerBadgeY)
            .size(wb::kVerColW, wb::kVerBadgeH).color(pbg).radius(wb::kVerBadgeH * 0.5f).build();
        // 圆点：绿/橙/红/灰，一眼看出"能不能双击开"
        ui.rect(id + ".dot").position(vx + wb::kVerDotX,
                                      cy + wb::kVerBadgeY + (wb::kVerBadgeH - wb::kVerDotSize) * 0.5f)
            .size(wb::kVerDotSize, wb::kVerDotSize).color(pfg).radius(wb::kVerDotSize * 0.5f).build();
        // B1：版本号改回默认字体（与其它文本一致）；代价是 NotoSansSC 的「1」衬线会吃掉紧随的
        // 小数点，版本串如 "1.20.1.0" 可能被读成 "120.10" —— 见交付说明，必要时可改回等宽 + 加粗。
        ui.text(id + ".pillt").position(vx + wb::kVerTextX, cy + wb::kVerBadgeY)
            .size(wb::kVerTextW, wb::kVerBadgeH)
            .text(wbFit(pill, wb::kVerTextW, 11.0f)).fontSize(11.0f).lineHeight(wb::kVerBadgeH)
            .color(pfg).verticalAlign(core::VerticalAlign::Center).build();
    }
    ui.text(id + ".time").position(vx, cy + wb::kTimeY).size(wb::kVerColW, wb::kTimeH)
        .text(wbFmtTime(it.mtime)).fontSize(11.0f).lineHeight(wb::kTimeH).color(T.metaFg).build();

    // 启动按钮（工程）/ 进入按钮（文件夹，C1：带按钮底、垂直居中，仿工程启动按钮）
    if (!it.folder) {
        const float bx = cardW - wb::kLaunchRight;
        const float by = cy + (wb::kCardH - wb::kLaunchH) * 0.5f;
        components::mouseArea(ui, id + ".lb").position(bx, by).size(wb::kLaunchW, wb::kLaunchH)
            .radius(8.0f).color(T.launchBg)
            .onTap([row = (int)index] { wbLaunchRow(row); }).build();
        ui.text(id + ".lbt").position(bx, by).size(wb::kLaunchW, wb::kLaunchH).text("启动")
            .fontSize(12.5f).lineHeight(wb::kLaunchH).color(T.launchFg)
            .horizontalAlign(core::HorizontalAlign::Center)
            .verticalAlign(core::VerticalAlign::Center).build();
    } else {
        const float bx = cardW - wb::kLaunchRight;
        const float by = cy + (wb::kCardH - wb::kLaunchH) * 0.5f;
        // C1：文件夹进入按钮仿工程启动按钮外观（带边框圆角底）。mouseArea 不支持 border，
        // 故底衬用 ui.rect 画，mouseArea 叠在上面做透明命中区（默认 color 全透明）。
        ui.rect(id + ".foldbg").position(bx, by).size(wb::kLaunchW, wb::kLaunchH)
            .color(T.chipBg).radius(8.0f).border(1.0f, T.rowBorder).build();
        components::mouseArea(ui, id + ".foldbtn").position(bx, by).size(wb::kLaunchW, wb::kLaunchH)
            .radius(8.0f).onTap([row = (int)index] { wbEnter(row); }).build();
        ui.text(id + ".foldt").position(bx, by).size(wb::kLaunchW, wb::kLaunchH).icon(0xF054)
            .fontSize(14.0f).lineHeight(wb::kLaunchH).color(T.chipFg)
            .horizontalAlign(core::HorizontalAlign::Center)
            .verticalAlign(core::VerticalAlign::Center).build();
    }
}

// `.pxc` 关联的注册 / 注销。UI 在「设置 → 通用」（那里的关联按钮和两个 Switch 都由设置页画），
// 这里只留状态机与系统调用 —— 原来工程页底部那条关联长条已删除，别再画第二份。
static std::filesystem::path wbSelfExe() {
    wchar_t buf[MAX_PATH * 4] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return n ? std::filesystem::path(std::wstring(buf, n)) : std::filesystem::path();
}

static void wbToggleAssoc() {
    wbBoot();
    std::string err;
    if (g_wbAssoc.state == pxc_assoc::State::Other) {
        pxc_assoc::openDefaultAppsSettings();
        return;
    }
    if (g_wbAssoc.state == pxc_assoc::State::Ours) {
        pxc_assoc::unregisterSelf(g_wbStore.assoc.previousProgid, err);
        g_wbStore.assoc.registered = false;
        g_wb.status = err.empty() ? "已取消 .pxc 关联。" : ("取消关联出错：" + err);
    } else {
        std::string prev;
        if (pxc_assoc::registerSelf(wbSelfExe(), prev, err)) {
            g_wbStore.assoc.registered = true;
            g_wbStore.assoc.previousProgid = prev;
            g_wbStore.assoc.registeredAt = pxc_index::nowIso();
            g_wb.status = "已注册 .pxc 关联。想在双击时直接打开启动器，"
                          "还需在「打开方式 → 始终使用」里选一次（系统会自己记住）。";
        } else {
            g_wb.status = "注册关联失败：" + err;
        }
    }
    wbSaveSoon();
    g_wbAssoc = pxc_assoc::query();
    requestUpdate();
}

// ---------- 筛选浮窗（v4）----------
// 不用 components::dropdown：它的弹层会被 scrollView 的 .clip() 裁掉（§7.5 记过的坑）。
// 这里按根级 overlay 自绘，遮罩 900 / 面板 901，与版本库那套下拉同一个做法。
static float g_wbFilterX = 0.0f, g_wbFilterY = 0.0f;
static eui::Signal<std::string> g_wbFromDate{""};
static eui::Signal<std::string> g_wbToDate{""};

// "2026-10-01" -> 当天 00:00 的 unix 秒；解析不出返回 0（= 不限）。
static std::int64_t wbParseDate(const std::string& s) {
    int y = 0, m = 0, d = 0;
    if (sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return 0;
    if (y < 1970 || y > 2200 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (std::int64_t)mktime(&tm);
}

// 版本多选的可选项：本机登记过的版本 ∪ 当前层出现过的版本，末尾固定一个「未知版本」（空串）。
// 只在这个浮窗打开时算，不进每帧路径。
static std::vector<std::string> wbVersionChoices() {
    std::vector<std::string> out;
    for (const auto& e : g_cfg.registry) {
        if (e.product_version.empty()) continue;
        const std::string v = pxc_reg::normalizeVersion(e.product_version);
        if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    }
    for (const auto& it : g_wb.all) {
        if (it.folder || it.version.empty()) continue;
        if (std::find(out.begin(), out.end(), it.version) == out.end()) out.push_back(it.version);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const auto ka = pxc_adv::parseVersion(a), kb = pxc_adv::parseVersion(b);
        return std::make_tuple(ka.major, ka.minor, ka.patch, ka.build) >
               std::make_tuple(kb.major, kb.minor, kb.patch, kb.build);
    });
    out.push_back("");   // 未知版本
    return out;
}

static void composeWbFilterPanel(core::dsl::Ui& ui, const eui::Screen& screen) {
    if (!g_wb.filterOpen) return;
    const auto th = uiTheme();
    const wb::Tokens T = wb::make(th);
    const float PW = wb::kFilterPanelW;
    const float px = g_wbFilterX;
    // 面板底不能越过窗口下沿（窗口矮时往上贴）
    const float ph = std::min(wb::kFilterPanelH, std::max(300.0f, screen.height - g_wbFilterY - 12.0f));
    const float py = g_wbFilterY;
    const float pad = 14.0f;

    ui.stack("wb.fp.dismiss").position(0.0f, 0.0f).size(screen.width, screen.height)
        .zIndex(900).content([&] {
            ui.rect("wb.fp.dismiss.bg").size(screen.width, screen.height)
                .color(core::Color{0.0f, 0.0f, 0.0f, 0.02f})
                .onClick([] {
                    g_wb.filterOpen = false;
                    requestUpdate();
                }).build();
        }).build();

    ui.stack("wb.fp").position(px, py).size(PW, ph).zIndex(901).content([&, PW, ph, pad] {
        ui.rect("wb.fp.bg").size(PW, ph).color(th.surface).radius(12.0f)
            .border(1.0f, components::theme::withOpacity(th.border, 0.85f))
            .shadow(components::theme::popupShadow(th)).build();

        ui.text("wb.fp.title").position(pad, 12.0f).size(200.0f, 22.0f).text("筛选").fontSize(13.0f)
            .lineHeight(22.0f).fontWeight(600).color(T.titleFg).build();
        if (g_wb.filter.active()) {
            ui.stack("wb.fp.reset").position(PW - pad - 52.0f, 12.0f).size(52.0f, 22.0f).content([&] {
                ui.rect("wb.fp.reset.bg").size(52.0f, 22.0f).color(T.chipBg).radius(11.0f)
                    .onClick([] {
                        g_wb.filter.reset();
                        g_wbFromDate.set("");
                        g_wbToDate.set("");
                        wbRequery();
                    }).build();
                ui.text("wb.fp.reset.t").size(52.0f, 22.0f).text("重置").fontSize(11.0f).lineHeight(22.0f)
                    .color(T.chipFg).horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center).build();
            }).build();
        }

        auto chip = [&](const std::string& cid, const std::string& label, bool on, float cx, float cyy,
                        float cw, std::function<void()> fn, const std::string& fam = std::string()) {
            ui.stack(cid).position(cx, cyy).size(cw, 24.0f).content([&, cid, label, on, cw, fn, fam] {
                ui.rect(cid + ".bg").size(cw, 24.0f).color(on ? T.chipOnBg : T.chipBg).radius(12.0f)
                    .onClick(fn).build();
                auto tx = ui.text(cid + ".t").size(cw, 24.0f).text(label).fontSize(11.0f)
                              .lineHeight(24.0f);
                if (!fam.empty()) tx.fontFamily(fam);
                tx.color(on ? T.chipOnFg : T.chipFg)
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center).build();
            }).build();
        };
        auto section = [&](const std::string& sid, const std::string& text, float sy) {
            ui.text("wb.fp." + sid).position(pad, sy).size(200.0f, 16.0f).text(text).fontSize(11.0f)
                .lineHeight(16.0f).color(T.faintFg).build();
        };

        // ---- 版本（2 列，最多 8 个；再多只列前 7 + 未知）----
        float y = 42.0f;
        section("v", "版本", y);
        y += 20.0f;
        {
            auto vs = wbVersionChoices();
            const float gap = 8.0f;
            const float cw = (PW - pad * 2.0f - gap) * 0.5f;
            const size_t maxShow = 8;
            for (size_t k = 0; k < vs.size() && k < maxShow; ++k) {
                const std::string v = vs[k];
                const bool on = std::find(g_wb.filter.versions.begin(), g_wb.filter.versions.end(), v) !=
                                g_wb.filter.versions.end();
                const std::string label = v.empty() ? "未知版本" : v;
                const float cx = pad + (k % 2) * (cw + gap);
                const float cyy = y + (k / 2) * 28.0f;
                chip("wb.fp.v" + std::to_string(k), label, on, cx, cyy, cw, [v] {
                    auto& f = g_wb.filter.versions;
                    auto it2 = std::find(f.begin(), f.end(), v);
                    if (it2 == f.end()) f.push_back(v);
                    else f.erase(it2);
                    wbRequery();
                }, wbVerFontOf(label));
            }
            y += ((std::min(vs.size(), maxShow) + 1) / 2) * 28.0f + 8.0f;
        }

        // ---- 状态（四档，与卡片圆点同口径）----
        section("s", "状态", y);
        y += 20.0f;
        {
            // 位与文案都取 pxc_index 的同一份映射（显示顺序 ≠ 枚举顺序，理由见 header）。
            const float gap = 8.0f;
            const float cw = (PW - pad * 2.0f - gap) * 0.5f;
            for (int k = 0; k < pxc_index::kStateChipCount; ++k) {
                const int bit = pxc_index::stateChipBit(k);
                const bool on = (g_wb.filter.states & bit) != 0;
                const float cx = pad + (k % 2) * (cw + gap);
                const float cyy = y + (k / 2) * 28.0f;
                chip("wb.fp.s" + std::to_string(k), pxc_index::stateChipName(k), on, cx, cyy, cw,
                     [bit] {
                         g_wb.filter.states ^= bit;
                         wbRequery();
                     });
            }
            y += 2 * 28.0f + 8.0f;
        }

        // ---- 修改时间（六档，单选）----
        section("t", "修改时间", y);
        y += 20.0f;
        {
            const float gap = 6.0f;
            const float cw = (PW - pad * 2.0f - gap * 2.0f) / 3.0f;
            for (int k = 0; k < 6; ++k) {
                const bool on = (g_wb.filter.timeRange == k);
                const float cx = pad + (k % 3) * (cw + gap);
                const float cyy = y + (k / 3) * 28.0f;
                chip("wb.fp.t" + std::to_string(k), pxc_index::timeRangeName(k), on, cx, cyy, cw, [k] {
                    g_wb.filter.timeRange = k;
                    wbRequery();
                });
            }
            y += 2 * 28.0f + 4.0f;
        }
        if (g_wb.filter.timeRange == 5) {
            const float iw = (PW - pad * 2.0f - 24.0f) * 0.5f;
            ui.stack("wb.fp.from.wrap").position(pad, y).size(iw, 26.0f).content([&, iw] {
                components::input(ui, "wb.fp.from").theme(th).size(iw, 26.0f).fontSize(11.0f)
                    .placeholder("2026-01-01").bind(g_wbFromDate)
                    .onChange([](const std::string& v) {
                        g_wb.filter.from = wbParseDate(v);
                        wbRequery();
                    }).build();
            }).build();
            ui.text("wb.fp.tilde").position(pad + iw, y).size(24.0f, 26.0f).text("～")
                .fontSize(11.0f).lineHeight(26.0f).color(T.faintFg)
                .horizontalAlign(core::HorizontalAlign::Center).build();
            ui.stack("wb.fp.to.wrap").position(pad + iw + 24.0f, y).size(iw, 26.0f).content([&, iw] {
                components::input(ui, "wb.fp.to").theme(th).size(iw, 26.0f).fontSize(11.0f)
                    .placeholder("2026-12-31").bind(g_wbToDate)
                    .onChange([](const std::string& v) {
                        const std::int64_t d = wbParseDate(v);
                        g_wb.filter.to = d ? d + 86399 : 0;   // 含当天
                        wbRequery();
                    }).build();
            }).build();
            y += 32.0f;
        }

        // ---- 收藏 ----
        section("f", "收藏", y);
        y += 20.0f;
        chip("wb.fp.fav", "仅显示已收藏", g_wb.filter.onlyFav, pad, y,
             PW - pad * 2.0f, [] {
                 g_wb.filter.onlyFav = !g_wb.filter.onlyFav;
                 wbRequery();
             });

        // 底栏：完成
        const float by = ph - 44.0f;
        ui.stack("wb.fp.done.wrap").position(PW - pad - 84.0f, by).size(84.0f, 30.0f).content([&] {
            components::button(ui, "wb.fp.done").theme(th).size(84.0f, 30.0f).text("完成").fontSize(12.0f)
                .onClick([] {
                    g_wb.filterOpen = false;
                    requestUpdate();
                }).build();
        }).build();
    }).build();
}

static void composeWorkbench(core::dsl::Ui& ui, float width) {
    wbBoot();
    wbTick();

    // 第二实例转发 / 双击进来的路径：.pxc 直接打开，目录进工程库
    wbProcessInbox();

    if (!g_wb.loaded[g_wb.tab]) wbLoadTab(g_wb.tab, false);
    if (pxc_index::foldQuery(g_wbQuery.get()) != g_wbAppliedQuery) wbRequery();
    wbTickDetect();

    const float innerW = width - wb::kPad * 2.0f;
    const float cardRight = wb::kPad + innerW;
    const auto th = uiTheme();
    const wb::Tokens T = wb::make(th);
    const float x = wb::kPad;
    const bool inFolder = !g_wb.dir.empty();

    ui.text("wb.title").position(x, wb::kTitleY).size(innerW - 330.0f, 40.0f).text("工程文件")
        .fontSize(28.0f).lineHeight(34.0f).fontWeight(800).color(T.titleFg).build();

    // 右上两枚（照参考图：左边实心、右边描边）
    const float fw1 = 150.0f, fw2 = 140.0f;
    const float btop = wb::kTitleY + 3.0f;
    ui.stack("wb.impd.wrap").position(cardRight - fw2 - 8.0f - fw1, btop).size(fw1, 34.0f).content([&] {
        components::button(ui, "wb.impd").theme(th, true).size(fw1, 34.0f).text("导入工程文件夹").fontSize(12.5f)
            .onClick([] { guarded("导入工程文件夹", wbOnImportFolder); }).build();
    }).build();
    ui.stack("wb.impf.wrap").position(cardRight - fw2, btop).size(fw2, 34.0f).content([&] {
        components::button(ui, "wb.impf").theme(th, false).size(fw2, 34.0f).text("导入 PXC 文件").fontSize(12.5f)
            .onClick([] { guarded("导入 PXC 文件", wbOnImportFile); }).build();
    }).build();

    // 页签带（六页签）。进入态**仍然高亮原页签** —— 进入是在某个页签内部下钻，不是新页签。
    float tx = x;
    float tabsRight = x;
    for (int t = 0; t < pxc_index::kTabCount; ++t) {
        const std::string label = pxc_index::tabName(t);
        const float w = wbTextW(label, 12.5f) + 30.0f;
        const bool on = (t == g_wb.tab);
        const std::string id = "wb.tab" + std::to_string(t);
        ui.stack(id).position(tx, wb::kTabsY).size(w, 30.0f).content([&, id, t, w, label, on] {
            ui.rect(id + ".bg").size(w, 30.0f).color(on ? T.tabSelBg : T.chipBg).radius(15.0f)
                .onClick([t] { wbSetTab(t); }).build();
            ui.text(id + ".t").size(w, 30.0f).text(label).fontSize(12.5f).lineHeight(30.0f)
                .color(on ? T.tabSelFg : T.chipFg)
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center).build();
        }).build();
        tx += w + 8.0f;
        tabsRight = tx;
    }

    // 搜索框（右）：窗口窄时压缩，优先保住页签带
    const float qAvail = std::max(180.0f, cardRight - tabsRight - 12.0f);
    const float qw = std::min(300.0f, qAvail);
    ui.stack("wb.q.wrap").position(cardRight - qw, wb::kTabsY).size(qw, 30.0f).content([&, qw] {
        components::input(ui, "wb.q").theme(th).size(qw, 30.0f).fontSize(12.0f)
            .placeholder("搜索：文件名 / 路径（空格分词）")
            .bind(g_wbQuery)
            .onChange([](const std::string& v) { g_wbQuery.set(v); wbRequery(); })
            .build();
    }).build();

    // 进入态：面包屑占掉表头那行，表头与列表整体下移
    const float shift = inFolder ? wb::kCrumbShift : 0.0f;
    if (inFolder) {
        float bx = x;
        ui.stack("wb.crumb.back").position(bx, wb::kSortY).size(26.0f, wb::kCrumbH).content([&] {
            ui.rect("wb.crumb.back.bg").size(26.0f, wb::kCrumbH).color(T.chipBg).radius(6.0f)
                .onClick([] { wbUp(); }).build();
            ui.text("wb.crumb.back.t").size(26.0f, wb::kCrumbH).icon(0xF060).fontSize(11.0f)
                .lineHeight(wb::kCrumbH).color(T.chipFg)
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center).build();
        }).build();
        bx += 32.0f;
        // 根段 = 页签名（点它回根层）；后面每段可点回那一层
        // 面包屑是"我在哪一层"的唯一提示，字号跟着行高走（12.5），并与左侧返回按钮同中线
        constexpr float kCrumbFont = 13.5f;
        std::string segLabel = pxc_index::tabName(g_wb.tab);
        float segW = wbTextW(segLabel, kCrumbFont) + 10.0f;
        ui.text("wb.crumb.root").position(bx, wb::kSortY).size(segW, wb::kCrumbH).text(segLabel)
            .fontSize(kCrumbFont).lineHeight(wb::kCrumbH).color(T.chipFg)
            .verticalAlign(core::VerticalAlign::Center).build();
        components::mouseArea(ui, "wb.crumb.root.hit").position(bx, wb::kSortY)
            .size(segW, wb::kCrumbH).onTap([] { wbCrumbTo(-1); }).build();
        bx += segW;
        for (int k = 0; k < (int)g_wb.crumbs.size(); ++k) {
            const std::string name = g_wb.crumbs[(size_t)k].first;
            ui.text("wb.crumb.s" + std::to_string(k)).position(bx, wb::kSortY)
                .size(10.0f, wb::kCrumbH).text("/").fontSize(kCrumbFont).lineHeight(wb::kCrumbH)
                .color(T.faintFg).verticalAlign(core::VerticalAlign::Center).build();
            bx += 10.0f;
            const float nw = std::min(220.0f, wbTextW(name, kCrumbFont) + 10.0f);
            const bool last = (k == (int)g_wb.crumbs.size() - 1);
            ui.text("wb.crumb.n" + std::to_string(k)).position(bx, wb::kSortY).size(nw, wb::kCrumbH)
                .text(wbFit(name, nw, kCrumbFont)).fontSize(kCrumbFont).lineHeight(wb::kCrumbH)
                .color(last ? T.titleFg : T.chipFg)
                .verticalAlign(core::VerticalAlign::Center).build();
            if (!last) {
                components::mouseArea(ui, "wb.crumb.n" + std::to_string(k) + ".hit")
                    .position(bx, wb::kSortY).size(nw, wb::kCrumbH)
                    .onTap([k] { wbCrumbTo(k); }).build();
            }
            bx += nw;
        }
    }

    // 排序 / 筛选 / 刷新 同一行，且装在同一条**底板**里 —— 三者是一条工具条，
    // 底板让"这行在管列表"这件事在结构上看得出来，不再是三段浮在页面上的散件。
    // 筛选（漏斗）与刷新（撤销箭头）并排放在这行**最右端**（用户要求相邻）。
    {
        const float hy = wb::kSortY + shift;
        const float listRight = cardRight - wb::kListScrollReserve;

        // 底板先画：后声明的元素压在上面（EUI 的层级就是声明顺序）
        ui.rect("wb.bar").position(x, wb::kBarY + shift).size(innerW, wb::kBarH)
            .color(T.barBg).radius(8.0f).border(1.0f, T.barBorder).build();

        // 右端：漏斗 + 刷新，两枚 28x24 图标按钮紧挨着（间隔 kBarGap）
        {
            const float bw = wb::kIconBtnW, bh = wb::kIconBtnH;
            const float rfX = listRight - bw;
            const float fbX = rfX - wb::kBarGap - bw;
            g_wbFilterX = wb::kPageX + x;
            g_wbFilterY = wb::kPageY + hy + bh + 6.0f;

            // 图标用 Font Awesome 7 Free：FILTER = U+F0B0，UNDO = U+F0E2。
            auto iconBtn = [&](const std::string& sid, unsigned int cp, float bx, core::Color bg,
                               std::function<void()> onClick) {
                ui.rect("wb." + sid + ".bg").position(bx, hy).size(bw, bh)
                    .states(bg, T.rowHover, T.rowHover).radius(6.0f)
                    .border(1.0f, T.barBorder).onClick(std::move(onClick)).build();
                ui.text("wb." + sid + ".ic").position(bx, hy).size(bw, bh).icon(cp)
                    .fontSize(13.0f).lineHeight(bh).color(T.chipFg)
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center).build();
            };
            iconBtn("fb", 0xF0B0, fbX,
                    (g_wb.filterOpen || g_wb.filter.active()) ? T.chipOnBg : T.chipBg,
                    [] {
                        g_wb.filterOpen = !g_wb.filterOpen;
                        requestUpdate();
                    });
            iconBtn("rf", 0xF0E2, rfX, T.chipBg, [] { guarded("刷新列表", wbRefresh); });

            // 有激活筛选时，漏斗左侧给一枚小「重置」—— 仍贴着图标按钮，不回到行首。
            // 宽度卡在 44：再宽就会压到「版本」chip 的右端（版本 chip 右缘 = listRight-116）。
            if (g_wb.filter.active()) {
                const float rw = 44.0f;
                const float rbX = fbX - wb::kBarGap - rw;
                ui.rect("wb.fr.bg").position(rbX, hy).size(rw, bh)
                    .states(T.chipBg, T.rowHover, T.rowHover).radius(6.0f)
                    .border(1.0f, T.barBorder)
                    .onClick([] {
                        g_wb.filter.reset();
                        g_wbFromDate.set("");
                        g_wbToDate.set("");
                        wbRequery();
                    }).build();
                ui.text("wb.fr.t").position(rbX, hy).size(rw, bh).text("重置")
                    .fontSize(11.0f).lineHeight(bh).color(T.chipFg)
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center).build();
            }
        }

        // 排序 chip：与左右图标按钮**同一高度、同一中线**（原来 18 高、顶对齐，比按钮高 3px）
        auto sortChip = [&](const std::string& sid, const std::string& text, pxc_index::SortKey key,
                            float sx, float sw) {
            const auto cur = g_wbSort[g_wb.tab];
            const bool on = (cur.key == key);
            const std::string label = text + (on ? (cur.asc ? " ↑" : " ↓") : "");
            ui.text("wb.sh." + sid).position(sx, hy).size(sw, wb::kIconBtnH).text(label)
                .fontSize(11.5f).lineHeight(wb::kIconBtnH).color(on ? T.titleFg : T.metaFg)
                .verticalAlign(core::VerticalAlign::Center).build();
            components::mouseArea(ui, "wb.sh." + sid + ".hit").position(sx, hy)
                .size(sw, wb::kIconBtnH).onTap([key] { wbCycleSort(key); }).build();
        };
        // 文件名对齐卡片名列（kNameX）
        const float nameX = x + wb::kNameX;
        sortChip("name", "文件名", pxc_index::SortKey::Name, nameX, 130.0f);
        // 修改时间 / 版本：拆两枚独立 chip，修改时间在左、版本在右（A2）。
        // 卡片内这俩仍竖排同一列（列左缘 = listRight - kVerColRight），这里只是把表头拆开。
        const float vx = listRight - wb::kVerColRight;
        sortChip("time", "修改时间", pxc_index::SortKey::Mtime, vx - 92.0f, 92.0f);
        sortChip("ver", "版本", pxc_index::SortKey::Version, vx, 92.0f);
    }

    // 列表
    const float listY = wb::kListY + shift;
    g_wb.listTop = listY;
    // 底部预留（关联条已搬去设置页，这里只剩状态行）：
    //   列表 → 状态行 8 + 状态行 18 + 状态行 → 窗口下缘 12 = 38。
    // 这里**不**再扣 clientInsetH()：实测本机 `screen.height` 就报客户区高（1040x720 的窗口
    // 报 720，而外框是 1056x759），按外框差值再减 39 会白丢 39 px。
    // 旧值写死 96 —— 当年关联条还在底部，后来搬走却没收回预留，白留了 70 px
    // （用户 2026-11-04：底部空白太多，要还给列表）。
    constexpr float kListGapBelow = 8.0f;      // 列表下缘 → 状态行
    constexpr float kStatusH = 18.0f;          // 状态行高
    constexpr float kListBottomPad = 12.0f;    // 状态行下缘 → 窗口下缘
    const float listH = std::max(120.0f, g_scrH - listY
                                             - kListGapBelow - kStatusH - kListBottomPad);
    if (g_wb.rows.empty()) {
        // 「本来就没有」「被搜索滤空」「被筛选滤空」是三件事：后两者要说清怎么恢复，
        // 否则用户会以为自己的工程都没了。
        std::string empty = g_wb.emptyText;
        const std::string q = g_wbQuery.get();
        if (!q.empty())
            empty = "没有匹配「" + q + "」的工程。清空搜索框就能看全部。";
        else if (g_wb.filter.active())
            empty = "「筛选 · " + std::to_string(g_wb.filter.activeGroups()) +
                    "」下没有工程。点「重置」看全部条目。";
        ui.text("wb.empty").position(x, listY + 36.0f).size(innerW, 60.0f).text(empty)
            .fontSize(12.5f).lineHeight(20.0f).color(T.faintFg).wrap(true)
            .horizontalAlign(core::HorizontalAlign::Center).build();
    } else {
        components::virtualList(ui, "wb.list." + std::to_string(g_wbListGen))
            .position(x, listY).size(innerW, listH)
            .itemCount((std::int64_t)g_wb.rows.size()).rowHeight(wb::kRowH)
            .bind(g_wbScroll).scrollbarWidth(8.0f).scrollbarGap(6.0f)
            .row(composeWbRow).build();
    }

    // 状态行 + 关联条
    const float fy = listY + listH + kListGapBelow;
    ui.text("wb.status").position(x, fy).size(innerW, kStatusH).text(g_wb.status).fontSize(11.0f)
        .lineHeight(kStatusH).color(T.metaFg).build();
    // 关联条从「一行 34」变成「按钮 26 + 4 + 说明 16」= 56（说明挪到按钮下方了）。
    // 关联条搬去设置页之后，底部只剩状态行（docs/06 §10.3.2）。
    g_pageH = std::max(g_scrH, fy + kStatusH + kListBottomPad);
}

static void composeProjects(core::dsl::Ui& ui, float width) { composeWorkbench(ui, width); }

// ---------- 缺版本协商浮窗 ----------
// 情形：双击/启动一个工程，识别出它要 1.19.0.11，但本机只登记了 1.18.5 和 1.20.1。
// 直接报错把用户堵死没有意义 —— 他手里就这几个版本。所以给出候选、说明风险、让他自己决定。
// 主按钮是「创建副本并启动」：跨版本的风险是**直接损坏文件** ——
// 用不匹配的版本打开并保存过一次，这个工程文件就报废了（不是"旧版本打不开"，那是可逆的）。
// 副本把这份不可逆风险关在一个新文件里，原文件全程只读。
static void composeVersionPicker(core::dsl::Ui& ui, const eui::Screen& screen) {
    if (!g_pick.open) return;
    const auto& A = g_pick.advice;
    const float W = 640.0f, H = 420.0f, pad = 24.0f;
    const float innerW = W - pad * 2.0f;

    auto btn = [&](const char* rid, float bx, float by, float bw, const std::string& label,
                   std::function<void()> fn, bool primary) {
        ui.stack(std::string(rid) + ".wrap").position(bx, by).size(bw, 34.0f)
            .content([&, rid, bw, label, fn, primary] {
                components::button(ui, rid).theme(uiTheme(), primary).size(bw, 34.0f)
                    .text(label).fontSize(12.0f).onClick(fn).build();
            }).build();
    };

    components::dialog(ui, "pick.dlg")
        .theme(uiTheme())
        .screen(screen.width, screen.height)
        .size(W, H)
        .zIndex(420)
        .open(true)
        .onOpenChange([](bool v) {
            if (!v && g_pick.open) closeVersionPicker();
        })
        .content([&] {
            const std::string name = toUtf8(g_pick.path.filename().c_str());
            ui.text("pick.title").position(pad, 16.0f).size(innerW, 24.0f)
                .text("这个工程需要的版本，本机没有").fontSize(16.0f).lineHeight(22.0f)
                .fontWeight(800).color({0.95f, 0.97f, 1.0f, 1.0f}).build();

            ui.text("pick.name").position(pad, 48.0f).size(innerW, 20.0f)
                .text(name).fontSize(13.5f).lineHeight(19.0f)
                .color({0.88f, 0.92f, 0.99f, 1.0f}).build();
            ui.text("pick.path").position(pad, 68.0f).size(innerW, 16.0f)
                .text(g_pick.path.u8string()).fontSize(10.5f).lineHeight(15.0f)
                .color({0.52f, 0.57f, 0.65f, 1.0f}).build();

            ui.text("pick.head").position(pad, 92.0f).size(innerW, 36.0f)
                .text(A.headline).fontSize(12.5f).lineHeight(17.0f).wrap(true)
                .color({0.80f, 0.90f, 0.98f, 1.0f}).build();
            ui.text("pick.detail").position(pad, 128.0f).size(innerW, 16.0f)
                .text(A.detail).fontSize(10.5f).lineHeight(15.0f)
                .color({0.55f, 0.60f, 0.68f, 1.0f}).build();

            const int n = (int)A.ordered.size();
            const int shown = std::min(n, 4);
            ui.text("pick.listhint").position(pad, 148.0f).size(innerW, 16.0f)
                .text(n == 0 ? "版本库里还没有登记任何 PXC。"
                             : ("已登记的版本（点一行选中）" + (n > shown ? "，共 " + std::to_string(n) + " 个，显示前 " + std::to_string(shown) + " 个" : "")))
                .fontSize(10.5f).lineHeight(15.0f).color({0.52f, 0.57f, 0.65f, 1.0f}).build();

            const float rowH = 40.0f, listY = 168.0f;
            for (int i = 0; i < shown; ++i) {
                const auto& c = A.ordered[(size_t)i];
                const bool sel = (i == g_pick.sel);
                const std::string rid = "pick.row" + std::to_string(i);
                ui.stack(rid).position(pad, listY + (float)i * rowH).size(innerW, rowH - 4.0f)
                    .content([&, i, sel, c, rid, innerW, rowH] {
                        ui.rect(rid + ".bg").size(innerW, rowH - 4.0f)
                            .color(sel ? core::Color{0.20f, 0.27f, 0.36f, 1.0f}
                                       : core::Color{0.145f, 0.165f, 0.205f, 1.0f})
                            .radius(7.0f)
                            .border(1.0f, sel ? core::Color{0.35f, 0.62f, 0.90f, 1.0f}
                                              : core::Color{0.24f, 0.27f, 0.33f, 1.0f})
                            .build();
                        ui.text(rid + ".ver").position(12.0f, 0.0f).size(120.0f, rowH - 4.0f)
                            .text(c.version).fontSize(12.5f).lineHeight(rowH - 6.0f)
                            .verticalAlign(core::VerticalAlign::Center)
                            .color({0.95f, 0.97f, 1.0f, 1.0f}).build();
                        ui.text(rid + ".fam").position(136.0f, 0.0f).size(100.0f, rowH - 4.0f)
                            .text(pxc_adv::familyName(c.family)).fontSize(10.5f)
                            .lineHeight(rowH - 6.0f).verticalAlign(core::VerticalAlign::Center)
                            .color({0.55f, 0.62f, 0.72f, 1.0f}).build();
                        ui.text(rid + ".why").position(240.0f, 0.0f).size(innerW - 252.0f, rowH - 4.0f)
                            .text(c.reason).fontSize(10.5f).lineHeight(rowH - 6.0f)
                            .verticalAlign(core::VerticalAlign::Center)
                            .color({0.72f, 0.78f, 0.86f, 1.0f}).build();
                        components::mouseArea(ui, rid + ".hit").position(0.0f, 0.0f)
                            .size(innerW, rowH - 4.0f)
                            .onTap([i] { g_pick.sel = i; requestUpdate(); }).build();
                    }).build();
            }

            ui.text("pick.risk").position(pad, 336.0f).size(innerW, 16.0f)
                .text("跨版本的风险是直接损坏文件：用不匹配的版本打开并保存过一次，这个工程就报废了。")
                .fontSize(10.5f).lineHeight(15.0f)
                .color({0.96f, 0.74f, 0.42f, 1.0f}).build();

            btn("pick.cancel", pad, 372.0f, 88.0f, "取消", [] { closeVersionPicker(); }, false);
            if (n > 0) {
                btn("pick.direct", 368.0f, 372.0f, 120.0f, "直接用原文件",
                    [] { guarded("直接打开原文件", [] { pickLaunch(pxc_project::StartMode::Direct); }); },
                    false);
                btn("pick.copy", 500.0f, 372.0f, 116.0f, "创建副本并启动",
                    [] { guarded("创建副本并启动", [] { pickLaunch(pxc_project::StartMode::Copy); }); },
                    true);
            }
        })
        .build();
}


// ---------- PXC 界面语言（Preferences/<构建号>/keys.json 的 "local"） ----------
// 生成语言文件只是把文件放好；PXC 到底显示哪个语言，记在它自己的偏好里。
// 所以"生成完还要手动去 PXC 里点一次"这件事可以省掉 —— 只要 PXC 没在运行。
// （变量与声明在文件上方，onGenerate 生成完要直接改它。）
static void refreshPxcPrefs(bool force) {
    static std::string lastDir;
    static int lastCount = -1;
    const std::string& d = g_cfg.gen.user_dir;
    // 扫目录不算贵（Locale 与 Preferences 各一层），但没必要每帧做。
    if (!force && d == lastDir && lastCount == (int)g_pxcPrefs.available.size()) return;
    lastDir = d;
    g_pxcPrefs = pxc_prefs::probe(pxc_util::pathFromUtf8(d));
    lastCount = (int)g_pxcPrefs.available.size();

    g_locLangItems = g_pxcPrefs.available;
    // 当前设置值与"生成目标名"也放进候选：前者可能指向一个已被删掉的目录，
    // 后者可能还没生成（列出来才不会让人以为没生效）。
    for (const std::string& s : {g_pxcPrefs.current, g_langDir.get()}) {
        if (s.empty()) continue;
        if (std::find(g_locLangItems.begin(), g_locLangItems.end(), s) == g_locLangItems.end())
            g_locLangItems.push_back(s);
    }
    if (g_locLangItems.empty()) g_locLangItems.push_back(g_langDir.get().empty() ? "zh" : g_langDir.get());
    g_locLangSel = -1;
    for (int i = 0; i < (int)g_locLangItems.size(); ++i)
        if (g_locLangItems[(size_t)i] == g_pxcPrefs.current) {
            g_locLangSel = i;
            break;
        }
}

static void onPickPxcLang(int idx) {
    if (idx < 0 || idx >= (int)g_locLangItems.size()) return;
    const std::string lang = g_locLangItems[(size_t)idx];
    // 注意：**先不推进 g_locLangSel**。原来的写法在读三道门之前就推进，失败时下拉显示
    // "已选中"、文件里还是旧值 —— 用户看到的是"改成功了但没生效"（§8.5.1 的根因）。
    // 现在由 applyPxcLang 在写成功后统一刷新（refreshPxcPrefs 会重算选中项）。
    applyPxcLang(lang);
    requestUpdate();
}

// ---------- 设置页（docs/06）----------
// 两个页签：通用（长期配置，从别的页面搬过来）/ 界面（两个缩放滑块）。
// 这一页已无任何弹层控件（下拉全换滑块、主题色这版不做），所以不用 ddField / overlay。
static int g_setTab = 0;   // 0 通用 / 1 界面

// 界面缩放：75%~200%，5% 一档（26 档）。
// uiScale 只是渲染变换（不进字号），26 档不会撑爆字形图集（docs/06 §10.4.2.1）。
static constexpr int kUiScaleCount = 26;
static float setUiScaleAt(int idx) { return 0.75f + 0.05f * (float)idx; }
static int setUiScaleIndex(float s) {
    int idx = (int)((s - 0.75f) / 0.05f + 0.5f);
    if (idx < 0) idx = 0;
    if (idx >= kUiScaleCount) idx = kUiScaleCount - 1;
    return idx;
}

// 文本缩放：**相对基准的倍率**（基准见 store.h 的 kTextBaseScale = 1.4）。
// 100% = 框架原始字号 × 1.4 —— 也就是原来要拉 1.4 才顺眼的那个大小。
// **必须离散** —— 字形图集全局单页且无淘汰，每换一个字号就多一批未缓存字形；
// 连续拖动必爆，溢出后文字永久消失（§10.4.2.1）。
// 档位：75%~150%，5% 一档，共 16 档（= 绝对字号 1.05~2.10 倍原始值）。
// **档位不受布局约束**：某个档位下出现折行、挤压、截断，说明那页的文本内容和排版要改，
// 不是要砍掉这个档位（docs/06 §10.4.2 的口径）。
static const float kTextScales[] = {0.75f, 0.80f, 0.85f, 0.90f, 0.95f, 1.00f, 1.05f, 1.10f,
                                    1.15f, 1.20f, 1.25f, 1.30f, 1.35f, 1.40f, 1.45f, 1.50f};
static constexpr int kTextScaleCount = (int)(sizeof(kTextScales) / sizeof(kTextScales[0]));
static constexpr int kTextScaleDefaultIdx = 5;   // 1.00 = 基准（绝对 1.40）
static int setTextScaleIndex(float v) {
    int best = 0;
    for (int i = 1; i < kTextScaleCount; ++i)
        if (std::fabs(kTextScales[i] - v) < std::fabs(kTextScales[best] - v)) best = i;
    return best;
}

// 滑块值 → 真正进内核的系数。**唯一**的合成点：界面里所有字号都走这里，
// 提示窗刻意不走（它是原生态，见 applyTextScale 里的说明）。
static float effectiveTextScale(float userScale) { return pxc_store::kTextBaseScale * userScale; }

// idx = 已生效；pending = 滑块当前位置。拖动中只动 pending（右侧数字跟着变），
// 松手才 apply —— 缩放一应用就整树重排，拖动中每帧都做会打滑（docs/06 §10.4.1）。
static int g_uiScaleIdx = 3;
static int g_uiScalePending = 3;
static int g_textScaleIdx = kTextScaleDefaultIdx;
static int g_textScalePending = kTextScaleDefaultIdx;

// 窗口同步放大后的确认条：10 秒内没按「保留」→ 窗口尺寸 + uiScale + 滑块位置一起回滚，且不写盘。
struct ScaleConfirm {
    bool open = false;
    float applied = 1.0f;                         // 这次试用的倍率
    float prev = 1.0f;                            // 试用前的倍率（回滚目标）
    int oldX = 0, oldY = 0, oldW = 0, oldH = 0;   // 试用前的外框矩形
    int newW = 0, newH = 0;                       // 试用后的实际客户区（可能被屏幕夹过）
    double deadlineMs = 0.0;
};
static ScaleConfirm g_scaleConfirm;
static constexpr double kScaleConfirmMs = 10000.0;   // 10 秒（docs/06 §10.8）

static bool setNear(float a, float b) { return std::fabs(a - b) < 0.001f; }

static void commitUiScale() {
    g_cfg.ui.scale = setUiScaleAt(g_uiScaleIdx);
    saveConfig();
    g_scaleConfirm.open = false;
    requestUpdate();
}

static void rollbackUiScale() {
    if (!g_scaleConfirm.open) return;
    // 三样一起回：窗口尺寸、uiScale、滑块位置。少回一样就是"数字回去了、界面还是大的"。
    mutableDslAppConfig().uiScale(g_scaleConfirm.prev);
    pxc_nui::setMainWindowRect(g_scaleConfirm.oldX, g_scaleConfirm.oldY,
                               g_scaleConfirm.oldW, g_scaleConfirm.oldH);
    g_uiScaleIdx = setUiScaleIndex(g_scaleConfirm.prev);
    g_uiScalePending = g_uiScaleIdx;
    g_scaleConfirm.open = false;
    requestUpdate();
}

// 松手时调用：先改窗口（用户得看到 150% 到底什么样才能判断），再让框架重排，最后挂确认条。
static void applyUiScale(int idx) {
    const float next = setUiScaleAt(idx);
    if (setNear(next, setUiScaleAt(g_uiScaleIdx))) return;   // 没变就别折腾窗口
    if (!g_scaleConfirm.open) {
        // 只有第一次尝试才记旧值；连续调两档时"回滚目标"仍是最初那个。
        if (!pxc_nui::getMainWindowRect(g_scaleConfirm.oldX, g_scaleConfirm.oldY,
                                        g_scaleConfirm.oldW, g_scaleConfirm.oldH))
            return;   // 窗口还没建出来：不动
        g_scaleConfirm.prev = setUiScaleAt(g_uiScaleIdx);
    }
    g_uiScaleIdx = idx;
    g_uiScalePending = idx;
    mutableDslAppConfig().uiScale(next);   // 下一帧 logicalWidth 变 → 框架自动整树重排
    const int tw = (int)(1040.0f * next + 0.5f);
    const int th = (int)(720.0f * next + 0.5f);
    int aw = tw, ah = th;
    pxc_nui::setMainWindowClientSize(tw, th, aw, ah);
    g_scaleConfirm.open = true;
    g_scaleConfirm.applied = next;
    g_scaleConfirm.newW = aw;
    g_scaleConfirm.newH = ah;
    g_scaleConfirm.deadlineMs = wbNowMs() + kScaleConfirmMs;
    requestUpdate();
}

// 文本缩放不碰窗口，所以不挂确认条：直接生效 + 写盘（反正双击滑块能回默认档）。
static void applyTextScale(int idx) {
    const float v = kTextScales[idx];
    if (setNear(v, kTextScales[g_textScaleIdx])) return;
    g_textScaleIdx = idx;
    g_textScalePending = idx;
    g_cfg.ui.text_scale = v;                     // 配置里存**相对倍率**（基准是代码常量，不落盘）
    // 两处都要改：内核系数决定渲染，config 决定"下次 initialize() 时进内核什么值"。
    // 只改内核不改 config，本次运行是对的、下次启动就退回 1.0（就是用户报的那个 bug）。
    mutableDslAppConfig().textScale(effectiveTextScale(v));
    setTextScale(effectiveTextScale(v));         // 内核系数 + 强制重排（字号在 compose 里算好，不重排不生效）
    // 提示窗是原生 GDI（独立线程独立消息泵），不在 EUI 的 text pipeline 里，
    // fontSize 系数对它无效 —— 倍率得单独转交一份。**只给相对倍率**：它有自己的设计尺寸，
    // 不该跟着 EUI 那个 1.4 的基准一起放大（否则 100% 时提示窗比正文还大）。
    pxc_runner::setToastTextScale(v);
    saveConfig();
}

// 主题色立即生效：只换 primary 一个 token，不涉及任何布局，所以不需要确认条 / 不需要重排窗口，
// 下一帧所有组件自然取到新色（uiTheme() 每帧现算）。
static eui::Signal<bool> g_accentPickerOpen;   // 取色弹窗开合；必须跨帧存活

// core::Color（0..1）→ COLORREF 的 0x00BBGGRR。提示窗是原生 GDI 窗、不在 EUI 的 UI 树里，
// 拿不到 uiTheme()，主题色只能这样转一份给它（见 pxc_toast::setAccent）。
static unsigned int accentRgb() {
    auto c8 = [](float v) {
        return (unsigned int)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
    };
    return (c8(g_accent.b) << 16) | (c8(g_accent.g) << 8) | c8(g_accent.r);
}

static void applyAccent(core::Color c) {
    if (std::fabs(c.r - g_accent.r) < 0.002f && std::fabs(c.g - g_accent.g) < 0.002f &&
        std::fabs(c.b - g_accent.b) < 0.002f) return;
    g_accent = c;
    g_cfg.ui.accent = colorToHex(c);
    pxc_runner::setToastAccent(accentRgb());   // 提示窗描边跟着换
    saveConfig();
    requestUpdate();
}

// ---------- 设置页控件（统一规范）----------
// 这一页原来"花"是因为各自为政：页签是蓝底 pill、开关是带 ✓ 的 pill、按钮 26 高、
// 标签 13 号、说明 11.5 号且各用一档灰。现在收敛成一套尺子：
//   字号只留 28 / 14 / 12 三档；灰只留一档 {0.55,0.60,0.68}；
//   控件统一 34 高；标签列固定 120 宽（与汉化页同一把尺子）；控件列左缘统一 = pad + 120。
static constexpr float kSetLabelW = 120.0f;
static constexpr float kSetCtrlH = 34.0f;

static core::Color setTextMain() { return {0.88f, 0.91f, 0.95f, 1.0f}; }
static core::Color setTextDim() { return {0.55f, 0.60f, 0.68f, 1.0f}; }

// 字段标签：左列固定宽、与右侧控件同一行居中对齐。
static void setLabel(core::dsl::Ui& ui, const std::string& id, const std::string& text,
                     float x, float y, float h) {
    ui.text(id).position(x, y).size(kSetLabelW, h).text(text).fontSize(14.0f).lineHeight(20.0f)
        .color(setTextMain()).verticalAlign(core::VerticalAlign::Center).build();
}

// 辅助说明：12 号、一档灰、自动换行。
static void setHint(core::dsl::Ui& ui, const std::string& id, const std::string& text,
                    float x, float y, float w) {
    ui.text(id).position(x, y).size(w, 17.0f).text(text).fontSize(12.0f).lineHeight(17.0f)
        .color(setTextDim()).wrap(true).build();
}

// 「关于信息」里的信息行，统一两行式：第一行说明文字 + 右侧按钮，第二行 12 号灰字放
// 具体值（地址 / 群号）。细节**不能和说明并成一行**：这一列文字区只有 innerW-90（右边
// 78 px 留给按钮），而 ui.text 不裁剪 —— 长 URL 会直接压到按钮底下。实测 PXC 那行合成
// 时 813 px、版本行 847 px，可用只有 686 px。
// 按钮锚在右边缘（pad + innerW - 78）而不是跟在文字后面：几行的按钮左沿因此对齐成一条
// 竖线，跟着文字宽度浮动会显得参差。
static void setAboutRow(core::dsl::Ui& ui, const std::string& id, const std::string& label,
                        const std::string& detail, const std::string& btnText,
                        std::function<void()> fn, float pad, float innerW, float& y) {
    const auto th = uiTheme();
    ui.text(id).position(pad, y).size(innerW - 90.0f, kSetCtrlH)
        .text(label).fontSize(14.0f).lineHeight(20.0f).color(setTextMain())
        .verticalAlign(core::VerticalAlign::Center).build();
    ui.stack(id + ".act.wrap").position(pad + innerW - 78.0f, y).size(72.0f, kSetCtrlH).content([&] {
        components::button(ui, id + ".act").theme(th, false).size(72.0f, kSetCtrlH)
            .text(btnText).fontSize(13.0f)
            .onClick(std::move(fn)).build();   // 按值捕获：按钮回调是延后执行的
    }).build();
    y += kSetCtrlH + 4.0f;
    ui.text(id + ".sub").position(pad, y).size(innerW, 18.0f)
        .text(detail).fontSize(12.0f).lineHeight(16.0f).color(setTextDim())
        .verticalAlign(core::VerticalAlign::Center).build();
    y += 18.0f + 10.0f;
}

// 外链专用：第二行放 URL，按钮是「打开」。
static void setAboutLinkRow(core::dsl::Ui& ui, const std::string& id, const std::string& label,
                            const std::string& url, float pad, float innerW, float& y) {
    setAboutRow(ui, id, label, url, "打开", [url] { wbOpenUrl(url); }, pad, innerW, y);
}

// 页签：纯文字 + 选中项一条 2px 主色下划线。
// 不用 pill —— 左侧导航已经是"选中=蓝底圆角"，页面里再来一层蓝 pill 就重了，
// 而且 pill 的形状跟按钮一样，会让人以为"点一下会执行什么"。返回占用宽度便于连续排布。
static float setTab(core::dsl::Ui& ui, const std::string& id, const std::string& label,
                    bool on, float x, float y, int tab) {
    const auto th = uiTheme();
    const float w = core::TextPrimitive::measureTextWidth(label, {}, 14.0f * textScale(), on ? 700 : 400) + 6.0f;
    ui.stack(id + ".wrap").position(x, y).size(w, 30.0f).content([&, id, label, on, w, tab, th] {
        ui.rect(id + ".hit").size(w, 30.0f).color({0.0f, 0.0f, 0.0f, 0.0f}).onClick([tab] {
            if (g_setTab == tab) return;
            g_setTab = tab;
            requestUpdate();
        }).build();
        ui.text(id + ".t").size(w, 30.0f).text(label).fontSize(14.0f).lineHeight(30.0f)
            .fontWeight(on ? 700 : 400)
            .color(on ? core::Color{0.95f, 0.97f, 1.0f, 1.0f} : setTextDim())
            .horizontalAlign(core::HorizontalAlign::Center)
            .verticalAlign(core::VerticalAlign::Center).build();
        if (on)
            ui.rect(id + ".ul").x(0.0f).y(28.0f).size(w, 2.0f).color(th.primary).radius(1.0f).build();
    }).build();
    return w;
}

// 开关行：左侧用框架自带 Switch，右侧同一行跟一句说明。
// Switch 自己会画 label，所以外框宽度必须把 label 算进去（轨道 + 间距 + 文字 + 内边距）。
static void setSwitchRow(core::dsl::Ui& ui, const std::string& id, const std::string& label,
                         const std::string& hint, bool on, float x, float y, float hintW,
                         std::function<void(bool)> onChange) {
    const auto th = uiTheme();
    const float swW = 40.0f + 12.0f + core::TextPrimitive::measureTextWidth(label, {}, 13.0f * textScale(), 400) + 14.0f;
    ui.stack(id + ".wrap").position(x, y).size(swW, kSetCtrlH)
        .content([&, id, label, on, swW, onChange, th] {
            components::toggleSwitch(ui, id).theme(th).size(swW, kSetCtrlH)
                .trackSize(40.0f, 22.0f).fontSize(13.0f).checked(on).text(label)
                .onChange(onChange).build();
        }).build();
    setHint(ui, id + ".hint", hint, x + swW + 16.0f, y + 9.0f, hintW);
}

static constexpr const char* kLauncherVersion = "v0.0.3";   // 启动器版本号（关于信息展示，换版本只改这里）
// 项目仓库（设置页「关于信息」里的跳转目标）。放在版本号旁边：发版时两处一起看，
// 免得版本号改了、链接还停在旧仓库。
static constexpr const char* kLauncherRepoUrl = "https://github.com/TheMorningCat/PixelComposer-Launcher/tree/main";

// 设置页头部高度：标题 28→80、页签 80→118、分隔线、间距 →144。
//
// 这个数**同时**是正文滚动视口的上沿（见 compose 里 page.host 的 position）——
// 头部固定不滚动、正文单独滚，就是靠它把两块拼起来。改头部排版必须一起改这里，
// 否则正文要么顶到页签上，要么在分隔线下面空出一条。
static constexpr float kSetHeaderH = 144.0f;

// 设置页头部：标题 / 页签 / 分隔线。画在滚动视口**上方**，不参与滚动。
static void composeSettingsHeader(core::dsl::Ui& ui, float width) {
    const float pad = 32.0f;
    const float innerW = width - pad * 2.0f;
    const auto th = uiTheme();

    float y = 28.0f;
    ui.text("set.title").position(pad, y).size(innerW, 40.0f).text("设置")
        .fontSize(28.0f).lineHeight(34.0f).fontWeight(800).color({0.95f, 0.97f, 1.0f, 1.0f}).build();
    y += 52.0f;

    float tx = pad;
    tx += setTab(ui, "set.tab0", "通用", g_setTab == 0, tx, y, 0) + 26.0f;
    tx += setTab(ui, "set.tab1", "界面", g_setTab == 1, tx, y, 1) + 26.0f;
    setTab(ui, "set.tab2", "汉化", g_setTab == 2, tx, y, 2);
    y += 38.0f;
    ui.rect("set.line").position(pad, y).size(innerW, 1.0f)
        .color(components::theme::withOpacity(th.border, 0.8f)).build();
    y += 26.0f;
}

// 设置页正文：页签以下的全部控件。y 从 0 起 —— 头部占的那 144 由滚动视口自己让出来
// （kSetHeaderH），这里不再重复偏移，否则内容会白掉一段。
static void composeSettingsBody(core::dsl::Ui& ui, float width) {
    const float pad = 32.0f;
    const float innerW = width - pad * 2.0f;
    const float ctrlX = pad + kSetLabelW;        // 控件列左缘：这一页所有控件都从这里开始
    const float ctrlW = innerW - kSetLabelW;     // 控件列可用宽度
    const auto th = uiTheme();
    wbTick();   // 顺带刷新 .pxc 关联状态（2 秒一次）+ 落盘节流

    float y = 0.0f;

    if (g_setTab == 0) {
        // ---- PXC 用户目录（跨页配置，从汉化页搬来）----
        setLabel(ui, "set.ud.lab", "PXC 用户目录", pad, y, kSetCtrlH);
        ui.stack("set.ud.wrap").position(ctrlX, y).size(112.0f, kSetCtrlH).content([&] {
            components::button(ui, "set.ud").theme(th, false).size(112.0f, kSetCtrlH).text("选择目录")
                .onClick([] { guarded("选择用户目录", onPickUserDir); }).build();
        }).build();
        ui.stack("set.uda.wrap").position(ctrlX + 120.0f, y).size(96.0f, kSetCtrlH).content([&] {
            components::button(ui, "set.uda").theme(th, false).size(96.0f, kSetCtrlH).text("自动查找")
                .onClick([] { guarded("自动查找用户目录", onAutoUserDir); }).build();
        }).build();
        ui.text("set.ud.v").position(ctrlX + 232.0f, y).size(std::max(120.0f, ctrlW - 232.0f), kSetCtrlH)
            .text(g_cfg.gen.user_dir.empty() ? "(未选择，通常是 %LOCALAPPDATA%\\PixelComposer)" : g_cfg.gen.user_dir)
            .fontSize(12.0f).lineHeight(16.0f).color(setTextDim())
            .verticalAlign(core::VerticalAlign::Center).wrap(true).build();
        y += kSetCtrlH + 10.0f;
        setHint(ui, "set.ud.hint", "汉化文件写到这里；工程页的「自动恢复」也从这里读。",
                ctrlX, y, ctrlW);
        y += 38.0f;

        // ---- .pxc 文件关联 ----
        setLabel(ui, "set.as.lab", "pxc 文件关联", pad, y, kSetCtrlH);
        {
            const bool ours = (g_wbAssoc.state == pxc_assoc::State::Ours);
            const char* btnLabel = ours ? "取消关联"
                                        : (g_wbAssoc.state == pxc_assoc::State::Other ? "打开默认应用" : "关联 .pxc");
            const float bw = (g_wbAssoc.state == pxc_assoc::State::Other) ? 112.0f : 96.0f;
            ui.stack("set.as.wrap").position(ctrlX, y).size(bw, kSetCtrlH).content([&, bw, btnLabel] {
                components::button(ui, "set.as").theme(th, false).size(bw, kSetCtrlH).text(btnLabel)
                    .onClick([] { guarded("切换 .pxc 关联", wbToggleAssoc); }).build();
            }).build();
            // 状态就跟在按钮右边，不再单独占一行 —— 它只是这个按钮的结果。
            std::string st = "未关联";
            if (ours) {
                st = "已关联";
            } else if (g_wbAssoc.state == pxc_assoc::State::Other) {
                st = "被其它程序占用";
                if (!g_wbAssoc.otherName.empty()) st += "（" + g_wbAssoc.otherName + "）";
            }
            const bool warn = (g_wbAssoc.state == pxc_assoc::State::Other);
            ui.text("set.as.v").position(ctrlX + bw + 14.0f, y).size(ctrlW - bw - 14.0f, kSetCtrlH).text(st)
                .fontSize(12.0f).lineHeight(16.0f)
                .color(warn ? core::Color{0.85f, 0.65f, 0.40f, 1.0f} : setTextDim())
                .verticalAlign(core::VerticalAlign::Center).build();
        }
        y += kSetCtrlH + 10.0f;
        setHint(ui, "set.as.hint", "双击 .pxc 用启动器打开（系统还会问一次「始终使用」）。",
                ctrlX, y, ctrlW);
        y += 34.0f;

        // ---- 启动行为（两个真 Switch）----
        setLabel(ui, "set.tg.lab", "启动行为", pad, y, 26.0f);
        setSwitchRow(ui, "set.tg.silent", "双击 .pxc 时静默启动",
                     "拉起 PXC 后启动器缩在任务栏，不弹窗抢焦点。",
                     g_wbStore.assoc.silentLaunch, ctrlX, y - 4.0f, ctrlW - 250.0f,
                     [](bool v) {
                         g_wbStore.assoc.silentLaunch = v;
                         wbSaveSoon();
                         requestUpdate();
                     });
        y += 42.0f;
        setSwitchRow(ui, "set.tg.reuse", "复用已打开的 PXC",
                     "同版本已在运行时直接在它里面开新标签，不启第二个进程。",
                     g_wbStore.assoc.reuseInstance, ctrlX, y - 4.0f, ctrlW - 250.0f,
                     [](bool v) {
                    g_wbStore.assoc.reuseInstance = v;
                        wbSaveSoon();
                        requestUpdate();
                    });
        y += 42.0f;

        // ---- 底部信息：QQ 交流群 / 启动器版本 ----
        y += 14.0f;
        ui.rect("set.about.line").position(pad, y).size(innerW, 1.0f)
            .color(components::theme::withOpacity(th.border, 0.8f)).build();
        y += 22.0f;

        // 关于信息里的行，统一走 setAboutRow（说明 + 右侧按钮 / 具体值另起一行）。
        // 顺序：QQ 交流群 → 启动器版本 → PXC 原项目 → UI 框架 → 作者主页。
        setAboutRow(ui, "set.about.qq", "PixelComposer QQ 交流群", "965593104", "复制",
                    [] {
                        wbCopyToClipboard("965593104");
                        showToast("已复制 QQ 群号", "965593104 已复制到剪贴板", false);
                    }, pad, innerW, y);
        setAboutLinkRow(ui, "set.about.ver", std::string("启动器版本 ") + kLauncherVersion,
                        kLauncherRepoUrl, pad, innerW, y);
        setAboutLinkRow(ui, "set.about.pxc", "特效编辑器 Pixel-Composer 项目链接",
                        "https://github.com/Ttanasart-pt/Pixel-Composer", pad, innerW, y);
        setAboutLinkRow(ui, "set.about.eui", "UI 框架 EUI-NEO 项目链接",
                        "https://github.com/sudoevolve/EUI-NEO", pad, innerW, y);
        setAboutLinkRow(ui, "set.about.author", "作者：凌晨猫  B 站主页",
                        "https://space.bilibili.com/9170538", pad, innerW, y);

        // 汉化作者（随包维护中文语言文件的人，与上面的 PXC 原作者不是同一个人，所以单独一行）
        ui.text("set.about.loc.author").position(pad, y).size(innerW, kSetCtrlH)
            .text("汉化作者：WWNNL").fontSize(14.0f).lineHeight(20.0f)
            .color(setTextMain()).verticalAlign(core::VerticalAlign::Center).build();
        y += kSetCtrlH;
    } else if (g_setTab == 1) {
        // ---- 界面缩放 ----
        setLabel(ui, "set.s1.lab", "界面缩放", pad, y, kSetCtrlH);
        ui.stack("set.s1.wrap").position(ctrlX, y + 6.0f).size(260.0f, 22.0f).content([&] {
            components::slider(ui, "set.s1").theme(th).size(260.0f, 22.0f)
                .value((float)g_uiScalePending / (float)(kUiScaleCount - 1))
                .default(3.0f / (float)(kUiScaleCount - 1))   // 双击回 100%
                .onChange([](float nv) {
                    int idx = (int)(nv * (float)(kUiScaleCount - 1) + 0.5f);
                    idx = idx < 0 ? 0 : (idx >= kUiScaleCount ? kUiScaleCount - 1 : idx);
                    if (idx == g_uiScalePending) return;
                    g_uiScalePending = idx;   // 只换数字，绝不在这里 apply
                    requestUpdate();
                })
                .onRelease([] { applyUiScale(g_uiScalePending); })   // 松手才真的动
                .build();
        }).build();
        {
            char buf[24];
            snprintf(buf, sizeof(buf), "%d%%", (int)(setUiScaleAt(g_uiScalePending) * 100.0f + 0.5f));
            ui.text("set.s1.v").position(ctrlX + 272.0f, y).size(72.0f, kSetCtrlH).text(buf).fontSize(14.0f)
                .lineHeight(20.0f).color(setTextMain()).verticalAlign(core::VerticalAlign::Center).build();
        }
        y += kSetCtrlH + 10.0f;
        setHint(ui, "set.s1.hint",
                "拖动松手后才生效。窗口按同比例一起缩放；10 秒内没点「保留」自动恢复原状。",
                ctrlX, y, ctrlW);
        y += 44.0f;

        // ---- 文本缩放 ----
        setLabel(ui, "set.s2.lab", "文本缩放", pad, y, kSetCtrlH);
        ui.stack("set.s2.wrap").position(ctrlX, y + 6.0f).size(260.0f, 22.0f).content([&] {
            components::slider(ui, "set.s2").theme(th).size(260.0f, 22.0f)
                .value((float)g_textScalePending / (float)(kTextScaleCount - 1))
                .default((float)kTextScaleDefaultIdx / (float)(kTextScaleCount - 1))   // 双击回 100%
                .onChange([](float nv) {
                    int idx = (int)(nv * (float)(kTextScaleCount - 1) + 0.5f);
                    idx = idx < 0 ? 0 : (idx >= kTextScaleCount ? kTextScaleCount - 1 : idx);
                    if (idx == g_textScalePending) return;
                    g_textScalePending = idx;
                    requestUpdate();
                })
                .onRelease([] { applyTextScale(g_textScalePending); })
                .build();
        }).build();
        {
            char buf[24];
            snprintf(buf, sizeof(buf), "%d%%", (int)(kTextScales[g_textScalePending] * 100.0f + 0.5f));
            ui.text("set.s2.v").position(ctrlX + 272.0f, y).size(72.0f, kSetCtrlH).text(buf).fontSize(14.0f)
                .lineHeight(20.0f).color(setTextMain()).verticalAlign(core::VerticalAlign::Center).build();
        }
        y += kSetCtrlH + 10.0f;
        setHint(ui, "set.s2.hint",
                "只放大文字，按钮与卡片尺寸不变，所以极端档位下文字会显得拥挤。范围 75%~150%，5% 一档："
                "连续变化的字号会把字形图集撑爆，文字会直接消失。提示窗只跟这个倍率，不跟字号基准。",
                ctrlX, y, ctrlW);
        y += 60.0f;

        // ---- 主题色 ----
        setLabel(ui, "set.ac.lab", "主题色", pad, y, kSetCtrlH);
        // 色块只做预览、不可点：能点的东西要长得像按钮，色块长得像"当前值"。
        ui.stack("set.ac.prev").position(ctrlX, y).size(kSetCtrlH, kSetCtrlH).content([&, th] {
            ui.rect("set.ac.prev.bg").size(kSetCtrlH, kSetCtrlH).color(g_accent).radius(10.0f)
                .border(1.0f, components::theme::withOpacity(th.border, 0.9f)).build();
        }).build();
        ui.text("set.ac.v").position(ctrlX + 46.0f, y).size(96.0f, kSetCtrlH).text(colorToHex(g_accent))
            .fontSize(13.0f).lineHeight(18.0f).color(setTextMain())
            .verticalAlign(core::VerticalAlign::Center).build();
        ui.stack("set.ac.pick.wrap").position(ctrlX + 142.0f, y).size(72.0f, kSetCtrlH).content([&] {
            components::button(ui, "set.ac.pick").theme(th, false).size(72.0f, kSetCtrlH).text("更改")
                .fontSize(13.0f)
                .onClick([] { g_accentPickerOpen.set(true); requestUpdate(); }).build();
        }).build();
        ui.stack("set.ac.def.wrap").position(ctrlX + 222.0f, y).size(84.0f, kSetCtrlH).content([&] {
            components::button(ui, "set.ac.def").theme(th, false).size(84.0f, kSetCtrlH).text("恢复默认")
                .fontSize(13.0f)
                .onClick([] {
                    core::Color d;
                    if (hexToColor(kAccentDefault, d)) applyAccent(d);
                }).build();
        }).build();
        y += kSetCtrlH + 10.0f;
        setHint(ui, "set.ac.hint",
                "只换高亮色：按钮、选中态、滑块进度、开关、导航下划线。灰阶背景与正文颜色不跟着变。",
                ctrlX, y, ctrlW);
        y += 40.0f;
    } else {
        // ---- 汉化分页（docs/08 §8.10）----
        // 这里只放**维护者工具**与它的一句说明。「启动前自动生成」不在这一页 ——
        // 它属于"这次怎么生成"，已经搬到汉化页分组卡第一行（§8.2.5），别往这里再加一遍。
        // 「更新汉化文件」「自动获取汉化更新」两项的语义已定但还没做（下载渠道未定），
        // 按项目纪律不画死按钮：不可用的控件比没有更糟。
        y += 6.0f;
        setLabel(ui, "set.bld.lab", "对照表构建器", pad, y, kSetCtrlH);
        ui.stack("set.bld.wrap").position(ctrlX, y).size(170.0f, kSetCtrlH).content([&] {
            components::button(ui, "set.bld").theme(th, false).size(170.0f, kSetCtrlH)
                .text("打开对照表构建器").fontSize(13.0f)
                .onClick([] { guarded("打开构建器", onBldOpen); }).build();
        }).build();
        y += kSetCtrlH + 10.0f;
        setHint(ui, "set.bld.hint",
                "维护翻译用的：从英文骨架与现有汉化包生成 data/translation_db.json。普通用户不需要打开它。",
                ctrlX, y, ctrlW);
        y += 40.0f;

        setLabel(ui, "set.dbinfo.lab", "当前对照表", pad, y, kSetCtrlH);
        ui.text("set.dbinfo.v").position(ctrlX, y).size(ctrlW, kSetCtrlH).text(dbLabel())
            .fontSize(12.0f).lineHeight(16.0f)
            .color(g_dbLoaded ? setTextDim() : core::Color{0.85f, 0.65f, 0.40f, 1.0f})
            .verticalAlign(core::VerticalAlign::Center).wrap(true).build();
        y += kSetCtrlH + 10.0f;
        setHint(ui, "set.dbinfo.hint",
                "骨架永远取自你已装的那个 PXC 版本，换对照表不会影响它。生成语言文件在左侧「汉化」页。",
                ctrlX, y, ctrlW);
        y += 40.0f;
    }

    g_pageH = y + 90.0f;
}

// 缩放确认条：固定在主窗**左下角**（x+16, 底边上 16）。
// 为什么不是左上角（docs/06 §10.4.1.1 原方案）：左上角正好压住页面标题和页签，
// 而这条要挂 10 秒、期间用户还要能正常操作 —— 压住页签等于把"不拦任何操作"变成了空话。
// 左下角同样满足"一定看得见"：我们自己把窗口夹到了工作区，底边一定在屏内（原点那一侧不用赌。
static void composeScaleConfirm(core::dsl::Ui& ui, const eui::Screen& screen) {
    if (!g_scaleConfirm.open) return;
    const float barY = screen.height - 72.0f - 16.0f - 30.0f;   // -30：screen.height 是含标题栏的外框
    const auto th = uiTheme();
    int msLeft = (int)(g_scaleConfirm.deadlineMs - wbNowMs());
    if (msLeft < 0) msLeft = 0;
    int secs = (msLeft + 999) / 1000;
    // 用 std::to_string 拼而不是 snprintf：格式串里带全角括号与 %% 时 MSVC 的参数计数会错位（C4474）。
    const std::string line = "界面缩放已改为 " + std::to_string((int)(g_scaleConfirm.applied * 100.0f + 0.5f)) +
                             "%（窗口 " + std::to_string(g_scaleConfirm.newW) + "×" +
                             std::to_string(g_scaleConfirm.newH) + "）　这个大小合适吗？";
    const std::string undoLabel = "恢复（" + std::to_string(secs) + "）";

    ui.stack("set.cf.bgwrap").position(16.0f, barY).size(560.0f, 72.0f).content([&, th] {
        ui.rect("set.cf.bg").size(560.0f, 72.0f).color({0.165f, 0.175f, 0.205f, 0.98f}).radius(12.0f)
            .border(1.0f, components::theme::withOpacity(th.border, 0.9f)).build();
    }).build();
    ui.text("set.cf.t").position(30.0f, barY + 22.0f).size(350.0f, 44.0f).text(line).fontSize(12.0f)
        .lineHeight(18.0f).color(setTextMain()).wrap(true).build();
    ui.stack("set.cf.ok.wrap").position(396.0f, barY + 38.0f).size(80.0f, 28.0f).content([&, th] {
        components::button(ui, "set.cf.ok").theme(th).size(80.0f, 28.0f).text("保留").fontSize(13.0f)
            .onClick([] { commitUiScale(); }).build();
    }).build();
    ui.stack("set.cf.no.wrap").position(486.0f, barY + 38.0f).size(80.0f, 28.0f).content([&, th, undoLabel] {
        components::button(ui, "set.cf.no").theme(th, false).size(80.0f, 28.0f).text(undoLabel).fontSize(13.0f)
            .onClick([] { rollbackUiScale(); }).build();
    }).build();
}

// 主题色取色弹窗。与其它浮窗**平级**放在根级，不嵌进任何 dialog 的 content ——
// 两层全屏 overlay 嵌套时"谁在上面"就退化成由创建顺序决定，而 colorPicker 与 dialog
// 的默认 zIndex 都是 1000，所以这里显式抬到 1100（DLSS5 项目踩过这一条）。
// 「点外部关闭 / 取色时滚轮不透传下层 / 点完成才提交」全是组件内建，不用自己写。
static void composeAccentPicker(core::dsl::Ui& ui, const eui::Screen& screen) {
    ui.stack("acc.host").size(screen.width, screen.height).position(0.0f, 0.0f).ignoreLayout(true)
        .content([&] {
            components::colorPicker(ui, "acc.picker")
                .screen(screen.width, screen.height)
                .size(420.0f, 330.0f)
                .bindOpen(g_accentPickerOpen)
                .theme(uiTheme())      // 弹窗内的强调色（完成按钮、色块选中环）也跟主题走
                .value(g_accent)
                .title("主题色")
                .confirm("完成")
                .zIndex(1100)
                .onChange([](core::Color c) { applyAccent(c); })
                .build();
        }).build();
}

// ---------- 汉化页（同一把尺子）----------
// 排版照 `参考图/参考图-汉化.png`（docs/08 §8.2）：A 标题 → B 状态行（PXC 界面语言 + 打开语言目录）
// → C 小节头（自定义生成语言 + 导入/导出偏好）→ D 分组卡（"这次怎么生成"的全部参数）→ 没有尾巴。
// 尺子沿用设置页：控件 34 高、标签列 96、灰只留一档、一页只有一个主色实心按钮。
// 上一版那两段常驻状态文字（`loc.stat` / `loc.status`）已经删了 —— 结果一律走 toast（§8.6.3），
// 页面上留一段"上次生成了什么"只会让人以为是本次状态。
static constexpr float kLocCtrlH = 34.0f;
static constexpr float kLocBtnFont = 13.0f;
static constexpr float kLocLabelW = 96.0f;     // 卡内标签列宽（§8.2.2）
static constexpr float kLocRowGap = 10.0f;     // 卡内行距
static constexpr float kLocCardPad = 16.0f;    // 卡内 padding
static constexpr float kLocSubIndent = 40.0f;  // 「文本设置」分支再缩进多少
static constexpr float kLocColGap = 20.0f;     // 两列之间的空档

static void locButton(core::dsl::Ui& ui, const std::string& id, float x, float y, float w,
                      const std::string& label, bool primary, std::function<void()> fn,
                      unsigned icon = 0) {
    const auto th = uiTheme();
    ui.stack(id + ".wrap").position(x, y).size(w, kLocCtrlH)
        .content([&, id, w, label, primary, icon, th, fn] {
            if (icon != 0)
                components::button(ui, id).theme(th, primary).size(w, kLocCtrlH).text(label)
                    .fontSize(kLocBtnFont).icon(icon).iconSize(12.0f).onClick(fn).build();
            else
                components::button(ui, id).theme(th, primary).size(w, kLocCtrlH).text(label)
                    .fontSize(kLocBtnFont).onClick(fn).build();
        }).build();
}

// 「生成并应用」在自动模式下换态：实心 → 主色描边（§8.2.5.1）。
// 为什么不是换个颜色：一页只允许一个主色实心按钮，换色相会破坏这套纪律；"实心 → 描边"
// 既保住主色语义（还是它），又能一眼看出"这个动作已被接管"。
static void locGenButton(core::dsl::Ui& ui, float x, float y, float w, bool autoMode,
                         bool noVersion, std::function<void()> fn) {
    const auto th = uiTheme();
    const std::string label = noVersion ? "生成并应用" : (autoMode ? "已开启自动生成" : "生成并应用");
    ui.stack("loc.gen.wrap").position(x, y).size(w, kLocCtrlH)
        .content([&, label, w, autoMode, noVersion, th, fn] {
            if (noVersion) {
                // 空态：一个版本都取不到骨架，生成必然失败 —— 按钮直接置灰（§8.2.4 空态）。
                ui.rect("loc.gen.off").size(w, kLocCtrlH).color(th.surface)
                    .radius(12.0f).border(1.0f, components::theme::withOpacity(th.border, 0.7f))
                    .opacity(0.55f).build();
                ui.text("loc.gen.off.t").size(w, kLocCtrlH).text(label).fontSize(kLocBtnFont)
                    .lineHeight(kLocCtrlH).color(setTextDim())
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center).build();
                return;
            }
            auto b = components::button(ui, "loc.gen").theme(th, !autoMode).size(w, kLocCtrlH)
                         .text(label).fontSize(kLocBtnFont);
            if (autoMode) b.border(1.0f, th.primary);
            b.onClick(fn).build();
        }).build();
}

// 卡内标签：14 / 主色字，与右侧控件同高居中，固定 96 宽。
static void locLabel(core::dsl::Ui& ui, const std::string& id, const std::string& text,
                     float x, float y) {
    ui.text(id).position(x, y).size(kLocLabelW, kLocCtrlH).text(text).fontSize(14.0f)
        .lineHeight(20.0f).color(setTextMain())
        .verticalAlign(core::VerticalAlign::Center).build();
}

// 卡内 12 号说明字（跟在控件右边，与控件同高居中）。颜色可传，默认用那一档灰。
static void locNote(core::dsl::Ui& ui, const std::string& id, const std::string& text,
                    float x, float y, float w, core::Color color = {0.55f, 0.60f, 0.68f, 1.0f}) {
    ui.text(id).position(x, y).size(std::max(0.0f, w), kLocCtrlH).text(text).fontSize(12.0f)
        .lineHeight(16.0f).color(color)
        .verticalAlign(core::VerticalAlign::Center).wrap(true).build();
}

// 「!」：16px 圆底 + 居中叹号，仅多版本时出现（§8.2.4）。
// 常态是次要灰，悬停转 accent —— 它是"提示"，不是"警告"，常态不用橙/红。
// 浮窗画在根级（composeLocaleTip），因为页内容在 scrollView 的裁剪画布里。
static void locExclaim(core::dsl::Ui& ui, float px, float py, bool dim) {
    const auto th = uiTheme();
    const float s = 16.0f;
    const float cy = py + (kLocCtrlH - s) * 0.5f;
    // 自动模式下整枚变暗（"你选了也不生效"），但别暗到看不出来它是个圆 —— 0.45 那档在卡底色上
    // 几乎等于没有，照片里就剩一个孤零零的感叹号。
    const core::Color accent = dim ? components::theme::withOpacity(th.primary, 0.60f) : th.primary;
    const core::Color edge = g_locExclaimHover
                                 ? accent
                                 : components::theme::withOpacity(th.border, dim ? 0.70f : 0.9f);
    ui.stack("loc.tv.help").position(px, cy).size(s, s).zIndex(9)
        .content([&, s, px, cy, dim, accent, edge, th] {
            // 圆底要比卡底色**亮一档**：卡底色本身是 surface 提亮 5%，用 surface 当底
            // 会在卡上挖出一个黑洞，孤零零一个"!"看着像渲染错误（旁边已经没有文字了）。
            ui.rect("loc.tv.help.o").size(s, s)
                .color(components::theme::withOpacity(dim ? th.surface : th.surfaceHover, 0.95f))
                .radius(s * 0.5f).border(1.0f, edge).build();
            ui.text("loc.tv.help.t").size(s, s).text("!").fontSize(12.0f).lineHeight(s).fontWeight(700)
                .color(g_locExclaimHover ? accent : (dim ? components::theme::withOpacity(setTextDim(), 0.8f)
                                                         : setTextDim()))
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center).build();
            // 探 hover：透明命中区。坐标要换成客户区（卡片在画布里，得加导航宽、减滚动量）。
            components::mouseArea(ui, "loc.tv.help.hit").position(0.0f, 0.0f).size(s, s)
                .cursor(core::CursorShape::Arrow)
                .onHover([px, cy](bool on) {
                    g_locExclaimHover = on;
                    if (on)
                        locTipShow("多版本共用一个汉化文件：按某一版生成、在另一版里用，端口名可能对不上"
                                   "（会显示成同节点里别的参数）。\n"
                                   "打开上面的「自动生成」，每次启动前都会按它自己的版本重新生成一次。",
                                   kNavW + px + 16.0f, cy - g_pageScroll.get() + 16.0f, 320.0f);
                    else
                        locTipHide();
                }).build();
        }).build();
}

// 开/关「自动生成」（汉化卡第 ① 行，左标题 + 右开关）。开关本体和左边的标题都能点，
// 两条路走同一段逻辑；标题上挂悬停说明（命中区在 compose 那边，id 用 loc.auto.title）。
static void setAutoGen(bool v) {
    if (g_cfg.gen.auto_gen_on_launch == v) return;
    g_cfg.gen.auto_gen_on_launch = v;
    saveConfig();
    requestUpdate();
}

static void composeLocale(core::dsl::Ui& ui, float width) {
    const float pad = 28.0f;
    const float x = pad;
    const float innerW = width - pad * 2.0f;
    const auto th = uiTheme();
    const auto dim = setTextDim();

    // 已装版本清单只在"进页 / 登记表变了"时重建：数节点要读各版本 nodes.json，
    // Steam 版还要解一次 zip，绝不能进帧循环。
    refreshSkelOpts();
    // 只列**能取到骨架**的版本（§8.2.4「不可用不列」）：exe 被删 / 目录里没 en 的版本
    // 就算登记着也不该出现 —— 选了也生成不了，列出来只是噪音。
    std::vector<int> usable;
    for (int i = 0; i < (int)g_skelOpts.size(); ++i)
        if (g_skelOpts[(size_t)i].hasSkeleton) usable.push_back(i);
    const std::string target = effectiveTargetVersion();
    const bool multi = (int)usable.size() >= 2;                  // §8.2.5：多版本才出现自动生成那行
    const bool autoMode = g_cfg.gen.auto_gen_on_launch && multi;  // 单版本时开关不显示，也就不算"自动模式"

    float y = 24.0f;
    ui.text("loc.title").position(x, y).size(innerW, 40.0f).text("汉化")
        .fontSize(28.0f).lineHeight(34.0f).fontWeight(800).color({0.95f, 0.97f, 1.0f, 1.0f}).build();

    // ---- B 状态行：PXC 界面语言 + 打开语言目录 ----
    // 脚本语言（写哪个语言文件）不在这里 —— 它在卡里，因为它属于"这一次怎么生成"；
    // 界面语言是"PXC 现在显示什么"，属于页面级状态（§8.2.1）。
    y = 70.0f;
    refreshPxcPrefs();
    locLabel(ui, "loc.local.lab", "PXC 界面语言", x, y);
    ddField(ui, "loc.local", x + kLocLabelW, y, 220.0f, kLocCtrlH, g_locLangItems, g_locLangSel,
            [](int idx) { guarded("设置 PXC 界面语言", [idx] { onPickPxcLang(idx); }); },
            g_pxcPrefs.current.empty() ? "(未设置)" : g_pxcPrefs.current);
    locButton(ui, "loc.open", x + kLocLabelW + 232.0f, y, 136.0f, "打开语言目录", false,
              [] {
                  const std::filesystem::path d = pxc_util::pathFromUtf8(g_cfg.gen.user_dir);
                  std::error_code ec;
                  const std::filesystem::path t =
                      std::filesystem::is_directory(d / "Locale", ec) ? d / "Locale" : d;
                  if (!pxc_nui::revealInExplorer(t, false))
                      showToast("打不开语言目录", "先去「设置 → 通用」把 PXC 用户目录选对", true);
              },
              0xF07B);
    {
        const int uds = userDirState();
        const core::Color warn{0.85f, 0.65f, 0.40f, 1.0f};
        std::string hint;
        core::Color hc = dim;
        // 这一行只有 300 出头像素（下拉 220 + 按钮 136 之后），字少才放得下 ——
        // "当前 xx" 已经由下拉本身显示，这里不重复。
        if (uds == 2) {
            hint = "先去「设置 → 通用」选用户目录";
            hc = warn;
        } else if (uds == 1) {
            hint = "先启动一次 PXC 建出 Locale";
            hc = warn;
        } else if (!g_pxcPrefs.readable) {
            hint = "没找到 Preferences/keys.json";
            hc = warn;
        }
        // 一切正常时不留说明字（下拉里显示的就是真相）。
        const float hx = x + kLocLabelW + 232.0f + 136.0f + 14.0f;
        const float hw = x + innerW - hx;
        if (!hint.empty() && hw > 80.0f) locNote(ui, "loc.local.hint", hint, hx, y, hw, hc);
    }

    // ---- C 小节头：标题 + 对照表摘要 + 右端导入/导出 ----
    y = 122.0f;
    ui.text("loc.sec").position(x, y + 7.0f).size(150.0f, 20.0f).text("自定义生成语言")
        .fontSize(14.0f).lineHeight(20.0f).fontWeight(600).color(setTextMain()).build();
    const float expW = 108.0f;
    const float expX2 = x + innerW - expW;
    const float expX1 = expX2 - expW - 8.0f;
    // 偏好文件（覆盖差异 + 语言目录名 + 显示表达式）是整份设置，所以留在这一页 ——
    // 它不是"某一条译文的改法"，放进自定义译文浮窗就错了层级。
    locButton(ui, "loc.prexp", expX1, y, expW, "导出偏好…", false,
              [] { guarded("导出偏好", onOvExport); });
    locButton(ui, "loc.primp", expX2, y, expW, "导入偏好…", false,
              [] { guarded("导入偏好", onOvImport); });
    {
        const float mx = x + 158.0f;
        const float mw = expX1 - 16.0f - mx;
        if (mw > 60.0f) locNote(ui, "loc.dbmeta", dbLabel(), mx, y, mw,
                                g_dbLoaded ? dim : core::Color{0.85f, 0.65f, 0.40f, 1.0f});
    }

    // ---- D 分组卡 ----
    // 高度先算后画：卡底色必须作为**先画的兄弟节点**铺在下面（EUI 按创建顺序绘制），
    // 而它又要贴着内容自适应，所以行高与行距改成常量、在这里加一遍总和。
    y = 168.0f;
    const float cardX = x, cardY = y, cardW = innerW;
    const float cx = cardX + kLocCardPad;
    const float cw = cardW - kLocCardPad * 2.0f;
    const float hAuto = multi ? (kLocCtrlH + kLocRowGap) : 0.0f;
    const float hTopRows = 3.0f * (kLocCtrlH + kLocRowGap);    // 目标版本 / 语言名称 / 文本设置
    const float hExpr = 3.0f * (kLocCtrlH + kLocRowGap);       // 六项表达式（两列 × 三行）
    const float hPh = 20.0f + kLocRowGap;                      // 占位符说明（整行）
    const float hFont = kLocCtrlH + kLocRowGap;                // 界面字体 / 代码字体
    const float hPrev = 40.0f + kLocRowGap;                    // 预览条（整行，高 40）
    const float hFoot = 6.0f + kLocCtrlH;                      // 底栏（上方 16 分隔 = 行距 10 + 6）
    const float cardH = kLocCardPad * 2.0f + hAuto + hTopRows + hExpr + hPh + hFont + hPrev + hFoot;
    const core::Color cardBg =
        th.dark ? core::mixColor(th.surface, core::Color{1.0f, 1.0f, 1.0f, 1.0f}, 0.05f)
                : core::mixColor(th.surface, core::Color{0.0f, 0.0f, 0.0f, 1.0f}, 0.02f);
    ui.rect("loc.card").position(cardX, cardY).size(cardW, cardH).color(cardBg).radius(12.0f)
        .border(1.0f, components::theme::withOpacity(th.border, 0.9f)).build();

    // （旧 col2X 右列竖线已废：三档预设移到卡右缘、表达式区两列由 colW 自算，
    //   都不再依赖这条 0.45c 的线。）

    float cy = cardY + kLocCardPad;

    // ① 自动生成（仅 ≥2 个版本）—— 单版本时整行不出现，不留空行（§8.2.2）。
    //    左标题「自动生成」、右开关（用户 2026-11-03：原标题太长，说明并入悬停浮窗）。
    if (multi) {
        locLabel(ui, "loc.auto.lab", "自动生成", cx, cy);
        // 标题悬停出解释；点一下等于切换。注意 id 绝不能叫 "loc.auto.hit"：toggleSwitch 内部
        // 的命中区就叫 <id>+".hit"（switch.h），两个同 id 的 interactive 元素会共享
        // 同一个 interaction 状态，hover 每帧被互相覆盖 —— 实测每帧重复触发
        // on=1/on=0（一次会话 963 次），移开时 hide 被乱序淹没，浮窗残留不消失。
        const float labsW =
            core::TextPrimitive::measureTextWidth("自动生成", {}, 14.0f * textScale(), 400);
        components::mouseArea(ui, "loc.auto.title").position(cx - 4.0f, cy)
            .size(labsW + 8.0f, kLocCtrlH).cursor(core::CursorShape::Arrow)
            .onHover([cx, labsW, cy](bool on) {
                // 浮窗摆在标题右侧、同一行上：标题下面紧挨着「目标版本」那一行，
                // 往下放会正好盖住下拉框（第一版就是压在它身上）。
                if (on)
                    locTipShow("防止多个版本切换下部分翻译错位，每次启动PXC前生成对应版本的汉化文件",
                               kNavW + cx + kLocLabelW + 60.0f + 10.0f,
                               cy - g_pageScroll.get() - 2.0f, 320.0f);
                else if (g_locTip.on && g_locTip.text.rfind("防止多个版本切换", 0) == 0)
                    locTipHide();
            })
            .onTap([] { setAutoGen(!g_cfg.gen.auto_gen_on_launch); })
            .build();
        // 开关移到标题右侧、与下方输入框左对齐（cx+kLocLabelW），不再贴卡右缘。
        ui.stack("loc.auto.wrap").position(cx + kLocLabelW, cy).size(60.0f, kLocCtrlH)
            .content([&, th] {
                components::toggleSwitch(ui, "loc.auto").theme(th).size(60.0f, kLocCtrlH)
                    .trackSize(40.0f, 22.0f).fontSize(13.0f)
                    .checked(g_cfg.gen.auto_gen_on_launch)
                    .onChange([](bool v) { setAutoGen(v); }).build();
            }).build();
        cy += kLocCtrlH + kLocRowGap;
    }

    // ② 目标版本（§8.2.4）：只列已装且能取到骨架的版本，默认最高的那个。
    //    选项只写版本号 —— 节点数、覆盖率是维护者看的东西，放在这里只是噪音
    //    （用户 2026-11-03；覆盖率改为生成时写 log，见 onGenerate）。
    {
        const float ddW = 220.0f;
        std::vector<std::string> labels;
        int sel = -1;
        for (int i = 0; i < (int)usable.size(); ++i) {
            const auto& v = g_skelOpts[(size_t)usable[(size_t)i]];
            labels.push_back(v.version);
            if (pxc_reg::sameVersion(v.version, target)) sel = i;
        }
        const bool noVer = labels.empty();
        locLabel(ui, "loc.tv.lab", "目标版本", cx, cy);
        ddField(ui, "loc.tv", cx + kLocLabelW, cy, ddW, kLocCtrlH, labels, sel,
                [labels](int idx) {
                    if (idx < 0 || idx >= (int)labels.size()) return;
                    g_cfg.gen.target_version = labels[(size_t)idx];
                    saveConfig();
                    invalidateBase();   // 换骨架了，运行期那份缓存作废
                    requestUpdate();
                },
                noVer ? "（没有可用版本）" : "", false, autoMode || noVer);
        if (multi) locExclaim(ui, cx + kLocLabelW + ddW + 10.0f, cy, autoMode);
        // 右侧只剩两种情况要说话：自动模式为什么点不动它 / 一个版本都没有。
        const float hxx = cx + kLocLabelW + ddW + (multi ? 34.0f : 12.0f);
        std::string hint;
        core::Color hc = dim;
        if (autoMode) {
            hint = "实际按启动的版本生成";
        } else if (noVer) {
            hint = "先在版本库登记一个 PXC";
            hc = {0.85f, 0.65f, 0.40f, 1.0f};
        }
        if (!hint.empty()) locNote(ui, "loc.tv.hint", hint, hxx, cy, cx + cw - hxx, hc);
        cy += kLocCtrlH + kLocRowGap;
    }

    // ③ 语言名称
    {
        locLabel(ui, "loc.lang.lab", "语言名称", cx, cy);
        ui.stack("loc.lang.wrap").position(cx + kLocLabelW, cy).size(216.0f, kLocCtrlH).content([&] {
            components::input(ui, "loc.lang").theme(th).size(216.0f, kLocCtrlH).fontSize(13.0f)
                .placeholder("zh").bind(g_langDir).build();
        }).build();
        // 不再放「生成完会把 PXC 界面语言切到它」：生成结果 toast 第一行已交代
        // 「下次启动 PXC 时生效」，界面语言切没切看启动就知道（用户 2026-11-03）。
        cy += kLocCtrlH + kLocRowGap;
    }

    // ④ 文本设置：三选一的档位，不是三段独立动作 → 三连实心会被当成"按三下"，
    //    所以只有当前生效那档用主色描边 + 加粗，其余都是次要样式（§8.2.3）。
    //    三个档位贴**卡右缘**：右边缘与表达式区右列输入框对齐（cx+cw），看着是
    //    同一根右边界线；贴着标题排会像和「语言名称」并列的另一种设置（用户 2026-11-03）。
    {
        locLabel(ui, "loc.pre.lab", "文本设置", cx, cy);
        const char* preLabels[3] = {"纯中文", "中文在前", "英文在前"};
        const int curPreset = g_preset.get();
        const float preBtnW = 92.0f, preBtnGap = 8.0f;
        const float preRight = cx + cw - (preBtnW * 3.0f + preBtnGap * 2.0f);
        for (int p = 0; p < 3; ++p) {
            const float bx = preRight + (float)p * (preBtnW + preBtnGap);
            const bool on = (p == curPreset);
            ui.stack("loc.pre" + std::to_string(p) + ".wrap").position(bx, cy).size(92.0f, kLocCtrlH)
                .content([&, p, on] {
                    components::button(ui, "loc.pre" + std::to_string(p)).theme(th, false)
                        .size(92.0f, kLocCtrlH).text(preLabels[p]).fontSize(kLocBtnFont)
                        .border(1.0f, on ? th.primary
                                         : components::theme::withOpacity(th.border, 0.85f))
                        .textColor(on ? th.primary : th.text)
                        .onClick([p] { onApplyPreset(p); }).build();
                }).build();
        }
        cy += kLocCtrlH + kLocRowGap;
    }

    // ⑤⑥⑦ 六项表达式：两列网格，整组**缩进 40px** —— 它们改的就是「文本设置」那一档的
    // 具体内容，缩进才看得出是分支；两列等宽、输入框同宽，左右两列右边缘各对齐一根线
    //（用户 2026-11-03：「让设置能被理解为文本设置的分支项，并且使输入框看上去对齐」）。
    {
        struct ExRow { const char* id; const char* label; eui::Signal<std::string>* sig; };
        ExRow rows[6] = {
            {"nn", "节点名", &g_exNodeName}, {"nt", "节点提示", &g_exNodeTip},
            {"in", "输入端口", &g_exInName},  {"on", "输出端口", &g_exOutName},
            {"dd", "下拉项", &g_exDd},        {"tx", "界面文本", &g_exText}};
        const float exLabW = 72.0f;
        const float colW = (cw - kLocSubIndent - kLocColGap) * 0.5f;
        const float iw = std::max(110.0f, colW - exLabW);
        for (int i = 0; i < 6; ++i) {
            const bool left = (i % 2 == 0);
            const float colx = cx + kLocSubIndent + (left ? 0.0f : colW + kLocColGap);
            const float ry = cy + (float)(i / 2) * (kLocCtrlH + kLocRowGap);
            ui.text(std::string("loc.ex.") + rows[i].id + ".lab").position(colx, ry)
                .size(exLabW, kLocCtrlH).text(rows[i].label).fontSize(13.0f).lineHeight(18.0f)
                .color({0.80f, 0.84f, 0.90f, 1.0f})
                .verticalAlign(core::VerticalAlign::Center).build();
            ui.stack(std::string("loc.ex.") + rows[i].id + ".wrap").position(colx + exLabW, ry)
                .size(iw, kLocCtrlH)
                .content([&, i, iw] {
                    components::input(ui, std::string("loc.ex.") + rows[i].id).theme(th).fontSize(13.0f)
                        .size(iw, kLocCtrlH).placeholder("%zh").bind(*rows[i].sig).build();
                }).build();
        }
        cy += 3.0f * (kLocCtrlH + kLocRowGap);
    }

    // ⑧ 占位符说明（整行，跟六项同一条左边界）
    ui.text("loc.ex.hint").position(cx + kLocSubIndent, cy).size(cw - kLocSubIndent, 20.0f)
        .text("占位符 %en 英文、%zh 中文。")
        .fontSize(12.0f).lineHeight(16.0f).color(dim).wrap(true).build();
    cy += 20.0f + kLocRowGap;

    // ⑨ 字体：两列，选项用字体本身渲染（§8.3 —— 让人在选之前就能看出中文长什么样）
    // 列不全：Windows 的 Fonts 目录动辄上百个，其中大半是符号字体（`AIGDT`、`MTSY` 之类），
    // 用它们渲染自己的名字只会得到一串豆腐块。按 §8.3 约束 3 只列前 20 项 + 「更多…」，
    // 内置字体排在前（`scanFonts()` 就是先扫内置再扫系统）。
    {
        const float c2x = cx + cw * 0.45f;
        const float fLabW = 72.0f;
        const bool noFont = g_fonts.empty();
        const int kFontPage = 20;
        struct FRow { const char* id; const char* label; eui::Signal<int>* sig; };
        FRow fr[2] = {{"loc.f1", "界面字体", &g_fontUi}, {"loc.f2", "代码字体", &g_fontCode}};
        for (int i = 0; i < 2; ++i) {
            const bool left = (i == 0);
            const float colx = left ? cx : c2x;
            const float avail = (left ? c2x : cx + cw) - colx;
            const float dw = std::max(110.0f, std::min(200.0f, avail - fLabW - 8.0f));
            const std::string fid = fr[i].id;
            const int cur = fr[i].sig->get();
            const bool expanded = g_ddMore.count(fid) > 0;
            const int shown = noFont ? 1
                                     : (expanded ? (int)g_fonts.size()
                                                 : std::min((int)g_fonts.size(), kFontPage));

            std::vector<std::string> items, tags, paths;
            for (int k = 0; k < shown; ++k) {
                const std::string& nm = g_fonts[(size_t)k].first;
                // `scanFonts()` 的显示名是「[内置] 文件名」/「[系统] 文件名」，前缀都是 9 字节。
                // 前缀挪到右侧当灰标签，主文本只留文件名 —— 它要按 HTML 那个字体渲染，前面挂个
                // 中文标签反而看不清字形。
                const bool hasTag = (nm.rfind("[内置] ", 0) == 0 || nm.rfind("[系统] ", 0) == 0);
                items.push_back(hasTag ? nm.substr(9) : nm);
                tags.push_back(hasTag ? (nm.rfind("[内置] ", 0) == 0 ? "内置" : "系统") : "");
                paths.push_back(g_fonts[(size_t)k].second.u8string());
            }
            if (noFont) {
                items.push_back("（无可用字体）");
                tags.emplace_back();
                paths.emplace_back();
            } else if (!expanded && (int)g_fonts.size() > kFontPage) {
                items.push_back("更多…（还有 " + std::to_string((int)g_fonts.size() - kFontPage) + " 个）");
                tags.emplace_back();
                paths.emplace_back();
            }

            // 选中的那个不在当前这 20 项里时，用 placeholder 把它的名字显示出来 ——
            // 否则收起态会变成空白，看着像"没选字体"。
            std::string curName;
            if (!noFont && cur >= 0 && cur < (int)g_fonts.size()) {
                const std::string& nm = g_fonts[(size_t)cur].first;
                curName = (nm.rfind("[内置] ", 0) == 0 || nm.rfind("[系统] ", 0) == 0) ? nm.substr(9) : nm;
            }
            const int sel = (!noFont && cur >= 0 && cur < shown) ? cur : -1;
            const std::string ph = noFont ? std::string("（无可用字体）")
                                          : (sel < 0 ? wbFitHead(curName, dw - 40.0f, 13.0f) : std::string());

            ui.text(std::string(fr[i].id) + ".lab").position(colx, cy).size(fLabW, kLocCtrlH)
                .text(fr[i].label).fontSize(13.0f).lineHeight(18.0f)
                .color({0.80f, 0.84f, 0.90f, 1.0f})
                .verticalAlign(core::VerticalAlign::Center).build();
            ddField(ui, fid, colx + fLabW, cy, dw, kLocCtrlH, items, sel,
                    [i, noFont, shown, fid](int idx) {
                        if (noFont) return;
                        if (idx >= shown) {           // 「更多…」：就地展开，不动选中值
                            g_ddMore.insert(fid);
                            g_ddKeep = true;
                            requestUpdate();
                            return;
                        }
                        if (idx < 0 || idx >= (int)g_fonts.size()) return;
                        if (i == 0) {
                            g_cfg.gen.ui_font = g_fonts[(size_t)idx].second.u8string();
                            g_fontUi.set(idx);
                        } else {
                            g_cfg.gen.code_font = g_fonts[(size_t)idx].second.u8string();
                            g_fontCode.set(idx);
                        }
                        saveConfig();
                        requestUpdate();   // 预览条跟着下一帧重画（不重建骨架）
                    },
                    ph, true, false, tags, paths);
        }
        cy += kLocCtrlH + kLocRowGap;
    }

    // ⑩ 预览条（§8.4）：做成"凹槽"而不是按钮 —— 按钮会让人去点它。
    // 单一示例节点名，按「节点名」表达式实时渲染（不读骨架）：仍用界面字体渲染，切字体字形跟着变；
    // 内容是示例 zh=轮廓 / en=Outline，让用户直接看到这一档会生成出什么格式（例：%zh_%en → 轮廓_Outline）。
    {
        const float ph = 40.0f;
        const core::Color groove = core::mixColor(cardBg, core::Color{0.0f, 0.0f, 0.0f, 1.0f}, 0.22f);
        std::string ff;
        const int ui2 = g_fontUi.get();
        if (ui2 >= 0 && ui2 < (int)g_fonts.size()) ff = g_fonts[(size_t)ui2].second.u8string();
        ui.rect("loc.pv").position(cx, cy).size(cw, ph).color(groove).radius(8.0f)
            .border(1.0f, components::theme::withOpacity(th.border, 0.55f)).build();
        // 单一示例节点名：按「节点名」表达式渲染（示例 zh=轮廓 / en=Outline），
        // 让用户直接看到这一档会生成出什么格式的节点名；随输入框实时变化（compose 每帧跑）。
        const std::string pvName = pxc_expr::render(g_exNodeName.get(), "Outline", "轮廓");
        ui.text("loc.pv.t").position(cx, cy).size(cw, ph)
            .text(pvName).fontFamily(ff).fontSize(14.0f)
            .lineHeight(ph).color(setTextMain())
            .horizontalAlign(core::HorizontalAlign::Center)
            .verticalAlign(core::VerticalAlign::Center).build();
        cy += ph + kLocRowGap;
    }

    // ⑪ 底栏：左「自定义译文…」右「生成并应用」（本页唯一的主操作）
    cy += 6.0f;
    locButton(ui, "loc.ov", cx, cy, 112.0f, "自定义译文…", false,
              [] { guarded("打开自定义译文层", onOvOpen); });
    {
        const float gw = 166.0f;
        const bool noVer = usable.empty();
        locGenButton(ui, cx + cw - gw, cy, gw, autoMode, noVer, [] {
            // 自动模式下这颗按钮只是状态显示（文本已经写着"已开启自动生成"），
            // 动作由卡内第一行的开关接管 —— 点它不做事，把原因放在悬停里讲（§8.2.5.1）。
            if (g_cfg.gen.auto_gen_on_launch) return;
            guarded("生成并应用", onGenerate);
        });
        if (autoMode) {
            // 盖一层透明命中区：悬停出解释。它同时挡住了按钮的点击 —— 本来也无事可做。
            components::mouseArea(ui, "loc.gen.tip").position(cx + cw - gw, cy).size(gw, kLocCtrlH)
                .cursor(core::CursorShape::Arrow)
                .onHover([cx, cw, gw, cy](bool on) {
                    if (on)
                        locTipShow("每次启动 PXC 前都会按它自己的版本重写这份包。\n"
                                   "想现在手动生成一次，先关掉卡内第一行的开关。",
                                   kNavW + cx + cw - gw, cy - g_pageScroll.get() - 8.0f, 300.0f);
                    else if (g_locTip.on && g_locTip.text.rfind("每次启动 PXC 前都会", 0) == 0)
                        locTipHide();
                })
                .build();
        }
    }
    cy += kLocCtrlH;

    g_pageH = cardY + cardH + 28.0f;
}

// 粗估一段 UTF-8 文本在给定宽度下要占几行（只用于给"回执行"预留高度）。
// 目的不是精确排版，而是避免"按两行留位置、结果只说了一行"留下一条 30px 的死空白。
// 汉字/全角按 1em、ASCII 按 0.55em 估 —— 比真实字宽略偏大，宁可多留一行也不压字。
static int estimateLines(const std::string& s, float width, float fontSize) {
    if (width <= 1.0f) return 1;
    int lines = 1;
    float w = 0.0f;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        float adv = fontSize * 0.55f;
        if (c < 0x80) {
            len = 1;
        } else if ((c >> 5) == 0x06) {
            len = 2;
            adv = fontSize * 0.62f;
        } else if ((c >> 4) == 0x0E) {
            len = 3;
            adv = fontSize;
        } else {
            len = 4;
            adv = fontSize;
        }
        if (w + adv > width) {
            ++lines;
            w = 0.0f;
        }
        w += adv;
        i += len;
    }
    return lines;
}
// ---------- 汉化页的三个根级浮层 ----------
// 为什么都画在根级：页内容装在 scrollView 的裁剪容器里，挂在那里会被裁掉，也会被页面滚动带走。
// 顺序（自下而上）：`!` 提示 → 弹窗 → toast。z 值：提示 1100 / 弹窗 1010 / toast 组件自带 1100
// （toast 最后 build，同 z 时后画的在上）。

// EUI 的 `screen.height` 是**含标题栏与边框的外框**高度（实测比客户区大 30 上下），
// 而组件按这个值算浮层位置时会被推到可视区底部之外（照片上就是"贴着窗口底、圆角被切掉"）。
// 拿实时客户区换算一次，别学 composeScaleConfirm 里那个写死的 30 —— 标题栏高度随 DPI/主题变。
static float clientInsetH() {
    static float cached = -1.0f;
    if (cached >= 0.0f) return cached;
    HWND h = (HWND)pxc_nui::mainWindow();
    if (!h) return 0.0f;
    RECT outer{}, client{};
    if (!GetWindowRect(h, &outer) || !GetClientRect(h, &client)) return 0.0f;
    cached = (float)((outer.bottom - outer.top) - (client.bottom - client.top));
    return cached;
}

// toast（§8.6.3）：窗口右下角，坐标由组件按当前客户区自算。
// 高度必须按 textScale 现算：组件内部把 message 区域写死为 height - 50，
// 文本缩放 150% 时五行需要 135px，写死 88 高会直接裁字。
static void composeLocaleToast(core::dsl::Ui& ui, const eui::Screen& screen) {
    if (!g_toast.on) return;
    // 行数要按**折行后**算，不能只数 '\n'：组件正文是 12 号（已压小）、宽约 290px，
    // 一句 40 字的中文就会折成 3 行，只按 '\n' 数会给一个矮一截的盒子，最后一行被裁掉。
    // 用比实际可排宽度略小的 msgW 估行数，宁可多留一行也不压字（用户 2026-11-03：缩小文本与留白）。
    const float msgW = 290.0f;
    const float msgFs = 12.0f * textScale();     // 压到 12 号（原 14），更紧凑
    const float lineH = std::round(16.0f * textScale());
    int lines = 0;
    {
        std::string seg;
        for (size_t i = 0; i <= g_toast.msg.size(); ++i) {
            if (i == g_toast.msg.size() || g_toast.msg[i] == '\n') {
                lines += std::max(1, estimateLines(seg, msgW, msgFs));
                seg.clear();
                continue;
            }
            seg += g_toast.msg[i];
        }
    }
    const float h = std::max(56.0f, 50.0f + (float)lines * lineH);
    components::toast(ui, "loc.toast").theme(uiTheme())
        .screen(screen.width, std::max(80.0f, screen.height - clientInsetH()))
        .size(380.0f, h)
        .title(g_toast.title)
        .message(g_toast.msg)
        .fontSize(12.0f).titleFontSize(13.0f)   // 压小正文与标题（原 label 14 / control 18）
        .icon(g_toast.error ? 0xF057 : 0xF058)   // 圆叉 / 圆勾
        .duration(g_toast.error ? 0.0f : 6.0f)   // 失败不自动消失：必须看见"没成"
        .visible(true)
        .onDismiss([] {
            g_toast.on = false;
            requestUpdate();
        })
        .onAutoDismiss([] {
            g_toast.on = false;
            requestUpdate();
        })
        .build();
}

// 页面级悬停浮窗（§8.2.4）：`!` 与「自动生成」标题共用同一套渲染。
// 内容由 hover 方给（`locTipShow` 一次一句/一段），这里只管量高、定位与画。
static void composeLocaleTip(core::dsl::Ui& ui, const eui::Screen& screen) {
    if (!g_locTip.on || g_locTip.text.empty()) return;
    const auto th = uiTheme();
    const std::string text = g_locTip.text;
    const float w = g_locTip.w;
    // 字号与行高都要先乘 textScale 再量：fontSize 实际渲染成 fontSize×textScale，
    // 按裸字号量出来的行数会少一行，浮窗就矮一截、最后一行掉到框外（照片上直接可见）。
    // 原始值 12 号 + 20 内边距（用户 2026-11-03 曾误缩成 11 号 + 14，已回退：那次缩错了对象）。
    const float fs = 12.0f * textScale();
    const float lineH = 16.0f * textScale();
    int lines = 0;
    {
        std::string seg;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                lines += std::max(1, estimateLines(seg, w - 20.0f, fs));
                seg.clear();
                continue;
            }
            seg += text[i];
        }
    }
    const float h = (float)lines * lineH + 20.0f;
    float ty = g_locTip.y + 6.0f;
    if (ty + h > screen.height - 8.0f) ty = std::max(8.0f, g_locTip.y - h - 34.0f);
    float tx = std::min(g_locTip.x, screen.width - w - 12.0f);
    tx = std::max(12.0f, tx);
    ui.stack("loc.xtip").position(tx, ty).size(w, h).zIndex(1100)
        .content([&, w, h, lineH, text, th] {
            // 底色用接近不透明的深色（与版本库的路径浮窗同一档）：页面底色本身就不亮，
            // 再"提亮一档"看着像没画背景，文字会像直接飘在表单上。
            ui.rect("loc.xtip.bg").size(w, h)
                .color(th.dark ? core::Color{0.10f, 0.11f, 0.14f, 0.97f}
                              : core::Color{0.99f, 0.99f, 1.0f, 0.99f})
                .radius(8.0f)
                .border(1.0f, th.dark ? core::Color{0.28f, 0.31f, 0.37f, 1.0f}
                                      : components::theme::withOpacity(th.border, 0.9f))
                .shadow(components::theme::popupShadow(th)).build();
            ui.text("loc.xtip.t").position(10.0f, 10.0f).size(w - 20.0f, h - 20.0f)
                .text(text).fontSize(12.0f).lineHeight(lineH).color(th.text)
                .wrap(true).build();
        }).build();
}

// 界面语言写入被拒的两个弹窗（§8.5.2 / §8.5.3）。
// 为什么这里用模态、成功回执用 toast：toast 是"你做完了，回执看一眼"，错过也无所谓；
// 这里是"你要做的事没做成"，而且下拉已经被回滚 —— 只有模态能解释"我刚点的那个为什么弹回去了"。
// kind 0 有两种来路：直接改语言（正文只有失败原因）与「生成并应用」
// （正文先说"生成成功"，第二段放生成信息 —— 成功 toast 在这种情况下不弹，见 onGenerate）。
// 「打开 PXC 所在文件夹」按钮已移除（用户 2026-11-03）：两种来路下 PXC 都开着，
// 打开它的文件夹解决不了任何问题。其余说明也一并从简："写回 keys.json 会被覆盖"
// 是实现细节，用户不需要知道。
static void composeLocaleDialogs(core::dsl::Ui& ui, const eui::Screen& screen) {
    if (!g_locDlg.open) return;
    const auto th = uiTheme();
    const bool running = (g_locDlg.kind == 0);
    const bool fromGen = running && !g_locDlg.genInfo.empty();
    const float W = 440.0f;
    const std::string genInfo = g_locDlg.genInfo;
    // 段落高度按内容现算：生成信息是 2~3 行会折行的长文本，写死高度会截尾。
    auto wrapLines = [](const std::string& s, float w, float fs) {
        int n = 0;
        std::string seg;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '\n') {
                n += std::max(1, estimateLines(seg, w, fs));
                seg.clear();
                continue;
            }
            seg += s[i];
        }
        return n;
    };
    const float tw = W - 48.0f;
    const std::string mainText = fromGen
        ? "语言文件生成成功，但 PXC 未关闭，无法修改界面语言，请关闭 PXC 后再修改。"
        : (running ? "当前 PXC 未关闭，无法修改界面语言。请关闭 PXC 后再修改。"
                   : "没法写入界面语言：在用户目录里没找到 Preferences/<构建号>/keys.json。");
    float H = 264.0f;
    float secY = 0.0f, secH = 0.0f, mainH = 0.0f;
    if (running) {
        mainH = (float)wrapLines(mainText, tw, 13.0f * textScale()) * 20.0f;
        if (fromGen) {
            secY = 54.0f + mainH + 10.0f;
            secH = (float)wrapLines(genInfo, tw, 12.0f * textScale()) * 17.0f;
            H = secY + secH + 64.0f;   // 底部按钮行 48 + 上方 16 间距
        } else {
            H = 54.0f + mainH + 16.0f + 48.0f;
        }
    }
    components::dialog(ui, "loc.dlg").theme(th).screen(screen.width, screen.height)
        .size(W, H).zIndex(1010).open(true)
        .onOpenChange([](bool v) {
            if (!v) {
                g_locDlg.open = false;
                requestUpdate();
            }
        })
        .content([&, running, fromGen, W, H, th, mainText, genInfo, tw, mainH, secY, secH] {
            auto dbtn = [&](const std::string& id, float bx, float by, float bw,
                            const std::string& label, bool primary, std::function<void()> fn) {
                ui.stack(id + ".w").position(bx, by).size(bw, 32.0f)
                    .content([&, id, bw, label, primary, fn, th] {
                        components::button(ui, id).theme(th, primary).size(bw, 32.0f).text(label)
                            .fontSize(13.0f).onClick(fn).build();
                    }).build();
            };
            ui.text("loc.dlg.t").position(24.0f, 20.0f).size(tw, 24.0f)
                .text(running ? "PXC 正在运行" : "找不到 PXC 的语言设置文件")
                .fontSize(16.0f).lineHeight(22.0f).fontWeight(700)
                .color({0.96f, 0.97f, 1.0f, 1.0f}).build();
            ui.text("loc.dlg.m").position(24.0f, 54.0f).size(tw, running ? mainH : 44.0f)
                .text(mainText)
                .fontSize(13.0f).lineHeight(20.0f).color({0.88f, 0.91f, 0.95f, 1.0f})
                .wrap(true).build();
            if (running) {
                if (fromGen)
                    ui.text("loc.dlg.s").position(24.0f, secY).size(tw, secH)
                        .text(genInfo)
                        .fontSize(12.0f).lineHeight(17.0f).color(setTextDim()).wrap(true).build();
                dbtn("loc.dlg.ok", W - 116.0f, H - 48.0f, 92.0f, "知道了", true, [] {
                    g_locDlg.open = false;
                    requestUpdate();
                });
            } else {
                ui.text("loc.dlg.s").position(24.0f, 104.0f).size(tw, 80.0f)
                    .text("可能原因：① 用户目录不是 PXC 实际在用的那个；② PXC 从没在这台机器上正常启动过"
                          "（Preferences 是它启动时创建的）；③ 当前登录的构建号与探测到的目录不同。")
                    .fontSize(12.0f).lineHeight(17.0f).color(setTextDim()).wrap(true).build();
                dbtn("loc.dlg.re", 24.0f, H - 48.0f, 96.0f, "重新探测", false, [] {
                    onAutoUserDir();          // 重跑定位链
                    refreshPxcPrefs(true);    // 再探一次 local
                    if (!g_pxcPrefs.prefsFile.empty()) {
                        g_locDlg.open = false;
                        pxc_log::write("INFO", "Prefs",
                                       "重新探测到 " + g_pxcPrefs.prefsFile.u8string());
                    }
                    requestUpdate();
                });
                dbtn("loc.dlg.ok", W - 116.0f, H - 48.0f, 92.0f, "知道了", true, [] {
                    g_locDlg.open = false;
                    requestUpdate();
                });
            }
        }).build();
}


// 悬浮提示（自绘）。为什么不用 components::tooltip：它靠 hoverOpacityFrom 挂到"源元素"的
// hover 状态上，而置灰按钮**不接收指针事件**、压根没有 hover 状态可挂。这里用一层透明
// mouseArea 探 hover，提示框画在浮窗内容的最末尾（最后画 = 最上层，且面板不裁剪）。
static std::string g_tipText;
static float g_tipX = 0.0f, g_tipY = 0.0f, g_tipW = 300.0f;
static int g_tipLines = 2;
static void setTip(bool on, const std::string& text, float x, float y, float w, int lines) {
    if (on) {
        g_tipText = text;
        g_tipX = x;
        g_tipY = y;
        g_tipW = w;
        g_tipLines = lines;
    } else if (g_tipText == text) {
        g_tipText.clear();
    }
    requestUpdate();
}

// 浮窗：左「分页 + 筛选 + 搜索 + 条目列表」，右「条目字段编辑」。
// 自定义译文与构建器是**同一套界面**（g_ovMode 区分），差别只在数据源、顶栏按钮与底栏出口。
// 性能：两侧都 virtualList；分页懒构建；搜索/筛选走预计算归一化副本；保存只更新被编辑那一条。
static void composeOverrideDialog(core::dsl::Ui& ui, const eui::Screen& screen) {
    // 从未打开过就什么都不做：否则每帧都会触发一次分页构建（942 节点扫描），
    // 应用一启动就卡。打开过一次后 built=true，不会重复构建。
    if (!g_ovOpen.get() && !g_ovData[0].built) return;
    // 关掉浮窗时把悬浮提示一起清掉：指针若停在置灰按钮上就关窗，onHover(false) 不会再来一趟，
    // 提示框会带着上一轮的文字飘到下次打开的界面里。
    if (!g_ovOpen.get()) g_tipText.clear();
    // 草稿到期补写：节流窗口内攒的改动，靠这一帧落盘（不做任何读盘，只在到期时写）。
    if (g_ovMode == 1 && g_draftPending && GetTickCount64() - g_draftLast >= 2000) writeDraftNow();
    OvPageData& pd = ovCur();
    if (!pd.built) { ovBuildPage(ovCurPage()); ovRefreshView(); }
    else ovRefreshIfNeeded();
    ovCountsIfNeeded();   // 页签上的数量（缓存，只在查询/筛选/数据版本变化时重算）

    const bool bld = (g_ovMode == 1);
    // 高度必须自己先按可用空间钳一遍：dialog 内部会做 `min(H, screenH - overlay)`，
    // 如果布局仍按未钳的 H 排，底栏就会掉到面板外面（实测 700 被钳成 672，
    // 底栏按钮溢出去 18px，直接压在底下的页面上）。这里用同一套令牌算出真实 H。
    const float availH = screen.height - uiTheme().metrics.spacing.overlay;
    const float W = 920.0f, H = std::min(bld ? 700.0f : 620.0f, availH), pad = 20.0f;
    // 纵向只有三段：抬头（标题/导入带）→ 分页+筛选行 → 内容行。
    // 三段的 y 全部由 tabsY 派生，一律从"上一段的底边 + 固定间距"算，不再各自写死绝对值 ——
    // 之前分页用 top-36、搜索/列表用 top+38 两套独立偏移，左列由此多出 44px 空白；
    // 回执行消失时也不会有空档（它的高度只在存在时计入）。
    const bool bldMsg = bld && !g_ovStatus.empty();
    // 回执行占几行就给它留几行的高度，分页行的位置从这里派生 —— 固定留两行会在只说
    // 一句话时留下一条 30px 的空档（用户已经为这类空白提过一次）。
    const int msgLines = bldMsg ? std::min(3, estimateLines(g_ovStatus, W - pad * 2.0f, 11.0f)) : 0;
    // 「骨架 + 英文包 + 汉化包」挤在同一行；路径长时会回绕，按实际行数留高（上限两行）。
    // metaY = 主流程按钮行底边（50+32）再留 8px。
    // 顶部整体下移 8（标题 y 6→14、顶栏按钮 3→12）：原先把标题/顶栏按钮压在离弹窗顶 3px 处太贴边。
    // 标题行和主流程按钮行**两排都是按钮、且右端都右对齐**，只挪标题会让二者只隔 4px 读成一坨，
    // 所以主流程行跟着一起下移 8（42→50），这条 ≥12px 的间隔才保得住。
    const std::string metaText = bld ? ovMetaLine() : std::string();
    const int metaLines = bld ? std::min(2, estimateLines(metaText, W - pad * 2.0f, 10.5f)) : 0;
    const float metaY = 90.0f;
    const float msgY = metaY + (float)metaLines * 14.0f + 4.0f;
    const float tabsY = bld ? (msgY + (float)msgLines * 15.0f + 6.0f) : 70.0f;
    const float contentY = bld ? (tabsY + 38.0f) : tabsY;  // 右栏首行（用户模式没有分页行）
    const float listY = tabsY + 80.0f;                     // 左列表首行（上方是搜索框）
    // 底栏：构建器多一行「可译/已译/待翻译」统计，所以比用户模式高 30。
    const float bottomBar = bld ? 86.0f : 56.0f;
    // 覆盖率（+ 导出/导入 json）那一行的 y。编辑区底衬要用它收边，所以提前到这里算。
    const float covY = H - bottomBar + 4.0f;
    // 内容区底边（列表/字段的高度上限）。构建器再往上收 10：列表最后一行常被裁半截，
    // 紧贴下面的统计文字会看成"文字压住了列表"，留条缝就干净了。
    const float bodyEnd = H - bottomBar - (bld ? 10.0f : 0.0f);
    const float leftW = 300.0f;
    const float rightX = pad + leftW + 16.0f;
    const float rightW = std::max(200.0f, W - rightX - pad);
    const int selIdx = pd.sel;
    const bool hasSel = (selIdx >= 0 && selIdx < (int)pd.items.size());
    const std::string selId = hasSel ? pd.items[(size_t)selIdx].id : std::string();
    const std::string selEn = hasSel ? pd.items[(size_t)selIdx].en : std::string();
    const bool notesPage = (ovCurPage() == (int)OvPage::Notes);
    const std::string selHint = selId.empty()
                                    ? "从左侧选一个条目"
                                    : (notesPage ? ("notes/" + selId) : (selEn + "    " + selId));

    // 按钮统一走 stack 包裹（与其它页面一致），避免 position 与内容布局互相干扰。
    // bh 可省（默认 32）：顶栏那两个"表管理"按钮用 26 —— 它们和下面那排主流程按钮
    // 竖着紧邻，同高会显得两排黏在一起。
    auto btn = [&](const char* rid, float bx, float by, float bw, const std::string& label,
                   std::function<void()> fn, float bh = 32.0f) {
        ui.stack(std::string(rid) + ".wrap").position(bx, by).size(bw, bh)
            .content([&, rid, bw, bh, label, fn] {
                components::button(ui, rid).theme(uiTheme()).size(bw, bh).text(label)
                    .fontSize((bh < 30.0f) ? 11.0f : 12.0f)
                    .onClick(fn).build();
            }).build();
    };

    // 次要样式（无主色填充）：并排的两个按钮同色同高时，读起来像"两个一模一样的操作"，
    // 「或」这个字就白放了。主入口留主色，旁路入口降一档（docs/01 §4.1.1）。
    auto btnSec = [&](const char* rid, float bx, float by, float bw, const std::string& label,
                      std::function<void()> fn, float bh = 32.0f) {
        ui.stack(std::string(rid) + ".wrap").position(bx, by).size(bw, bh)
            .content([&, rid, bw, bh, label, fn] {
                components::button(ui, rid).theme(uiTheme(), false).size(bw, bh).text(label)
                    .fontSize((bh < 30.0f) ? 11.0f : 12.0f)
                    .onClick(fn).build();
            }).build();
    };

    // 统一置灰：外形仍然是按钮（有底、有圆角、同样的尺寸与位置），只是低饱和、点不动。
    // 以前"没有未保存改动"直接把按钮换成一串灰字，读起来像提示语而不是按钮 —— 用户看不出
    // 那儿原本是个按钮。tip 非空时叠一层透明 mouseArea 探 hover（置灰的 rect 收不到指针事件）。
    //
    // **不加描边**：按钮组件默认的 1px 边框配上一个和面板底几乎同色的填充，看起来就是个
    // 空输入框（实测被指认"像输入框"）。置灰的语义是"有这块按钮，但现在按不下去"，
    // 靠"填充比面板亮一档 + 文字降到 45%"就够了。
    // 四个页签带上各自的数量（当前筛选 + 搜索下的条目数）：一眼看出"活在哪一页、还剩多少"。
    // segmented 是等分宽度的，300px / 4 = 75px 一段，"节点 942" 这种 5~6 字刚好放得下；
    // 真放不下时宁可缩字号，也不要把数字砍掉 —— 没有数字这行就只是导航，不是工作量。
    std::vector<std::string> tabLabels;
    for (int i = 0; i < kOvPages; ++i)
        tabLabels.push_back(std::string(kOvPageName[i]) + " " +
                            std::to_string(g_ovCounts[i] < 0 ? 0 : g_ovCounts[i]));

    auto btnOff = [&](const char* rid, float bx, float by, float bw, float bh,
                      const std::string& label, const std::string& tip) {
        ui.stack(std::string(rid) + ".wrap").position(bx, by).size(bw, bh)
            .content([&, rid, bx, by, bw, bh, label, tip] {
                components::button(ui, rid).theme(uiTheme()).size(bw, bh)
                    .text(label).fontSize((bh < 30.0f) ? 11.0f : 12.0f)
                    .colors(core::Color{0.185f, 0.205f, 0.245f, 1.0f},
                            core::Color{0.185f, 0.205f, 0.245f, 1.0f},
                            core::Color{0.185f, 0.205f, 0.245f, 1.0f})
                    .textColor({0.43f, 0.47f, 0.55f, 1.0f})
                    .border(0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
                    .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
                    .disabled(true).build();
                if (!tip.empty())
                    components::mouseArea(ui, std::string(rid) + ".hov")
                        .position(0.0f, 0.0f).size(bw, bh)
                        .cursor(core::CursorShape::Arrow)   // 置灰按钮不该给"可点"的手型
                        .onHover([bx, by, bw, bh, tip, H](bool on) {
                            // 提示默认画在按钮下方；底栏那排按钮的下方就是面板边缘，会溢出去。
                            // 预留 2 行的高度（2×15 + 上下留白 14 = 44），放不下就翻到按钮上方。
                            const float th = 2.0f * 15.0f + 14.0f;
                            float ty = by + bh + 6.0f;
                            if (ty + th > H - 6.0f) ty = std::max(6.0f, by - th - 6.0f);
                            setTip(on, tip, bx + bw - 300.0f, ty, 300.0f, 2);
                        }).build();
            }).build();
    };

    components::dialog(ui, "ov.dlg")
        .theme(uiTheme())
        .screen(screen.width, screen.height)
        .size(W, H)
        .zIndex(300)
        .open(g_ovOpen.get())
        .onOpenChange([](bool v) {
            if (v) g_ovOpen.set(true);
            else onOvClose();  // 背板点击也走这里：保存后关闭
        })
        .content([&] {
            // 编辑区底衬：从「导入按钮行」一直到「导出 json 行」，比面板底亮一档。
            // 顶部是抬头、中间这一大块是"改东西"、最下一行才是"产出"—— 三分靠底色分开。
            //
            // **必须放在 content 的第一句**：dialog 的内容层按声明顺序绘制，后声明的盖住先声明的。
            // 这块底衬从 y=36 铺到几乎整个面板高，一旦排在分页/筛选之后，就会把标题行以下的
            // 所有控件（导入按钮、元信息、页签、筛选 chip）整片盖掉 —— 实测就是这么坏的。
            // 另外用 stack 包裹而不是裸 rect：裸 rect 直接 position 会参与内容层的流式布局。
            if (bld) {
                const float bgTop = 44.0f;   // 主流程行（50）上方留 6px，编辑区底衬从「导入按钮行」起
                const float bgBottom = covY + 24.0f;   // 导出 json 那一行收在里面（底部留 10px 内边距，不贴底衬边）；输出行留在外面
                const float bgW = W - (pad - 10.0f) * 2.0f;
                const float bgH = std::max(40.0f, bgBottom - bgTop);
                ui.stack("ov.editbg.wrap").position(pad - 10.0f, bgTop).size(bgW, bgH).content([&] {
                    // 比面板底（约 0.149/0.149/0.180）**亮一档**：取 0.175 那一档。
                    // 上一版写成 0.132 反而比面板更暗，实测采样 (34,36,43) < 面板 (38,38,46)，
                    // 看起来像给内容区挖了个坑，和"编辑区更醒目"的意图正好相反。
                    ui.rect("ov.editbg").size(bgW, bgH)
                        .color({0.175f, 0.185f, 0.215f, 1.0f}).radius(8.0f)
                        .border(1.0f, core::Color{0.20f, 0.22f, 0.27f, 1.0f}).build();
                }).build();
            }

            if (bld) {
                // ---- ① 标题行：谁在编辑 / 当前表版本（只读）/ 表管理两个按钮 ----
                // 版本号在这里是**只读**的：它表示"磁盘上那张表现在是哪一版"。
                // 可编辑的「输出版本」在底栏 —— 那是"这次要发布成什么"，改它不该让当前表看起来变了。
                ui.text("ov.title").position(pad, 14.0f).size(290.0f, 22.0f)
                    .text("对照表构建器 · 维护者").fontSize(17.0f).lineHeight(22.0f)
                    .fontWeight(700).color({0.95f, 0.97f, 1.0f, 1.0f}).build();
                // 两个「表管理」按钮 126/134×26：它们与下面那排主流程按钮竖着只隔十几像素，
                // 同高同宽会读成"一大片六七个按钮"，主流程与旁路就分不出来了。
                btn("ov.bimp", W - pad - 134.0f, 12.0f, 134.0f, "修改当前对照表",
                    [] { guarded("修改当前对照表", onBldImportDb); }, 26.0f);
                btn("ov.bexp", W - pad - 266.0f, 12.0f, 126.0f, "导出当前对照表",
                    [] { guarded("导出当前对照表", onBldExportDb); }, 26.0f);
                // 右端再留 10px：文字不裁剪，贴到「导出当前对照表」按钮左边缘会看成压住了按钮。
                ui.text("ov.curver").position(316.0f, 14.0f).size(W - pad - 276.0f - 316.0f, 20.0f)
                    // 文案刻意压短：EUI 的 text 不裁剪，写「当前对照表版本：」这种长前缀时
                    // 右对齐之后仍会往右溢出，压到「导出当前对照表」按钮上（实测）。
                    .text("当前表版本：" +
                          (g_db.version.empty() ? std::string("(未标)") : g_db.version))
                    .fontSize(11.5f).lineHeight(20.0f).color({0.72f, 0.78f, 0.90f, 1.0f})
                    .horizontalAlign(core::HorizontalAlign::Right)
                    .verticalAlign(core::VerticalAlign::Center).build();

                // ---- ② 主流程按钮行：按钮顺序 = 维护流程本身 ----
                // 换骨架 → 导出待翻译 → 外部翻译完导回 → 草稿。
                // 「导入已有汉化包」是**兼容整包翻译时代**的旁路（以前没工具筛选，只能整包翻），
                // 不该和主流程并排成一个"主要操作"，所以单独摆最右、无英文骨架时置灰。
                // y=50 而不是紧贴标题行（14–37）：两排按钮只留 13px 间隔就够，贴太近会看成同一组。
                // 「导出待翻译 / 导入已翻译」不在这里：它们是"把活送出去、收回来"，
                // 属于编辑区的最下一行（与覆盖率同行），见底栏 —— 界面自上而下就是维护流程本身。
                // 换骨架有**两个入口**（docs/01 §4.1.1）：维护者手上可能是"一份英文语言包"，
                // 也可能是"刚装上的新版 PXC"。后者只要给 exe —— 让他自己去翻 data/locale/en
                // 是多余的一步（同级还并排着 sample locale 和旧版本目录，容易翻错）。
                // 中间那个「或」就是读法：这两个选一个就行，都通向同一个 applyBldBaseline()。
                constexpr float kBldAW = 150.0f;   // 9 个汉字 @12px，128 会压字
                constexpr float kBldBW = 140.0f;
                constexpr float kBldOrW = 14.0f;
                constexpr float kBldGap = 8.0f;
                const float kBldBX = pad + kBldAW + kBldGap + kBldOrW + kBldGap;
                const float kBldAfter = kBldBX + kBldBW + kBldGap;
                btn("ov.b1", pad, 50.0f, kBldAW, "导入新版英文包骨架",
                    [] { guarded("导入新版英文包骨架", onBldPickBaseline); });
                ui.text("ov.or").position(pad + kBldAW + kBldGap, 50.0f).size(kBldOrW, 32.0f)
                    .text("或").fontSize(11.0f).lineHeight(32.0f)
                    .color({0.52f, 0.57f, 0.65f, 1.0f})
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center).build();
                btnSec("ov.b1b", kBldBX, 50.0f, kBldBW, "选择新版 PXC 路径",
                       [] { guarded("选择新版 PXC 路径", onBldPickBaselineFromExe); });
                if (g_draftPresent) {
                    // 紧跟两个骨架入口：草稿是"这一轮的续作"，不该跟主流程隔开 280px。
                    // 次要样式：它只在"上次没干完"时才出现，是续作入口，不是这一步的主操作。
                    btnSec("ov.bdr", kBldAfter, 50.0f, 88.0f, "恢复草稿",
                           [] { guarded("恢复草稿", onBldRestoreDraft); });
                    btnSec("ov.bdd", kBldAfter + 96.0f, 50.0f, 88.0f, "丢弃草稿",
                           [] { guarded("丢弃草稿", onBldDropDraft); });
                }
                if (g_bldEnPath.empty()) {
                    btnOff("ov.b2", W - pad - 140.0f, 50.0f, 140.0f, 32.0f, "导入已有汉化包",
                           "用于合并已翻译的新版中文包，必须先导入同版本英文骨架");
                } else {
                    btn("ov.b2", W - pad - 140.0f, 50.0f, 140.0f, "导入已有汉化包",
                        [] { guarded("导入已有汉化包", onBldImportPack); });
                }

                // ---- ③ 骨架 + 两个包的路径：同一行，维护者一眼看出"拿谁当参照比的"----
                ui.text("ov.meta").position(pad, metaY).size(W - pad * 2.0f, (float)metaLines * 14.0f)
                    .text(metaText).fontSize(10.5f).lineHeight(14.0f).wrap(true)
                    .color({0.52f, 0.57f, 0.65f, 1.0f}).build();
                if (bldMsg) {
                    // 高度按估算行数给，和 tabsY 用的是同一个 msgLines（两处必须一致，否则又出空档）。
                    ui.text("ov.stat").position(pad, msgY)
                        .size(W - pad * 2.0f, (float)msgLines * 15.0f)
                        .text(g_ovStatus).fontSize(11.0f).lineHeight(15.0f)
                        .color({0.88f, 0.82f, 0.60f, 1.0f}).wrap(true).build();
                }

                // ---- ③ 分页 + 七个筛选（位置由 tabsY 派生，加减一行不会再把空白撕开）----
                ui.stack("ov.tabs.wrap").position(pad, tabsY).size(leftW, 30.0f).content([&] {
                    components::segmented(ui, "ov.tabs").theme(uiTheme()).size(leftW, 30.0f).fontSize(12.0f)
                        .items(tabLabels)
                        .selected(ovCurPage())
                        .onChange([](int p) { guarded("切换构建器分页", [p] { ovSwitchTab(p); }); })
                        .build();
                }).build();
                // 筛选 chip：自绘。segmented 是**等分宽度**的，7 段塞进 564px 时
                // 每段只剩 ~80px 外壳、再多内边距文字就被压成竖条了（实测），所以这里按内容给宽度。
                // 每个 chip 带上「当前页签下这个档位有多少个**字段**」：光看条目数估不出工作量
                // （一个节点十几个端口），而"待校对 43"这种数字才决定要不要先清这一档。
                // 「全部」那档的数字就是以前单独放在右端的「字段 N」，不再另放一处。
                {
                    float cx = rightX;
                    for (int i = 0; i < 7; ++i) {
                        const std::string lb = std::string(kBldFilterName[i]) + " " +
                                               std::to_string(g_ovFltField[i]);
                        const float cw = ovChipWidth(lb);
                        const bool on = (g_bldFilter.get() == i);
                        const std::string cid = "ov.flt" + std::to_string(i);
                        ui.stack(cid + ".wrap").position(cx, tabsY + 2.0f).size(cw, 26.0f)
                            .content([&, i, cw, lb, on, cid] {
                                ui.rect(cid).size(cw, 26.0f)
                                    .states(on ? core::Color{0.22f, 0.42f, 0.62f, 0.95f}
                                               : core::Color{0.17f, 0.19f, 0.23f, 0.85f},
                                            core::Color{0.24f, 0.30f, 0.38f, 0.95f},
                                            core::Color{0.28f, 0.34f, 0.44f, 0.95f})
                                    .radius(6.0f).instantStates()
                                    .onClick([i] {
                                        if (g_bldFilter.get() == i) return;  // 已经在这档，别白算一遍
                                        g_bldFilter.set(i);
                                        ovRefreshView();
                                        OvPageData& q = ovCur();
                                        q.sel = q.view.empty() ? -1 : q.view.front();
                                        ovBuildRows(q.sel);
                                        g_ovScroll.set(0.0f);
                                        g_ovFieldScroll.set(0.0f);
                                        ++g_ovListGen;
                                        ++g_ovFieldGen;
                                        requestUpdate();
                                    }).build();
                                ui.text(cid + ".t").size(cw, 26.0f).text(lb)
                                    .fontSize(11.0f).lineHeight(26.0f)
                                    .color(on ? core::Color{0.96f, 0.98f, 1.0f, 1.0f}
                                              : core::Color{0.70f, 0.76f, 0.86f, 1.0f})
                                    .horizontalAlign(core::HorizontalAlign::Center)
                                    .verticalAlign(core::VerticalAlign::Center).build();
                            }).build();
                        cx += cw + 6.0f;
                    }
                }
            } else {
                ui.text("ov.title").position(pad, 12.0f).size(W - pad * 2.0f, 26.0f)
                    .text("自定义译文 · 只改自己不习惯的那几处").fontSize(17.0f).lineHeight(22.0f)
                    .fontWeight(700).color({0.95f, 0.97f, 1.0f, 1.0f}).build();
                ui.text("ov.sub").position(pad, 40.0f).size(W - pad * 2.0f, 18.0f)
                    .text("对照表是随程序分发的固定内容；这里只存你自己改过的条目，会覆盖生成结果里的对应字段。"
                          "留空 = 用默认译文。搜索同时匹配原名与现用名（不区分大小写）。")
                    .fontSize(11.0f).lineHeight(15.0f).color({0.55f, 0.60f, 0.68f, 1.0f}).build();
                ui.stack("ov.tabs.wrap").position(pad, tabsY).size(leftW, 30.0f).content([&] {
                    components::segmented(ui, "ov.tabs").theme(uiTheme()).size(leftW, 30.0f).fontSize(12.0f)
                        .items(tabLabels)
                        .selected(ovCurPage())
                        .onChange([](int p) { guarded("切换自定义译文层分页", [p] { ovSwitchTab(p); }); })
                        .build();
                }).build();
            }

            // 左：搜索 + 条目列表
            ui.stack("ov.q.wrap").position(pad, listY - 42.0f).size(leftW, 34.0f).content([&] {
                components::input(ui, "ov.q").theme(uiTheme()).size(leftW, 34.0f)
                    .fontSize(12.0f).placeholder(notesPage ? "搜索文件名（不区分大小写）"
                                                          : "搜索：中文 / 原中文 / 英文 / ID，空格分隔多词")
                    .bind(g_ovQuery)
                    .onChange([](const std::string& v) {
                        g_ovQuery.set(v);   // bind 的回调被这里替换了，得自己写回
                        g_ovScroll.set(0.0f);
                        ++g_ovListGen;
                        requestUpdate();
                    })
                    .build();
            }).build();
            components::virtualList(ui, "ov.list." + std::to_string(g_ovListGen))
                .position(pad, listY).size(leftW, std::max(60.0f, bodyEnd - listY))
                .itemCount((std::int64_t)pd.view.size()).rowHeight(30.0f)
                .bind(g_ovScroll).row(composeOvItemRow).build();

            // 右：选中条目的字段（虚拟列表，行高按分页决定）。
            // 「待翻译 / 待校对」开关在**每个字段的标题行右端**（字段级，见 composeOvFieldRow）：
            // 同一节点里"名字要翻、端口提示不翻"是常态，条目级一刀切会把不想翻的也塞进导出队列。
            // 标题行右端两个小按钮：清除「当前条目」的待翻译/待校对标记（构建器才有）。
            // 标题文本宽度相应收窄，避免压到按钮。没标记时按钮置灰（见 btnOff）。
            const float cbW = 82.0f, cbH = 22.0f, cbGap = 6.0f;
            const float cbX = rightX + rightW - (cbW * 2.0f + cbGap);
            ui.text("ov.rid")
                .position(rightX, contentY)
                .size(bld ? (rightW - (cbW * 2.0f + cbGap + 10.0f)) : rightW, 22.0f)
                .text(selHint)
                .fontSize(13.0f).lineHeight(18.0f).fontWeight(600)
                .color({0.93f, 0.95f, 1.0f, 1.0f}).build();
            if (bld) {
                const bool hasPt = hasSel && pd.items[(size_t)selIdx].pendTr;
                const bool hasPr = hasSel && pd.items[(size_t)selIdx].pend;
                if (hasSel && hasPt)
                    btn("ov.epr.pt", cbX, contentY, cbW, "清除待翻译",
                        [] { openConfirm("清除本条目待翻译标记？",
                                "会去掉当前条目下所有字段的「待翻译」标记。已导出的待翻译队列不受影响，只是把标记摘掉——之后想再导出得重新打标。",
                                [] { ovClearEntryPending(0); }); }, cbH);
                else
                    btnOff("ov.epr.pt", cbX, contentY, cbW, cbH, "清除待翻译",
                           "本条目没有「待翻译」标记");
                if (hasSel && hasPr)
                    btn("ov.epr.pr", cbX + cbW + cbGap, contentY, cbW, "清除待校对",
                        [] { openConfirm("清除本条目待校对标记？",
                                "会去掉当前条目下所有字段的「待校对」标记。",
                                [] { ovClearEntryPending(1); }); }, cbH);
                else
                    btnOff("ov.epr.pr", cbX + cbW + cbGap, contentY, cbW, cbH, "清除待校对",
                           "本条目没有「待校对」标记");
            }
            components::virtualList(ui, "ov.fields." + std::to_string(g_ovFieldGen))
                .position(rightX, contentY + 28.0f)
                .size(rightW, std::max(60.0f, bodyEnd - contentY - 28.0f))
                .itemCount((std::int64_t)g_ovRows.size()).rowHeight(ovFieldRowHeight())
                .bind(g_ovFieldScroll).row(composeOvFieldRow).build();

            // 底部：统计行 + 输出版本 + 出口按钮（构建器）/ 状态 + 两个按钮（自定义译文）
            // 构建器底栏 86 高：上一行是「可译/已译/待翻译」统计（贴近它真正影响的导出动作），
            // 下一行才是输出版本 + 五个按钮。
    const float by = bld ? (H - bottomBar + 44.0f) : (H - bottomBar + 8.0f);
    if (bld) {
                // 底栏右端"标记管理"一排按钮的统一坐标：清除待译 / 清除待校 / 导出 / 导入。
                // 提前算好，统计文本和按钮共用，避免两处各写一遍后错位。covY 行底边压在编辑区底衬上。
                const float qw = 112.0f, qh = 22.0f, qy = covY - 8.0f;
                const float clW = 116.0f, clGap = 8.0f;  // 放得下"清除全部待翻译/清除全部待校对"7 个汉字
                const float importX = W - pad - qw;
                const float exportX = importX - qw - clGap;
                const float clPrX = exportX - clW - clGap;   // 清除全部待校对
                const float clPtX = clPrX - clW - clGap;     // 清除全部待翻译
                // 每进程记一次实际排版数值：对话框高度会被框架按屏幕再钳一遍，
                // 光看截图量像素对不上号（曾被钳掉 28px 只能靠反推）。
                static bool layoutLogged = false;
                if (!layoutLogged) {
                    layoutLogged = true;
                    char lb[256];
                    sprintf_s(lb, "排版 H=%.0f bodyEnd=%.0f tabsY=%.0f listY=%.0f covY=%.0f by=%.0f",
                              H, bodyEnd, tabsY, listY, covY, by);
                    pxc_log::write("INFO", "Builder", lb);
                }
                {
                    // 用字符串拼接而不是 sprintf_s：格式串里混着全角字符，MSVC 在 GBK 代码页下
                    // 会数错占位符个数（C4474/C4477 一串警告），而这里没有任何性能理由冒险。
                    const double pct =
                        g_bldCov.total ? 100.0 * g_bldCov.translated / g_bldCov.total : 0.0;
                    char pctBuf[16];
                    sprintf_s(pctBuf, "%.1f", pct);
                    const std::string cov =
                        "可译 " + std::to_string(g_bldCov.total) + " · 已译 " +
                        std::to_string(g_bldCov.translated) + "（" + pctBuf + "%）· 待翻译 " +
                        std::to_string(g_bldCov.pendingTrans) + " · 待校对 " +
                        std::to_string(g_bldCov.pending) + " · 孤儿 " +
                        std::to_string(g_bldCov.orphan) +
                        (g_dbDirty ? " · 有未保存改动（已自动存草稿）" : "");
                    // 右侧这一排按钮（清除全部待翻译 / 清除全部待校对 / 导出 / 导入）占掉约 480px，统计文本得让开。
                    ui.text("ov.cov").position(pad, covY - 8.0f).size(clPtX - pad - 8.0f, 16.0f)
                        .text(cov).fontSize(10.5f).lineHeight(16.0f)
                        .color(g_dbDirty ? core::Color{0.88f, 0.82f, 0.60f, 1.0f}
                                         : core::Color{0.62f, 0.68f, 0.78f, 1.0f})
                        .build();
                }
                // 右端这一排都是"标记管理"动作：清除全部待翻译 / 清除全部待校对 / 导出待翻译队列 / 导入译文。
                // 清除全部会抹掉整张表的标记（危险），所以走二次确认，且在没有对应标记时直接置灰。
                // 它们和覆盖率数字是一件事的两面（数字说"还剩多少"，按钮说"把这批送走/清掉"），故同行。
                {
                    if (g_bldCov.pendingTrans > 0) {
                        btn("ov.clrpt", clPtX, qy, clW, "清除全部待翻译",
                            [] { openConfirm("清除全部待翻译标记？",
                                    "会去掉整张对照表里所有字段的「待翻译」标记（共 " +
                                    std::to_string((int)g_db.pendingTranslate.size()) + " 处）。" +
                                    "已导出的待翻译队列不受影响，只是把标记摘掉——之后想再导出得重新打标。",
                                    [] { ovClearAllPending(0); }); }, qh);
                    } else {
                        btnOff("ov.clrpt", clPtX, qy, clW, qh, "清除全部待翻译",
                               "当前没有任何「待翻译」标记可清除");
                    }
                    if (g_bldCov.pending > 0) {
                        btn("ov.clrpr", clPrX, qy, clW, "清除全部待校对",
                            [] { openConfirm("清除全部待校对标记？",
                                    "会去掉整张对照表里所有字段的「待校对」标记（共 " +
                                    std::to_string((int)g_db.pendingReview.size()) + " 处）。",
                                    [] { ovClearAllPending(1); }); }, qh);
                    } else {
                        btnOff("ov.clrpr", clPrX, qy, clW, qh, "清除全部待校对",
                               "当前没有任何「待校对」标记可清除");
                    }
                    if (g_bldCov.pendingTrans > 0) {
                        btn("ov.bq", exportX, qy, qw, "导出待翻译 json",
                            [] { guarded("导出待翻译", onBldExportQueue); }, qh);
                    } else {
                        btnOff("ov.bq", exportX, qy, qw, qh, "导出待翻译 json",
                               "当前没有标记为「待翻译」的条目；在右侧字段标题行勾上它就会进导出队列");
                    }
                    btn("ov.bi", importX, qy, qw, "导入已翻译 json",
                        [] { guarded("导入已翻译", onBldImportTranslations); }, qh);
                }
                // 「输出版本」= 保存/导出时写进 meta.version 的值。
                // 它和标题行的"当前对照表版本"是两件事：一个是"要发布成什么"，一个是"现在磁盘上是什么"。
                ui.text("ov.outver.lab").position(pad, by - 1.0f).size(120.0f, 14.0f).text("输出版本")
                    .fontSize(10.5f).lineHeight(14.0f).color({0.55f, 0.60f, 0.68f, 1.0f}).build();
                ui.stack("ov.outver.wrap").position(pad, by + 8.0f).size(180.0f, 22.0f).content([&] {
                    components::input(ui, "ov.outver").theme(uiTheme()).size(180.0f, 22.0f)
                        .fontSize(11.5f).placeholder("如 1.22.0.6-机翻版").bind(g_bldVer).build();
                }).build();
                // 底栏只剩"输出"这一组：输出版本 + 两个出口 + 关闭。
                // 「导出待翻译 / 导入译文」已上移到主流程按钮行（它们属于流程，不属于出口）。
                const float bx = W - pad - 388.0f;
                btn("ov.mk", bx, by, 130.0f, "生成对照表文件…",
                    [] { guarded("生成对照表文件", onBldExportMerged); });
                // 危险动作：直接把编辑结果覆盖运行期表。没有未保存改动时统一置灰（仍是按钮形状）。
                if (g_dbDirty) {
                    btn("ov.save", bx + 138.0f, by, 176.0f, "生成并覆盖当前对照表",
                        [] { guarded("确认覆盖对照表", [] { g_confirmOverwrite = true; requestUpdate(); }); });
                } else {
                    btnOff("ov.save", bx + 138.0f, by, 176.0f, 32.0f, "生成并覆盖当前对照表",
                           "当前没有未保存的改动，无需覆盖；改了内容（导入 / 编辑 / 打标）后即可点击");
                }
                btn("ov.close", bx + 322.0f, by, 66.0f, "关闭", [] { guarded("关闭构建器", onOvClose); });
            } else {
                std::string stat = g_ovStatus;
                const float rightBtns = 270.0f;
                ui.text("ov.stat").position(pad, by).size(W - pad * 2.0f - rightBtns - 10.0f, 32.0f)
                    .text(stat).fontSize(11.0f).lineHeight(15.0f).color({0.62f, 0.68f, 0.78f, 1.0f})
                    .wrap(true).verticalAlign(core::VerticalAlign::Center).build();
                const float bx = W - pad - 270.0f;
                btn("ov.clr", bx, by, 130.0f, "清除本条目", [] { guarded("清除本条目", ovClearNode); });
                btn("ov.done", bx + 140.0f, by, 130.0f, "完成（保存）",
                    [] { guarded("完成", onOvClose); });
            }

            // 悬浮提示：画在内容的最末尾 = 盖在所有控件之上（面板不裁剪，可以压在列表上）。
            if (!g_tipText.empty()) {
                const float th = (float)g_tipLines * 15.0f + 14.0f;
                ui.stack("ov.tip").position(g_tipX, g_tipY).size(g_tipW, th).zIndex(30).content([&] {
                    ui.rect("ov.tip.bg").size(g_tipW, th)
                        .color({0.09f, 0.11f, 0.14f, 0.98f}).radius(6.0f)
                        .border(1.0f, core::Color{0.32f, 0.37f, 0.44f, 1.0f}).build();
                    ui.text("ov.tip.t").position(8.0f, 0.0f).size(g_tipW - 16.0f, th)
                        .text(g_tipText).fontSize(10.5f).lineHeight(15.0f).wrap(true)
                        .color({0.92f, 0.95f, 1.0f, 1.0f})
                        .verticalAlign(core::VerticalAlign::Center).build();
                }).build();
            }
        })
        .build();

    // 覆盖前二次确认：这一步会把编辑结果写进运行期表（下次「生成并应用」立刻用它）。
    // 写错了不是"重新生成"能救的 —— 旧表虽然备份在 userdata/.db_backup/，但生成出来的语言文件
    // 已经被用户拿去用了。所以宁可多一次点击。
    if (g_confirmOverwrite) {
        components::dialog(ui, "ov.confirm")
            .theme(uiTheme())
            .screen(screen.width, screen.height)
            .size(460.0f, 184.0f)
            .zIndex(400)
            .open(true)
            .onOpenChange([](bool v) {
                if (!v) {
                    g_confirmOverwrite = false;
                    requestUpdate();
                }
            })
            .content([&] {
                ui.text("cf.t").position(24.0f, 20.0f).size(412.0f, 24.0f)
                    .text("生成并覆盖当前对照表？").fontSize(15.0f).lineHeight(22.0f)
                    .fontWeight(700).color({0.96f, 0.97f, 1.0f, 1.0f}).build();
                ui.text("cf.m").position(24.0f, 50.0f).size(412.0f, 64.0f)
                    .text("会用编辑区当前内容重写 data/translation_db.json，版本号为「" +
                          (g_bldVer.get().empty() ? std::string("(沿用当前)") : g_bldVer.get()) +
                          "」。覆盖前旧表会复制一份到 userdata/.db_backup/（文件名带版本号与时间）；"
                          "已经生成出去的语言文件不回滚。")
                    .fontSize(11.5f).lineHeight(17.0f).color({0.72f, 0.77f, 0.86f, 1.0f}).wrap(true).build();
                btn("cf.no", 218.0f, 132.0f, 100.0f, "取消", [] {
                    g_confirmOverwrite = false;
                    requestUpdate();
                });
                btn("cf.yes", 328.0f, 132.0f, 108.0f, "确认覆盖", [] {
                    g_confirmOverwrite = false;
                    guarded("覆盖对照表", onBldSave);
                });
            })
            .build();
    }

    // 通用二次确认（清除标签等）：内容与回调在 openConfirm() 里按动作填好。
    if (g_confirmOpen) {
        components::dialog(ui, "ov.confirm2")
            .theme(uiTheme())
            .screen(screen.width, screen.height)
            .size(460.0f, 184.0f)
            .zIndex(410)
            .open(true)
            .onOpenChange([](bool v) {
                if (!v) {
                    g_confirmOpen = false;
                    g_confirmFn = nullptr;
                    requestUpdate();
                }
            })
            .content([&] {
                ui.text("cf2.t").position(24.0f, 20.0f).size(412.0f, 24.0f)
                    .text(g_confirmTitle).fontSize(15.0f).lineHeight(22.0f)
                    .fontWeight(700).color({0.96f, 0.97f, 1.0f, 1.0f}).build();
                ui.text("cf2.m").position(24.0f, 50.0f).size(412.0f, 64.0f)
                    .text(g_confirmMsg)
                    .fontSize(11.5f).lineHeight(17.0f).color({0.72f, 0.77f, 0.86f, 1.0f}).wrap(true).build();
                btn("cf2.no", 218.0f, 132.0f, 100.0f, "取消", [] {
                    g_confirmOpen = false;
                    g_confirmFn = nullptr;
                    requestUpdate();
                });
                btn("cf2.yes", 328.0f, 132.0f, 108.0f, "确认", [] {
                    auto fn = std::move(g_confirmFn);
                    g_confirmOpen = false;
                    if (fn) fn();
                });
            })
            .build();
    }
}


// 旧版本把可变状态直接放在 bin/（launcher.json / pxc_backups / skeleton /
// data/projects.json / data/.cache / data/.db_backup / data/.draft / data/notes_zh）。
// 统一迁到 userdata/，让 bin/ 只剩程序 + 只读资源。一次性、幂等、失败静默。
static void migrateLegacyState() {
    const std::filesystem::path ud = pxc_nui::userDataDir();
    auto moveOne = [&](const std::filesystem::path& src) {
        if (src.empty()) return;
        std::error_code ec;
        if (!std::filesystem::exists(src, ec)) return;
        std::filesystem::create_directories(ud, ec);
        const std::filesystem::path dst = ud / src.filename();
        if (std::filesystem::equivalent(src, dst, ec)) return;
        std::filesystem::rename(src, dst, ec);
        if (ec) {  // 跨卷 rename 失败：递归拷贝后删源
            std::filesystem::copy(src, dst,
                                 std::filesystem::copy_options::recursive |
                                     std::filesystem::copy_options::overwrite_existing,
                                 ec);
            if (!ec) std::filesystem::remove_all(src, ec);
        }
    };
    moveOne(g_exeDir / "launcher.json");
    moveOne(g_exeDir / "pxc_backups");
    moveOne(g_exeDir / "skeleton");
    moveOne(g_exeDir / "logs");
    moveOne(dataDir() / "projects.json");
    moveOne(dataDir() / ".cache");
    moveOne(dataDir() / ".db_backup");
    moveOne(dataDir() / ".draft");
    moveOne(dataDir() / "notes_zh");
}

// ---------- EUI 入口 ----------
// 一次性初始化：读配置、扫字体、载入对照表、恢复界面状态。
// 注意：这里绝不能放进 dslAppConfig()，框架每帧会为了取 fps / uiScale / title 等
// 值多次调用那个函数，任何副作用（读盘、扫目录、写日志、Signal::set）都会被放大成
// 每帧几十次——曾导致 11 MB 日志、界面输入被反复重置。
static void initOnce() {
    static bool done = false;
    if (done) return;
    done = true;

    g_exeDir = exeDir();
    migrateLegacyState();   // 旧版落在 bin/ 的可变状态搬到 userdata/（幂等，失败静默）
    pxc_log::installCrashHandler();
    int regDupRemoved = 0;
    auto [cfg, ok] =
        pxc_store::load(pxc_nui::userDataDir() / "launcher.json", g_exeDir, &regDupRemoved);
    g_cfg = cfg;
    // 基准指纹 = 刚读进来的这份：不设的话第一次 refresh 会把"没变"当成"变了"，白写一次盘。
    // 例外：读盘时合并过重复条目（老版本相对路径写法留下的），内存与磁盘已经不一致，
    // 基准必须留空，让紧随其后的第一次刷新真正把它写回 —— 否则文件里永远躺着那堆重复。
    g_regSavedSig = regDupRemoved > 0 ? std::string() : registrySignature();
    if (regDupRemoved > 0)
        pxc_log::write("INFO", "Registry",
                       "合并重复登记 " + std::to_string(regDupRemoved) + " 条（历史相对路径写法）");
    if (!ok) pxc_log::write("WARN", "Config", "launcher.json 不存在或解析失败，使用默认配置");
    if (g_cfg.locale.mode.empty()) g_cfg.locale.mode = "zh";
    // 缩放档位初值：滑块位置与"已生效值"都以配置为准（越界值在这里被夹回合法档）。
    g_uiScaleIdx = setUiScaleIndex(g_cfg.ui.scale);
    g_uiScalePending = g_uiScaleIdx;
    g_cfg.ui.scale = setUiScaleAt(g_uiScaleIdx);
    g_textScaleIdx = setTextScaleIndex(g_cfg.ui.text_scale);
    g_textScalePending = g_textScaleIdx;
    g_cfg.ui.text_scale = kTextScales[g_textScaleIdx];
    // 提示窗（原生 GDI）的字号也要跟着首帧的设定，否则它永远停在 100%。
    pxc_runner::setToastTextScale(g_cfg.ui.text_scale);
    // 主题色：配置里的十六进制 → 全局强调色。解析不了就退回默认（store 侧也会校验一次，
    // 这里再兜一层是因为 launcher.json 允许用户手改）。
    if (!hexToColor(g_cfg.ui.accent, g_accent)) {
        g_cfg.ui.accent = kAccentDefault;
        hexToColor(g_cfg.ui.accent, g_accent);
    }
    // 启动时刷新一次（docs/07 §11.3 的第一处触发）：上次退出后 Steam 可能更新过、库可能搬过。
    // 这里是"读完配置后的第一个动作"，所以登记表从这一行起就是磁盘事实。
    // 版本探测缓存要先载入：否则这一轮刷新会把每条 exe 都重新扫一遍（§11.8.2）。
    pxc_probe::load(probeCachePath());
    loadRegIcons();
    refreshRegistry("启动");
    ensureDb();   // 对照表：随程序分发的只读资源
    scanFonts();
    // 用户目录：启动时自己找，**不弹窗让用户选**（docs/08 §8.7）。
    // 只在"配置里没存"或"存的那个已经不在了"时才找；用户手选过的路径要优先留住 ——
    // 他曾把 PXC 用户目录挪到 D 盘，自动查找给出的候选未必是他要的那个。
    {
        std::error_code ec;
        const std::filesystem::path saved = pxc_util::pathFromUtf8(g_cfg.gen.user_dir);
        if (saved.empty() || !std::filesystem::is_directory(saved, ec)) {
            const auto dirs = pxc_base::findUserDirs();
            if (!dirs.empty()) {
                g_cfg.gen.user_dir = dirs[0].u8string();
                saveConfig();
                pxc_log::write("INFO", "Prefs", "自动定位用户目录：" + g_cfg.gen.user_dir);
            }
        }
    }
    if (!g_cfg.gen.lang_dir.empty()) g_langDir.set(g_cfg.gen.lang_dir);
    g_exNodeName.set(g_cfg.gen.ex_node_name);
    g_exNodeTip.set(g_cfg.gen.ex_node_tip);
    g_exInName.set(g_cfg.gen.ex_in_name);
    g_exOutName.set(g_cfg.gen.ex_out_name);
    g_exDd.set(g_cfg.gen.ex_dd);
    g_exText.set(g_cfg.gen.ex_text);
    g_preset.set(g_cfg.gen.preset);

    // 字体：配置里没有（或指向的文件已不在）时自动落到内置字体。
    // 中文一旦没字体就是满屏豆腐块，不能赌用户手工选过。
    auto pickFont = [](const std::string& saved, const char* preferTag, const char* preferName) {
        int idx = fontIndex(saved);
        if (idx >= 0) return idx;
        int pref = -1, first = -1;
        for (int i = 0; i < (int)g_fonts.size(); ++i) {
            const std::string& n = g_fonts[i].first;
            if (n.rfind(preferTag, 0) != 0) continue;      // 只在内置字体里挑
            if (first < 0) first = i;
            if (pref < 0 && preferName && preferName[0] &&
                n.find(preferName) != std::string::npos)
                pref = i;
        }
        return pref >= 0 ? pref : first;
    };
    int ufi = pickFont(g_cfg.gen.ui_font, "[内置]", "NotoSansSC");
    int cfi = pickFont(g_cfg.gen.code_font, "[内置]", "FiraCode");
    if (cfi < 0) cfi = ufi;
    if (ufi >= 0) g_cfg.gen.ui_font = g_fonts[ufi].second.u8string();
    if (cfi >= 0) g_cfg.gen.code_font = g_fonts[cfi].second.u8string();
    g_fontUi.set(ufi);
    g_fontCode.set(cfi);

    // 单实例 + 命令行：双击 .pxc 时 shell 执行 "<launcher.exe>" "<file>"。
    // 第二个实例不建窗：把路径丢给已有窗口就退（避免 projects.json 互相覆盖，
    // 也避免同一个工程被两个 PXC 写 autosave）。
    {
        std::vector<std::filesystem::path> argvPaths;
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i < argc; ++i) {
                std::filesystem::path pp(argv[i]);
                std::error_code ec;
                if (isPxcFile(pp) && std::filesystem::is_regular_file(pp, ec)) argvPaths.push_back(pp);
            }
            LocalFree(argv);
        }
        wbBoot();
        if (!pxc_single::acquire()) {
            // 有文件要开 → 转发；没有（用户又点了一次图标）→ 把已有窗口提到前台。
            // 只转发不激活的话，第二种情况表现为"点了完全没反应"，用户会以为程序没启动、然后连点。
            if (!pxc_single::notifyExisting(argvPaths)) pxc_single::activateExisting();
            pxc_log::write("INFO", "Single", "已有实例，本进程退出");
            ExitProcess(0);
        }
        if (!argvPaths.empty()) {
            g_wbInbox = argvPaths;
            g_page = Page::Projects;
            g_silentLaunch = g_wbStore.assoc.silentLaunch;
        }
    }

    // 冒烟用：PXC_LAUNCHER_DEBUG_PAGE=1..3 直接落到某个页面（1 版本库 / 2 工程 / 3 汉化）。
    if (wchar_t b[8] = {0}; GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_PAGE", b, 8) > 0) {
        int v = _wtoi(b);
        if (v >= 1 && v <= 4) g_page = (Page)(v - 1);   // 4 = 设置
    }
    // 冒烟用：PXC_LAUNCHER_DEBUG_SETTAB=0|1|2 直接落到设置页的「通用 / 界面 / 汉化」页签。
    if (wchar_t b[8] = {0}; GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_SETTAB", b, 8) > 0) {
        g_page = Page::Settings;
        const int v = _wtoi(b);
        g_setTab = (v >= 0 && v <= 2) ? v : 0;
    }
    // 冒烟用：PXC_LAUNCHER_DEBUG_PICKER=1 直接把主题色取色弹窗打开（合成鼠标点不到色块时用）。
    if (wchar_t b[8] = {0}; GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_PICKER", b, 8) > 0) {
        if (_wtoi(b) == 1) {
            g_page = Page::Settings;
            g_setTab = 1;
            g_accentPickerOpen.set(true);
        }
    }

    // 冒烟用：PXC_LAUNCHER_DEBUG_WB_TAB=0..5 直接落到工作台某个页签
    // （0 近期 / 1 全部 / 2 工程库 / 3 启动历史 / 4 收藏 / 5 自动恢复）。
    // 伪造鼠标点页签会受"发送坐标 vs 实际命中"的偏移影响（实测点 y=119 打到列头上），
    // 截图核对靠这个钩子才稳。
    if (wchar_t b[8] = {0}; GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_WB_TAB", b, 8) > 0) {
        int v = _wtoi(b);
        if (v >= 0 && v < pxc_index::kTabCount) {
            g_wb.tab = v;
            g_wb.dir.clear();
            g_wb.crumbs.clear();
        }
    }
    // 冒烟用：PXC_LAUNCHER_DEBUG_WB_FILTER=<states 位掩码> 预置状态筛选，
    // 直接验证「点识别不出能筛出红色项」而不依赖在浮窗里点 chip。
    if (wchar_t b[8] = {0}; GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_WB_FILTER", b, 8) > 0) {
        g_wb.filter.states = _wtoi(b);
    }
    // 冒烟用：PXC_LAUNCHER_DEBUG_WB_HOVER=<行号> 把该行的路径置为悬浮态。
    // 悬浮靠真实鼠标进入 mouseArea 触发，截图环境里 SetCursorPos 唤不醒 glfwWaitEvents，
    // 所以「下划线长度 == 文字长度」这件事只能靠这个钩子拍下来核对。
    if (wchar_t b[8] = {0}; GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_WB_HOVER", b, 8) > 0) {
        g_wbHoverSub = _wtoi(b);
    }
    // 冒烟用：PXC_LAUNCHER_DEBUG_WB_DIR=<目录> 直接进入该文件夹（等价双击进入），
    // 用于截图核对文件夹内的四态徽章 / 文件行，不用去点卡片。
    if (wchar_t b[1024] = {0}; GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_WB_DIR", b, 1024) > 0) {
        wbLoadTab(g_wb.tab, true);                 // 先把根层装好（幂等）
        const std::string dir = pxc_util::toUtf8(b);
        std::string leaf = "文件夹";
        try {
            const auto fn = std::filesystem::path(pxc_util::pathFromUtf8(dir)).filename().wstring();
            if (!fn.empty()) leaf = pxc_util::toUtf8(fn.c_str());
        } catch (...) {
        }
        g_wb.crumbs.emplace_back(leaf, dir);
        wbLoadLayer(dir);
    }

    // 冒烟用：PXC_LAUNCHER_DEBUG_ADDEXE=<exe 路径> 启动后直接走一次"登记"，
    // 跳过模态文件对话框 —— 用于验「无法识别版本」与「同渠道同版本二选一」这两条分支，
    // 它们都要求在登记的瞬间弹窗，靠合成鼠标点不开 IFileDialog。
    if (wchar_t b[1024] = {0}; GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_ADDEXE", b, 1024) > 0) {
        std::error_code ec;
        const std::filesystem::path p(b);
        if (std::filesystem::is_regular_file(p, ec)) {
            g_page = Page::Registry;
            guarded("调试登记 exe", [p] { registerExe(p); });
        } else {
            pxc_log::write("WARN", "Debug", "PXC_LAUNCHER_DEBUG_ADDEXE 指向的不是文件：" +
                                                pxc_util::toUtf8(b));
        }
    }

    // 冒烟用：PXC_LAUNCHER_DEBUG_NOPREFDLG=1 让"进版本库页就弹偏好询问"不触发，
    // 只为验另一条触发：双击卡片启动时先问再起（进页那条会先弹，正常场景下它先命中）。
    if (wchar_t b[8] = {0}; GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_NOPREFDLG", b, 8) > 0)
        g_dbgNoPrefDlg = (b[0] == L'1');

    pxc_log::write("INFO", "App",
                   "应用启动 | 对照表 " + std::to_string((int)g_db.entries.size()) + " 条 | 字体 " +
                       std::to_string((int)g_fonts.size()) + " 个 | 语言目录 " + g_langDir.get());
}

DslAppConfig& mutableDslAppConfig() {
    // 纯 getter 的可写版本：框架每帧多次调用，这里只返回已构造好的单例，**不做任何副作用**。
    static DslAppConfig config = [] {
        initOnce();
        DslAppConfig cfg;
        cfg.title("PXC 启动器").fps(60.0).clearColor({0.13f, 0.15f, 0.17f, 1.0f});
        // 界面字体与图标字体：**必须显式指定**，不能吃 EUI 的默认值。
        // EUI 的默认是 `assets/JingNanJunJunTi-...ttf` 与 `assets/Font Awesome 7 Free-Solid-900.otf`，
        // 那是它 demo 用的资源；vendored 目录里的 assets/ 已整份移除（含商业音乐与着色器，
        // 见 EUI-NEO-main/MODIFICATIONS.md），默认值因此指向不存在的文件。
        // 后果是静默的：字体栈找不到主字体就退回系统字体，汉字换字形、`ui.text().icon()`
        // 的图标全变成缺字方框，**不报错也不进日志**（2026-11-05 踩过）。
        // 所以字体归属改到启动器自己的 data/fonts，路径给绝对路径 —— resolveFontFilePath
        // 只会在 <exe>/assets 下按文件名找，给相对名照样落空。
        {
            std::error_code fec;
            const std::filesystem::path uiFont = fontsRoot() / "JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf";
            const std::filesystem::path iconFont = fontsRoot() / "Font Awesome 7 Free-Solid-900.otf";
            const bool hasUi = std::filesystem::exists(uiFont, fec);
            const bool hasIcon = std::filesystem::exists(iconFont, fec);
            if (hasUi) cfg.textFont(pxc_util::toUtf8(uiFont.wstring().c_str()));
            if (hasIcon) cfg.iconFont(pxc_util::toUtf8(iconFont.wstring().c_str()));
            // 落一行日志：这条链失效时界面只是「悄悄换了字体」，看不出错也搜不到原因，
            // 唯一能在事发现场留下的证据就是这行。
            pxc_log::write("INFO", "Font",
                           std::string("界面字体=") + (hasUi ? "自定义" : "缺失→系统字体") +
                               " | 图标字体=" + (hasIcon ? "FontAwesome" : "缺失→系统字体") + " | 目录=" +
                               pxc_util::toUtf8(fontsRoot().wstring().c_str()));
        }
        // 文本缩放：**必须在这里进内核**。initialize() 只读一次 config.textScaleValue，
        // 早于第一次 compose —— 这正是"重启后字号回到 100%"那个 bug 的修法：以前只有
        // applyTextScale() 改内核，启动路径上没人读配置，于是每帧都按 1.0 渲染，
        // 直到用户再动一次滑块才恢复。值 = 基准 1.4 × 用户倍率。
        cfg.textScale(effectiveTextScale(g_cfg.ui.text_scale));
        // 界面缩放：建窗时就按配置来（存的 2.0 下次启动立刻生效，不再走确认条）。
        // 窗口尺寸必须**一起放大**：只改 uiScale 不改窗口，逻辑视口会缩水、工作台直接重排
        // （实测 uiScale=2 时 520×360，卡片 3 列变 1 列并叠字，docs/06 §10.4.1.1）。
        {
            const float s = setUiScaleAt(setUiScaleIndex(g_cfg.ui.scale));
            int w = (int)(1040.0f * s + 0.5f);
            int h = (int)(720.0f * s + 0.5f);
            // 换小屏时不能建一个比屏幕还大的窗（否则用户连"恢复"按钮都点不到）：
            // 建窗前拿系统工作区夹一次；运行期改尺寸在 native_ui::setMainWindowClientSize 里再夹。
            RECT wa{};
            if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
                const int aw = wa.right - wa.left, ah = wa.bottom - wa.top;
                if (w > aw) w = aw;
                if (h > ah) h = ah;
            }
            cfg.windowSize(w, h).uiScale(s);
        }
        // 静默启动（双击 .pxc）：建窗时不显示。
        // 实测（tmp/probe_flash.py）：GLFW 的 show 落在首帧，我们的 hide 比它晚约两帧 ——
        // 屏幕上会闪一块完整的 1056x759 窗口（t≈983→1043 ms，约 60 ms）。用 GLFW_VISIBLE=false
        // 让窗口从一开始就不露脸，再在首帧里显式最小化到任务栏。
        if (g_silentLaunch) cfg.visible(false);
        // 冒烟钩子：PXC_LAUNCHER_DEBUG_UISCALE=2 强制界面缩放。
        // 用途只有一个 —— 验「uiScale != 1 时点击命不命中」（docs/06 §10.6.1）。
        // 不靠它的话，得先把设置页做出来才能测这个尖峰，顺序就反了。
        wchar_t ub[32] = {0};
        if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_UISCALE", ub, 32) > 0) {
            const float v = static_cast<float>(_wtof(ub));
            if (v > 0.0f) cfg.uiScale(v);
        }
        return cfg;
    }();
    return config;
}

const DslAppConfig& dslAppConfig() { return mutableDslAppConfig(); }

void compose(eui::Ui& ui, const eui::Screen& screen) {
    // 界面缩放确认条：10 秒内没按「保留」就回滚（docs/06 §10.4.1.1）。
    // 放在最前面，是因为倒计时到点要立刻回滚窗口尺寸，不能等这一帧画完。
    // EUI 没有定时器，只能自己续帧 —— 条在的时候每帧都要一次重绘。
    if (g_scaleConfirm.open) {
        if (wbNowMs() >= g_scaleConfirm.deadlineMs) rollbackUiScale();
        else requestUpdate();
    }
    // 静默启动（双击 .pxc）：藏窗口必须放在 compose **最开头**。
    // 框架建窗时窗口就已经可见，等到这一帧画完再藏，用户会看到一块空窗 ——
    // 实测那一刻是 273 ms（16 帧），不是一帧白闪。放在这里只剩建窗到首帧开头的那点间隙。
    if (g_silentLaunch && g_silentStartMs == 0.0) {
        g_silentStartMs = wbNowMs();
        // 常驻：最小化到任务栏就不管了（用户要的是 PXC，不是启动器；弹出来反而抢焦点）。
        // 窗口建出来时是不可见的（GLFW_VISIBLE=false，为了不闪），这里把它交给任务栏。
        // 关掉常驻时退回旧行为：藏起来，等结果出来就退。
        if (g_stayResident) pxc_nui::minimizeToTaskbar();
        else pxc_nui::hideMainWindow();
    }

    // 协商窗提窗口失败时留的重试（见 openVersionPicker）：窗口刚建出来那一帧可能是不可见的，
    // SetForegroundWindow 那次拿不到东西；下一帧再试，仍失败就靠 forceForeground 里的闪任务栏。
    if (g_fgRetry > 0) {
        --g_fgRetry;
        if (pxc_nui::showMainWindow()) {
            g_fgRetry = 0;
            pxc_log::write("INFO", "Launch", "缺版本协商：重试提窗口成功");
        } else {
            requestUpdate();
        }
    }

    // worker / 提示窗只起一次（首帧窗口已就绪，能拿到 HWND 与字体目录）。
    if (!g_runnerReady) {
        g_runnerReady = true;
        pxc_nui::installLaunchDone(kMsgLaunchDone, wbOnWake);
        pxc_runner::init((g_exeDir / "data" / "fonts").wstring(), pxc_nui::mainWindow(),
                         kMsgLaunchDone, true);
        // 提示窗的描边色：init 之后才有窗口，但值可以先给（下一次 show 就会用上）。
        pxc_runner::setToastAccent(accentRgb());
    }

    // 冒烟钩子：PXC_LAUNCHER_DEBUG_APPLYSCALE=<档位下标 0..25> 首帧直接走一次"松手应用"，
    // 用于自动核对「窗口放大 + 确认条 + 10 秒回滚」这条链 —— 合成鼠标在本机沙箱送不进窗口，
    // 但这一段逻辑与滑块回调完全同一条路径（applyUiScale）。
    static bool applyDebugDone = false;
    if (!applyDebugDone) {
        wchar_t b[16] = {0};
        if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_APPLYSCALE", b, 16) > 0) {
            applyDebugDone = true;
            // 没显式指定页面时才落到设置页（验工作台各页在放大后长什么样要能换页）
            if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_PAGE", nullptr, 0) == 0) {
                g_page = Page::Settings;
                g_setTab = 1;
            }
            const int idx = _wtoi(b);
            g_uiScalePending = idx;
            applyUiScale(idx);
        }
    }
    // 同上：PXC_LAUNCHER_DEBUG_APPLYTEXT=<档位下标 0..12> 首帧直接套用一次文本缩放。
    if (!applyDebugDone) {
        wchar_t b[16] = {0};
        if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_APPLYTEXT", b, 16) > 0) {
            applyDebugDone = true;
            if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_PAGE", nullptr, 0) == 0) {
                g_page = Page::Settings;
                g_setTab = 1;
            }
            const int idx = _wtoi(b);
            g_textScalePending = idx;
            applyTextScale(idx);
        }
    }

    // 冒烟钩子：PXC_LAUNCHER_DEBUG_TEXTSWEEP=<起始下标> 每帧推进一档，跑完所有档位停住（不写盘）。
    // 只为验一件事：把每档字号都栅格化过之后，字形图集还装不装得下 ——
    // 图集写满的失败形态是"文字永久消失"，只看单档截图根本发现不了。
    {
        static int sweep = -1;
        static bool sweepChecked = false;
        if (!sweepChecked) {
            sweepChecked = true;   // 只读一次环境变量，别每帧一次系统调用
            wchar_t sb[8] = {0};
            if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_TEXTSWEEP", sb, 8) > 0) sweep = _wtoi(sb);
        }
        if (sweep >= 0 && sweep < kTextScaleCount) {
            const float sv = kTextScales[sweep];
            mutableDslAppConfig().textScale(effectiveTextScale(sv));
            setTextScale(effectiveTextScale(sv));
            g_textScalePending = sweep;
            ++sweep;
            requestUpdate();
        }
    }

    // 冒烟钩子：PXC_LAUNCHER_DEBUG_TOAST=ask|warn|fail|ok|reuse|dialog 直接摆出某个状态。
    // 否则要验「复用失败询问」得真造一次抢不到前台，「60 秒失败」得真等一分钟 —— 都不现实。
    static bool toastDebugDone = false;
    if (!toastDebugDone && g_runnerReady) {
        wchar_t b[64] = {0};
        if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_TOAST", b, 64) > 0) {
            toastDebugDone = true;
            const std::string v = pxc_util::toUtf8(b);
            const char* fn = "飞弹爆炸.pxc";
            const char* ver = "1.20.1";
            pxc_stage::Text t;
            if (v == "ask") {
                t = pxc_stage::makeAskNewProcess(fn, ver, "抢不到前台，未发送按键");
            } else if (v == "warn") {
                t = pxc_stage::makeText(pxc_stage::Stage::Warn, fn, ver, 27);
            } else if (v == "fail") {
                t = pxc_stage::makeText(pxc_stage::Stage::Failed, fn, ver, 0,
                                        "60 秒仍未出现窗口，可能卡住了");
            } else if (v == "ok") {
                t = pxc_stage::makeText(pxc_stage::Stage::Ok, fn, ver);
            } else if (v == "reuse") {
                t = pxc_stage::makeText(pxc_stage::Stage::OkReuse, fn, ver);
            } else {
                t = pxc_stage::makeText(pxc_stage::Stage::ReuseDialog, fn, ver);
            }
            pxc_toast::show(t);
        }
    }

    // 首帧时窗口已创建，此时注册 OS 级拖放（窗口未就绪则返回 false，后续帧自动重试）。
    static bool dropReady = false;
    if (!dropReady) {
        const bool d = pxc_nui::installDrop(onDropPaths);
        const bool c = pxc_nui::installCopyData([](std::vector<std::filesystem::path> paths) {
            // 第二实例转发进来的路径：这里只入队（<50 ms，别让发送方的 3 秒同步消息超时），
            // 然后 Post 一条消息让主线程在窗口过程里处理 —— 主窗最小化时 compose 不跑，
            // 指望 compose 来收单就是"双击了没反应"。
            for (auto& p : paths) g_wbInbox.push_back(p);
            pxc_nui::postToMainWindow(kMsgLaunchDone);
        });
        dropReady = d && c;
    }

    // 窗口图标（exe 资源 GLFW_ICON）。GLFW 只给窗口类设了大图标，16px 那张要靠这里按当前
    // DPI 补上，否则标题栏/Alt+Tab 拿到的是 32 硬缩的糊图。窗口未就绪时返回 false，下帧再试。
    static bool iconReady = false;
    if (!iconReady) iconReady = pxc_nui::applyWindowIcon();

    g_scrH = screen.height;
    g_scrW = screen.width;
    const float navW = kNavW;
    const float contentW = std::max(320.0f, screen.width - navW);
    g_ddFields.clear();  // 本帧重新登记各下拉的位置（滚动/换页都会变）
    ui.row("root").size(screen.width, screen.height).content([&] {
        components::navbar(ui, "navigation")
            .theme(uiTheme()).size(navW, screen.height)
            .brand("", 0x20)   // 0x20 = 空格：字形位置留空；标题也留空，logo 与字样都由 composeBrand 画（字号可自定）
            .selected((int)g_page)
            .items({
                {"registry", "版本库", 0xF015, 0},
                {"projects", "工程文件", 0xF07C, 1},
                {"locale", "汉化", 0xF031, 2},
                {"settings", "设置", 0xF013, 3},
            })
            .onChange([](int p) {
                // 换页 = 放弃那次被偏好询问拦下的启动：弹窗跟着页面消失，此时再补启动
                // 就成了"点了别处却突然起进程"。偏好本身没写，下次进版本库页还会问。
                if ((Page)p != Page::Registry && !g_prefLaunchExe.empty()) {
                    g_prefLaunchExe.clear();
                    g_prefDlgOpen = false;
                }
                g_page = (Page)p;
                g_ddOpenId.clear();  // 换页时收起弹层，否则它会飘在新页面上
                requestUpdate();
            })
            .build();
        // 设置页 / 版本库页：头部（标题、页签或导入刷新按钮）固定在上方，只有正文滚动。
        //
        // 做法是把滚动视口**整体下移**头部高度并相应变矮，头部单独画在视口上方。
        // 不采用"头部画进滚动内容再盖一层不透明底"：那样滚动条会从头部里穿出来，
        // 内容也会从页签底下透过去。
        //
        // 注意 g_pageScroll 仍然是"视口内的滚动量"。这两页里任何"页面坐标 → 屏幕坐标"
        // 的换算都要**再加头部高度**（版本库的右键菜单、hover 浮窗就是这种）；
        // 工程 / 汉化两页没有固定头部，视口起点还是 0，用 `y - g_pageScroll` 的浮层不受影响。
        const bool settingsPage = (g_page == Page::Settings);
        const bool registryPage = (g_page == Page::Registry);
        const float pageHeaderH = settingsPage ? kSetHeaderH : (registryPage ? regv::kHeaderH : 0.0f);
        ui.stack("page.area").size(contentW, screen.height).content([&] {
            components::scrollView(ui, "page.host")
                .position(0.0f, pageHeaderH)
                .theme(uiTheme()).size(contentW, screen.height - pageHeaderH)
                .step(48.0f).scrollbarWidth(10.0f).scrollbarGap(10.0f)
                .bind(g_pageScroll)
                .content([&](eui::Ui& c, float cw, float ch) {
                    c.stack("page.canvas").size(cw, std::max(ch, g_pageH))
                        .content([&] {
                        if (g_page == Page::Registry) composeRegistry(c, cw);
                        else if (g_page == Page::Projects) composeProjects(c, cw);
                        else if (g_page == Page::Locale) composeLocale(c, cw);
                        else composeSettingsBody(c, cw);
                    }).build();
                })
                .build();
            if (settingsPage) {
                ui.stack("page.header").position(0.0f, 0.0f).size(contentW, pageHeaderH).zIndex(4)
                    .content([&] { composeSettingsHeader(ui, contentW); }).build();
            } else if (registryPage) {
                ui.stack("page.header").position(0.0f, 0.0f).size(contentW, pageHeaderH).zIndex(4)
                    .content([&] { composeRegistryHeader(ui, contentW); }).build();
            }
        }).build();
    }).build();

    // 侧栏品牌图标 + 字样：navbar 那行只留了空格/空串，真正的 logo 与「PXC 启动器」叠在这里。
    composeBrand(ui);

    // 下拉弹层画在根级：这样它不受 scrollView 的裁剪影响（见 ddField 的说明）。
    composeDropdownOverlay(ui, screen);
    composeScaleConfirm(ui, screen);   // 缩放确认条：任何页面都在

    // 版本库的三个浮层（右键菜单 / 悬停路径 / 三类弹窗）都画在根级：
    // 页内容装在 scrollView 的裁剪容器里，挂在那里会被裁掉，也会被页面滚动带走。
    if (g_page == Page::Registry) {
        composeRegMenu(ui, screen);
        composeRegDialogs(ui, screen);
        composeRegTooltip(ui, screen);
    }

    // 浮窗放在最外层（最后 build → 覆盖在页面之上）；关闭时框架会自动禁用其子树。
    // 冒烟用调试钩子：PXC_LAUNCHER_DEBUG_OV=1..4 时开窗口直接弹出自定义译文层浮窗并落到
    // 第 1..4 个分页（节点/词条/界面/注释），便于自动化截图核对，不影响正常使用。
    static int bootOvPage = -1;
    static bool bootOv = [] {
        wchar_t b[8] = {0};
        if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_OV", b, 8) == 0) return false;
        int v = _wtoi(b);
        if (v <= 0) return false;
        bootOvPage = v - 1;
        return true;
    }();
    if (bootOv) {
        bootOv = false;
        // 调试钩子附带初始查询串：PXC_LAUNCHER_DEBUG_OV_Q=<词>，用于自动化核对搜索命中。
        static std::string bootQ = [] {
            wchar_t b[256] = {0};
            if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_OV_Q", b, 256) == 0) return std::string();
            return pxc_util::toUtf8(b);
        }();
        guarded("打开自定义译文层", onOvOpen);
        if (bootOvPage > 0) ovSwitchTab(bootOvPage);
        if (!bootQ.empty()) {
            g_ovQuery.set(bootQ);
            ovRefreshView();
            g_ovScroll.set(0.0f);
            ++g_ovListGen;
        }
    }
    // 冒烟用调试钩子：PXC_LAUNCHER_DEBUG_BLD=1 直接落到汉化页并打开构建器浮窗。
    static bool bootBld = [] {
        wchar_t b[8] = {0};
        if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_BLD", b, 8) == 0) return false;
        return _wtoi(b) == 1;
    }();
    if (bootBld) {
        bootBld = false;
        g_page = Page::Locale;
        guarded("打开构建器", onBldOpen);
    }
    // 冒烟/回归用调试钩子：PXC_LAUNCHER_DEBUG_PICK=<工程路径> 直接弹出「缺版本协商」浮窗。
    // 正常路径要靠"版本对不上"才会出现，自动化截图造不出来，所以留一个直达入口。
    static bool bootPick = [] {
        wchar_t b[1024] = {0};
        if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_PICK", b, 1024) == 0) return false;
        std::filesystem::path p(b);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(p, ec)) {
            pxc_log::write("WARN", "Debug",
                           "PXC_LAUNCHER_DEBUG_PICK 指向的不是文件：" + pxc_util::toUtf8(b));
            return false;
        }
        openVersionPicker(p);
        return true;
    }();
    if (bootPick) bootPick = false;
    // 冒烟用：PXC_LAUNCHER_DEBUG_WB_Q=<词> 启动就给工作台预置搜索串。
    // 输入框里的字没法用伪造键消息塞进去（EUI 的输入框不吃 WM_CHAR），
    // 所以"搜索 -> 列表刷新"这条链路靠这个钩子验证。
    static bool bootWbQ = [] {
        wchar_t b[256] = {0};
        if (GetEnvironmentVariableW(L"PXC_LAUNCHER_DEBUG_WB_Q", b, 256) == 0) return false;
        g_wbQuery.set(pxc_util::toUtf8(b));
        wbRequery();
        return true;
    }();
    if (bootWbQ) bootWbQ = false;

    composeOverrideDialog(ui, screen);
    composeAccentPicker(ui, screen);   // 主题色取色器：最后一层，盖住其它浮窗
    // 缺版本协商浮窗：只有真的打开时才画，未打开第一句就 return。
    composeVersionPicker(ui, screen);
    // 工作台右键菜单：画在根级，否则会被页面 scrollView 裁掉。
    composeWbMenu(ui, screen);
    // 筛选浮窗同理（dropdown 的弹层会被 scrollView 的 clip 裁掉，所以自绘在根级）
    composeWbFilterPanel(ui, screen);

    // 汉化页的三个根级浮层（docs/08 §8.2.4 / §8.5 / §8.6.3）：`!` 提示 → 弹窗 → toast。
    // 只在汉化页画提示与弹窗；toast 是全应用共用的回执通道，别的页面也会用到它。
    if (g_page == Page::Locale) {
        composeLocaleTip(ui, screen);
        composeLocaleDialogs(ui, screen);
    } else {
        // 换页时指针离开那个小圆点不会再收到 hover(false)（节点已经不在了），
        // 不清掉的话下次进汉化页会先冒出一个上次留下的说明框。
        g_locExclaimHover = false;
        g_locTip.on = false;
        g_locTip.text.clear();
    }
    composeLocaleToast(ui, screen);

    // 静默启动（双击 .pxc 的默认行为）：先把窗口藏掉，等"工程交出去"有结果再退出。
    // 不能一上来就退 —— 复用已有进程是异步的，提前退会把那次打开一起带走。
    // 注意 hide 之后 glfwWaitEvents() 会一直挡住，不会再有下一帧（实测），所以"等结果"
    // 不能依赖后续帧 —— 复用已在 wbLaunchPath 里改成同步，做到这一帧就已经落定。
    // 常驻模式：启动完成后**不退出**，最小化留在任务栏；只有用户关窗才退。
    // 关掉常驻（assoc.stay_resident=false）时保留旧行为：交出去了就退。
    if (g_silentLaunch && !g_stayResident) {
        const bool done = g_wbInbox.empty() && !pxc_runner::busy() && !g_pick.open;
        // 兜底：真出了意外（交出去的动作卡住），25 秒后也要退 ——
        // 绝不能留一个隐藏进程攥着单实例互斥体。
        const bool timeout = (wbNowMs() - g_silentStartMs) > 25000.0;
        if (done || timeout) {
            g_silentLaunch = false;
            // 立刻退出就没人替我们做节流保存了（wbTick 要等 500ms 后的那一帧），
            // 这里必须同步落盘，否则「全部」页签看不到这次启动。
            wbSaveStoreNow();
            if (g_wbCache.dirty()) g_wbCache.save(wbCachePath());
            PostQuitMessage(0);
        }
    }
}

} // namespace app
