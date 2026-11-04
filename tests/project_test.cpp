// v3 工程文件工作台 P0 的 headless 回归：版本顾问（结构族/距离/排序）+ 启动入口的失败分支。
// 不弹 UI、不拉进程，只验证纯逻辑，任何一条挂了直接非零退出。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "project_index.h"
#include "project_launcher.h"
#include "store.h"
#include "version_advisor.h"
#include "version_registry.h"

namespace fs = std::filesystem;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const std::string& what, const std::string& got = {}) {
    if (ok) {
        ++g_pass;
        std::printf("[PASS] %s\n", what.c_str());
    } else {
        ++g_fail;
        std::printf("[FAIL] %s%s%s\n", what.c_str(), got.empty() ? "" : "  got=", got.c_str());
    }
}

static std::string fam(pxc_adv::Family f) { return pxc_adv::familyName(f); }
static std::string ws(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? (char)c : '?');
    return s;
}

int main() {
    using namespace pxc_adv;

    // ---------- 1. 版本号解析 ----------
    {
        VKey k = parseVersion("1.18.5.0");
        check(k.ok && k.major == 1 && k.minor == 18 && k.patch == 5 && k.build == 0,
              "parseVersion 1.18.5.0");
        k = parseVersion("1.21rc2");
        check(k.ok && k.major == 1 && k.minor == 21, "parseVersion 1.21rc2");
        check(!parseVersion("").ok, "parseVersion 空串 -> !ok");
        check(!parseVersion("未知").ok, "parseVersion 非数字 -> !ok");
    }

    // ---------- 2. 结构族（仅凭版本号，用于 exe） ----------
    {
        check(familyOfVersion("1.16.1.0") == Family::A60, "1.16.1.0 -> A@11.6", fam(familyOfVersion("1.16.1.0")));
        check(familyOfVersion("1.17.9") == Family::A60, "1.17.9 -> A@11.6", fam(familyOfVersion("1.17.9")));
        check(familyOfVersion("1.18.5") == Family::A71, "1.18.5 -> A@11.71", fam(familyOfVersion("1.18.5")));
        check(familyOfVersion("1.19.0.11") == Family::B80, "1.19.0.11 -> B@11.80", fam(familyOfVersion("1.19.0.11")));
        check(familyOfVersion("1.20.1") == Family::CX, "1.20.1 -> C/PXCX", fam(familyOfVersion("1.20.1")));
        check(familyOfVersion("1.22.0.5") == Family::CX, "1.22.0.5 -> C/PXCX", fam(familyOfVersion("1.22.0.5")));
        check(familyOfVersion("") == Family::Unknown, "空版本 -> 未知", fam(familyOfVersion("")));
    }

    // ---------- 3. 结构族（有文件格式时以格式为准） ----------
    {
        // 1.22 的 PXC 里仍可能存着 1.18 的老工程：文件本身是 JSON，就该按 A@11.71 算，
        // 而不是按"当前版本 1.22"算成 PXCX。
        check(familyOf(pxc::Format::PXCX, "1.18.5") == Family::CX, "格式 PXCX 压过版本号",
              fam(familyOf(pxc::Format::PXCX, "1.18.5")));
        check(familyOf(pxc::Format::ZLIB, "1.18") == Family::B80, "格式 ZLIB -> B@11.80",
              fam(familyOf(pxc::Format::ZLIB, "1.18")));
        check(familyOf(pxc::Format::JSON, "1.18.5") == Family::A71, "JSON + 1.18 -> A@11.71",
              fam(familyOf(pxc::Format::JSON, "1.18.5")));
        check(familyOf(pxc::Format::JSON, "1.17.9") == Family::A60, "JSON + 1.17 -> A@11.6",
              fam(familyOf(pxc::Format::JSON, "1.17.9")));
        check(familyOf(pxc::Format::UNKNOWN, "1.20.1") == Family::CX, "格式未知时退回版本推断",
              fam(familyOf(pxc::Format::UNKNOWN, "1.20.1")));
    }

    // ---------- 4. 版本距离 ----------
    {
        check(versionDistance("1.18.5", "1.18.5") == 0, "距离 同版本 = 0");
        check(versionDistance("1.18.5", "1.19.0") == 150, "距离 1.18.5<->1.19.0 = 150",
              std::to_string(versionDistance("1.18.5", "1.19.0")));
        check(versionDistance("1.18.5", "1.20.1") == 240, "距离 1.18.5<->1.20.1 = 240",
              std::to_string(versionDistance("1.18.5", "1.20.1")));
        check(versionDistance("1.18.5", "") == -1, "距离 缺一侧 = -1");
        check(versionDistance("", "") == -1, "距离 两边都空 = -1");
    }

    auto entry = [](const std::string& path, const std::string& ver) {
        pxc_store::ExeEntry e;
        e.exe_path = path;
        e.product_version = ver;
        e.source = "manual";
        return e;
    };

    // ---------- 5. 推荐：同结构族优先于版本号更近的跨族版本 ----------
    {
        std::vector<pxc_store::ExeEntry> reg = {
            entry("D:/pxc/1185/PixelComposer.exe", "1.18.5"),
            entry("D:/pxc/1190/PixelComposer.exe", "1.19.0.11"),
            entry("D:/pxc/1201/PixelComposer.exe", "1.20.1"),
        };
        pxc::DetectionResult det;
        det.format = pxc::Format::JSON;
        det.version = "1.18.5";
        det.confidence = pxc::Confidence::HIGH;

        Advice a = advise(reg, det);
        check(a.ordered.size() == 3, "推荐返回全部 3 个候选");
        check(a.ordered[0].version == "1.18.5", "首选 1.18.5（同结构、距离 0）", a.ordered[0].version);
        check(a.ordered[0].sameFamily && a.ordered[0].distance == 0, "首选标记：同族 + 距离 0");
        check(!a.ordered[1].sameFamily, "次选跨结构（1.19 换 zlib）", fam(a.ordered[1].family));
        check(!a.ordered[0].reason.empty(), "首选有理由文案", a.ordered[0].reason);
        check(a.headline.find("结构") != std::string::npos, "结论里点明结构", a.headline);
        check(a.detail.find("A@11.71") != std::string::npos, "细节里给出文件结构族", a.detail);
    }

    // ---------- 6. 推荐：结构族全不匹配时按距离排，并明确警告 ----------
    {
        std::vector<pxc_store::ExeEntry> reg = {
            entry("D:/pxc/1185/PixelComposer.exe", "1.18.5"),
            entry("D:/pxc/1190/PixelComposer.exe", "1.19.0.11"),
        };
        pxc::DetectionResult det;
        det.format = pxc::Format::PXCX;
        det.version = "1.21.0.2";
        det.confidence = pxc::Confidence::HIGH;

        Advice a = advise(reg, det);
        // 1.21.0.2 vs 1.19.0.11 = 200+0+9 = 209；vs 1.18.5 = 300+50+2 = 352
        check(a.ordered[0].version == "1.19.0.11", "跨族时按距离选 1.19.0.11", a.ordered[0].version);
        check(!a.ordered[0].sameFamily, "并标记为不同族");
        check(a.ordered[0].reason.find("跨结构") != std::string::npos, "理由里写明跨结构风险",
              a.ordered[0].reason);
        check(a.headline.find("没有结构相同") != std::string::npos, "结论里说明没有同族版本", a.headline);
    }

    // ---------- 7. 推荐：空版本库 ----------
    {
        std::vector<pxc_store::ExeEntry> reg;
        pxc::DetectionResult det;
        det.format = pxc::Format::JSON;
        det.version = "1.18.5";
        Advice a = advise(reg, det);
        check(a.ordered.empty(), "空版本库 -> 无候选");
        check(!a.headline.empty(), "空版本库仍给一句提示", a.headline);
    }

    // ---------- 8. 副本命名：重名递增 ----------
    fs::path tmpDir = fs::temp_directory_path() / L"pxc_project_test";
    std::error_code ec;

    // ---------- 12. 扫描：只收 .pxc、name/size/mtime 三样 ----------
    fs::path scanDir = tmpDir / L"扫描样本";
    fs::create_directories(scanDir / L"子目录", ec);
    {
        auto touch = [&](const fs::path& p, const std::string& body) {
            std::ofstream f(p, std::ios::binary);
            f << body;
        };
        touch(scanDir / L"a.pxc", "AAA");
        touch(scanDir / L"b.pxc", "BBBBB");
        touch(scanDir / L"note.txt", "不是工程");
        touch(scanDir / L"子目录" / L"c.PXC", "CC");
        auto flat = pxc_index::scanPxcDir(scanDir, false);
        check(flat.size() == 2, "非递归只扫本层 .pxc（大写扩展名也算）", std::to_string(flat.size()));
        auto deep = pxc_index::scanPxcDir(scanDir, true);
        check(deep.size() == 3, "递归把子目录里的 .pxc 也收进来", std::to_string(deep.size()));
        // 名字去掉扩展名、大小与 mtime 有值
        bool named = false, sized = false, timed = false;
        for (const auto& it : deep) {
            if (it.name == "b") { named = true; sized = (it.size == 5); timed = (it.mtime > 0); }
        }
        check(named, "name 去掉 .pxc 扩展名");
        check(sized, "size 取到真实字节数");
        check(timed, "mtime 换算成 unix 秒");
    }

    // ---------- 13. 排序：名字 / 时间 / 版本（版本要按数字段，不能按字典序） ----------
    {
        std::vector<pxc_index::Item> v(3);
        v[0].path = "C:/x/b.pxc"; v[0].name = "b"; v[0].mtime = 200; v[0].version = "1.18.5"; v[0].detected = true;
        v[1].path = "C:/x/a.pxc"; v[1].name = "a"; v[1].mtime = 300; v[1].version = "1.9.0";  v[1].detected = true;
        v[2].path = "C:/x/c.pxc"; v[2].name = "c"; v[2].mtime = 100; v[2].version = "";       v[2].detected = true;

        std::vector<int> idx = {0, 1, 2};
        pxc_index::sortIndices(idx, v, {pxc_index::SortKey::Name, true});
        check(idx[0] == 1 && idx[1] == 0 && idx[2] == 2, "按文件名升序");

        idx = {0, 1, 2};
        pxc_index::sortIndices(idx, v, {pxc_index::SortKey::Mtime, false});
        check(idx[0] == 1 && idx[1] == 0 && idx[2] == 2, "按修改时间降序（最近在前）");

        idx = {0, 1, 2};
        pxc_index::sortIndices(idx, v, {pxc_index::SortKey::Version, true});
        check(idx[0] == 1 && idx[1] == 0 && idx[2] == 2,
              "按版本升序：1.9 在 1.18 之前（字典序会反过来）");

        std::vector<pxc_index::Item> copy = v;
        pxc_index::sortItems(copy, {pxc_index::SortKey::Name, false});
        check(copy[0].name == "c" && copy[2].name == "a", "sortItems 按名字降序");

        auto s = pxc_index::defaultSort((int)pxc_index::Tab::Recent);
        check(s.key == pxc_index::SortKey::None, "近期文件默认不排序（保持 PXC 的数组序）");
        s = pxc_index::defaultSort((int)pxc_index::Tab::Autosave);
        check(s.key == pxc_index::SortKey::Mtime && !s.asc, "自动恢复默认 mtime 降序");
        auto n1 = pxc_index::nextSort({pxc_index::SortKey::None, true});
        check(n1.key == pxc_index::SortKey::Name && n1.asc, "表头循环：默认 -> 文件名 ↑");
        auto n2 = pxc_index::nextSort(n1);
        check(n2.key == pxc_index::SortKey::Name && !n2.asc, "表头循环：文件名 ↑ -> 文件名 ↓");
        auto n3 = pxc_index::nextSort(n2);
        check(n3.key == pxc_index::SortKey::Mtime && !n3.asc, "表头循环：文件名 ↓ -> 修改时间 ↓");
    }

    // ---------- 14. 搜索与漏斗 ----------
    {
        pxc_index::Item it;
        it.path = "D:/工程/刀光素材.pxc";
        it.name = "刀光素材";
        const std::string q1 = pxc_index::foldQuery("刀光");
        check(pxc_index::matchQuery(it, q1), "搜索命中文件名");
        const std::string q2 = pxc_index::foldQuery("工程 素材");
        check(pxc_index::matchQuery(it, q2), "空格分词：每个词都要出现");
        const std::string q3 = pxc_index::foldQuery("工程 不存在");
        check(!pxc_index::matchQuery(it, q3), "分词里有词不出现就不命中");
        const std::string q4 = pxc_index::foldQuery("Ｄ：/工程");   // 全角
        check(pxc_index::matchQuery(it, q4), "全角字符折叠后也能搜到");
        check(pxc_index::matchQuery(it, pxc_index::foldQuery("")), "空查询全命中");

        const std::vector<std::string> reg = {"1.18.5"};
        pxc_index::Item ok = it, miss = it, unk = it, undet = it;
        ok.detected = true;  ok.version = "1.18.5";
        miss.detected = true; miss.version = "1.20.1";
        unk.detected = true;  unk.version = "";
        undet.detected = false;
        check(pxc_index::passFunnel(ok, 0, reg), "漏斗：全部档全过");
        check(pxc_index::passFunnel(ok, 1, reg), "漏斗：版本已登记 -> 本机可启动");
        check(!pxc_index::passFunnel(miss, 1, reg), "漏斗：版本没登记 -> 不算可启动");
        check(pxc_index::passFunnel(miss, 2, reg), "漏斗：版本已知但本机没有 -> 版本缺失");
        check(pxc_index::passFunnel(unk, 3, reg), "漏斗：识别不出 -> 版本未知");
        check(!pxc_index::passFunnel(unk, 1, reg), "漏斗：未知版本不算可启动");
        check(pxc_index::passFunnel(undet, 1, reg), "漏斗：还没识别的行先别藏（否则列表会跳）");
    }

    // ---------- 15. projects.json 往返 ----------
    {
        pxc_index::Store s;
        pxc_index::noteLaunch(s, "D:/a.pxc", "1.18.5", "1.18.5", "JSON", "高");
        pxc_index::noteLaunch(s, "D:/b.pxc", "1.20.1", "1.20.1", "PXCX", "高");
        check(s.history.size() == 2, "noteLaunch 新增两条");
        check(s.history[0].path == "D:/b.pxc", "最后启动的那条排最前");
        pxc_index::noteLaunch(s, "D:/a.pxc", "1.18.5", "1.18.5", "JSON", "高");
        check(s.history.size() == 2 && s.history[0].path == "D:/a.pxc", "重复启动不新增条目，只提到最前");
        check(s.history[0].count == 2, "启动次数累加", std::to_string(s.history[0].count));

        pxc_index::addImported(s, "D:/工程目录", true);
        pxc_index::addImported(s, "D:/工程目录", true);
        check(s.imported.size() == 1 && s.imported[0].type == "folder", "重复导入同一路径只留一条");
        pxc_index::addImported(s, "D:/c.pxc", false);
        // addImported 插到表头（最新导入在前），所以刚加的这条在 index 0
        check(s.imported.size() == 2 && s.imported[0].type == "file" &&
              s.imported[1].type == "folder", "文件导入记 type=file");

        check(!pxc_index::isFavorite(s, "D:/a.pxc"), "默认没收藏");
        check(pxc_index::toggleFavorite(s, "D:/a.pxc"), "收藏切换 -> 已收藏");
        check(pxc_index::isFavorite(s, "D:/a.pxc"), "收藏后能查到");
        check(!pxc_index::toggleFavorite(s, "D:/a.pxc"), "再切一次 -> 取消收藏");
        check(!pxc_index::isFavorite(s, "D:/a.pxc"), "取消后查不到");

        pxc_index::hideRecent(s, "D:/gone.pxc");
        pxc_index::hideRecent(s, "D:/gone.pxc");
        check(s.recentHidden.size() == 1, "隐藏同一路径不会重复记");

        for (int i = 0; i < 520; ++i) pxc_index::noteLaunch(s, "D:/h" + std::to_string(i) + ".pxc", "", "", "", "");
        check(s.history.size() <= pxc_index::kMaxHistory, "history 超过 500 条自动裁剪",
              std::to_string(s.history.size()));

        fs::path storePath = tmpDir / L"projects.json";
        check(pxc_index::saveStore(storePath, s), "projects.json 落盘");
        bool ok = false;
        pxc_index::Store back = pxc_index::loadStore(storePath, &ok);
        check(ok, "projects.json 回读成功");
        check(back.history.size() == s.history.size(), "history 条数一致");
        check(back.imported.size() == 2 && back.imported[0].type == "file",
              "imported 与 type 还原（顺序也不变）");
        check(back.assoc.silentLaunch && back.assoc.reuseInstance, "assoc 默认值还原");
        check(!back.recentHidden.empty() && back.recentHidden[0] == "D:/gone.pxc", "recent_hidden 还原");

        back.assoc.registered = true;
        back.assoc.previousProgid = "Other.pxc";
        check(pxc_index::saveStore(storePath, back), "改后再次落盘");
        pxc_index::Store back2 = pxc_index::loadStore(storePath, &ok);
        check(back2.assoc.registered && back2.assoc.previousProgid == "Other.pxc", "assoc 三态字段往返");
    }

    // ---------- 16. 识别缓存：命中的前提是 size+mtime 没变 ----------
    {
        fs::path cachePath = tmpDir / L"cache" / L"detect_cache.json";
        pxc_index::DetectCache c;
        pxc_index::CacheEntry e;
        e.size = 42106; e.mtime = 1770000000; e.version = "1.19.0.11"; e.format = "zlib"; e.conf = 0;
        c.set("D:/a.pxc", e);
        pxc_index::CacheEntry got;
        check(c.get("D:/a.pxc", 42106, 1770000000, got), "同 size+mtime 命中");
        check(got.version == "1.19.0.11" && got.format == "zlib", "缓存里的版本与格式还原");
        check(!c.get("D:/a.pxc", 42107, 1770000000, got), "size 变了就不命中（文件被改过）");
        check(!c.get("D:/a.pxc", 42106, 1770000001, got), "mtime 变了就不命中");
        check(!c.get("D:/nope.pxc", 1, 1, got), "没缓存过的路径不命中");
        check(c.save(cachePath), "缓存落盘");
        pxc_index::DetectCache c2;
        c2.load(cachePath);
        check(c2.size() == 1 && c2.get("D:/a.pxc", 42106, 1770000000, got), "缓存重启后仍在");
    }

    // ---------- 17. 近期文件：失效路径不进列表，但要报出来 ----------
    {
        fs::path userDir = tmpDir / L"用户目录";
        fs::create_directories(userDir, ec);
        fs::path alive = scanDir / L"a.pxc";
        const std::string alivePath = alive.u8string();
        std::string json = "{\"Recents\":[\"" ;
        // JSON 里反斜杠要转义；这里用 u8string 直接拼（Windows 下是反斜杠）
        std::string esc;
        for (char ch : alivePath) { if (ch == '\\') esc += "\\\\"; else esc += ch; }
        json += esc + "\",\"D:/这个路径不存在/x.pxc\"]}";
        std::ofstream f(userDir / L"recent.json", std::ios::binary);
        f << json;
        f.close();
        std::vector<std::string> missing;
        auto items = pxc_index::readRecent(userDir, &missing);
        check(items.size() == 1, "近期列表只保留有效路径", std::to_string(items.size()));
        check(missing.size() == 1 && missing[0] == "D:/这个路径不存在/x.pxc", "失效路径单独报出来（可记入 recent_hidden）");
    }

    // ---------- 13b. statPath 的时间戳（踩过的坑） ----------
    // statPath 原来用纳秒换算 FILETIME 纪元（1601），1601 到现在的纳秒数约 1.3e19
    // 已经越过 int64 上限，静默溢出成负数 —— 近期/全部/导入/收藏四页的「修改时间」
    // 全变成「—」，按时间排序也跟着错。§12 走的是 scanPxcDir(FindFirstFileW)，
    // 覆盖不到这条路径，所以单独钉一遍。
    {
        fs::path f = tmpDir / L"stat_probe.pxc";
        { std::ofstream o(f, std::ios::binary); o << "abc"; }
        pxc_index::Item it;
        const bool ok = pxc_index::statPath(f, it);
        const std::int64_t now = pxc_index::nowUnix();
        check(ok && it.mtime > now - 600 && it.mtime < now + 60,
              "statPath 的 mtime 落在当前时间附近（纪元换算不溢出）", std::to_string(it.mtime));
        check(it.size == 3, "statPath 取到真实大小");
        check(it.name == "stat_probe", "statPath 的 name 去掉扩展名");
    }

    fs::remove_all(tmpDir, ec);
    fs::create_directories(tmpDir, ec);
    fs::path src = tmpDir / L"Demo.pxc";
    {
        std::ofstream w(src, std::ios::binary);
        w << "{\"version\": 117100}";
    }
    {
        fs::path c1 = pxc_project::makeCopyPath(src, "1.18.5");
        check(c1.filename().wstring() == L"Demo (副本 1.18.5).pxc", "副本名 Demo (副本 1.18.5).pxc",
              ws(c1.filename().wstring()));
        check(!fs::exists(c1), "尚未创建时副本路径不冲突");

        fs::path c3 = pxc_project::makeCopyPath(src, "");
        check(c3.filename().wstring() == L"Demo (副本).pxc", "无版本标签 -> Demo (副本).pxc",
              ws(c3.filename().wstring()));
    }

    // ---------- 8b. 建副本：真的复制、内容一致、重名自动顺延 ----------
    {
        const auto srcSize = fs::file_size(src);
        std::string err;
        fs::path a1 = pxc_project::createCopy(src, "1.18.5", err);
        check(!a1.empty() && fs::exists(a1), "createCopy 真的落盘", err);
        check(a1.filename().wstring() == L"Demo (副本 1.18.5).pxc", "第一份副本命名正确",
              ws(a1.filename().wstring()));
        check(fs::file_size(a1) == srcSize, "副本与原文件同大小",
              std::to_string((long long)fs::file_size(a1)) + " vs " + std::to_string((long long)srcSize));

        fs::path a2 = pxc_project::createCopy(src, "1.18.5", err);
        check(a2.filename().wstring() == L"Demo (副本 1.18.5) (2).pxc", "第二次自动加 (2)",
              ws(a2.filename().wstring()));
        check(fs::file_size(src) == srcSize, "原文件没有被副本动作改动");
    }

    // ---------- 8c. 取消：本次刚建的副本必须被删掉（2026-11-04 改判，原方案是"保留不删"） ----------
    // 判据不能只看 result —— 真要命的是"用户取消了，副本却还躺在工程目录里"，
    // 每点一次取消就多一个没人用过的「(副本)」。所以这里查的是文件系统。
    {
        // 空文件冒充 exe：取消发生在拉起之前，正常路径根本走不到 CreateProcess；
        // 万一取消没生效，拿一个非 PE 文件去 CreateProcess 也只会失败，不会真开出一个 PXC。
        const fs::path fakeExe = tmpDir / L"fake_pxc.exe";
        {
            std::ofstream f(fakeExe, std::ios::binary);
            f << "not a pe";
        }

        std::vector<pxc_store::ExeEntry> reg = {entry(fakeExe.u8string(), "1.18.5")};
        pxc_store::BackupSettings bk;

        // 取消时机＝"用户在'正在创建副本'那一下点了取消"：定完 exe 之后放行（第 1 次问是 false），
        // 建完副本之后再问（第 2 次）才是 true。这样才真的走到"建了又删"。
        int asked = 0;
        pxc_project::StartRequest req;
        req.pxcPath = src;
        req.exePath = fakeExe.u8string();
        req.targetVersion = "1.18.5";
        req.mode = pxc_project::StartMode::Copy;
        req.canceled = [&asked] { return ++asked >= 2; };

        auto out = pxc_project::startProject(reg, bk, {}, req);
        check(out.result == pxc_project::StartResult::Canceled, "拉起前取消 -> Canceled", out.message);
        check(asked >= 2, "取消检查点覆盖了建副本之后", std::to_string(asked));
        check(!out.copyPath.empty(), "确实建过副本（否则这条测不到删除）");
        check(out.copyRemoved, "本次刚建的副本已删除");
        check(!fs::exists(out.copyPath), "副本文件真的不在了", ws(out.copyPath.filename().wstring()));

        // 对照：Direct 模式取消时压根没建副本，不能报"已删除"（提示文案会误导）。
        int asked2 = 0;
        pxc_project::StartRequest r2 = req;
        r2.mode = pxc_project::StartMode::Direct;
        r2.canceled = [&asked2] { return ++asked2 >= 1; };
        auto o2 = pxc_project::startProject(reg, bk, {}, r2);
        check(o2.result == pxc_project::StartResult::Canceled && o2.copyPath.empty() && !o2.copyRemoved,
              "Direct 取消：没有副本可删", o2.message);
    }

    // ---------- 9. 启动入口：版本缺失 -> NoVersion（不弹框、不拉进程） ----------
    {
        std::vector<pxc_store::ExeEntry> reg;
        pxc_store::BackupSettings bk;
        pxc_project::StartRequest req;
        req.pxcPath = src;
        req.targetVersion = "1.18.5";
        auto out = pxc_project::startProject(reg, bk, {}, req);
        check(out.result == pxc_project::StartResult::NoVersion, "空版本库 -> NoVersion");
        check(out.message.find("1.18.5") != std::string::npos, "提示里带上缺的版本号", out.message);
    }

    // ---------- 10. 启动入口：登记的 exe 不在了 -> ExeMissing ----------
    {
        std::vector<pxc_store::ExeEntry> reg = {entry("D:/不存在/的/PixelComposer.exe", "1.18.5")};
        pxc_store::BackupSettings bk;
        pxc_project::StartRequest req;
        req.pxcPath = src;
        req.targetVersion = "1.18.5";
        auto out = pxc_project::startProject(reg, bk, {}, req);
        check(out.result == pxc_project::StartResult::ExeMissing, "exe 不存在 -> ExeMissing");
        check(out.message.find("不在") != std::string::npos, "提示说明 exe 找不到了", out.message);
    }

    // ---------- 11. 启动入口：工程文件不存在 -> 拒绝并把错误说清 ----------
    {
        std::vector<pxc_store::ExeEntry> reg;
        pxc_store::BackupSettings bk;
        pxc_project::StartRequest req;
        req.pxcPath = tmpDir / L"根本没有这个.pxc";
        auto out = pxc_project::startProject(reg, bk, {}, req);
        check(out.result == pxc_project::StartResult::LaunchFailed, "文件不存在 -> LaunchFailed");
        check(out.message.find("不存在") != std::string::npos, "提示说明文件不存在", out.message);
    }

    // ---------- 14. 版本号尾部 .0：文件侧省略、版本库四段 —— 必须当成同一个版本 ----------
    // 线上症状：PXC 1.20.1.0 写出的工程被识别成 "1.20.1"，版本库里登记的是 "1.20.1.0"，
    // 字符串全等比 -> NoVersion -> 明明装了对应版本，却弹"本机没有这个版本"的版本库列表。
    {
        check(pxc_reg::normalizeVersion("1.18.5") == "1.18.5.0", "normalizeVersion 1.18.5 -> 1.18.5.0",
              pxc_reg::normalizeVersion("1.18.5"));
        check(pxc_reg::normalizeVersion("1.20.1.0") == "1.20.1.0", "normalizeVersion 四段原样",
              pxc_reg::normalizeVersion("1.20.1.0"));
        check(pxc_reg::normalizeVersion("1.20") == "1.20.0.0", "normalizeVersion 两段补齐",
              pxc_reg::normalizeVersion("1.20"));
        check(pxc_reg::normalizeVersion("1.21rc2") == "1.21rc2", "带后缀不猜，原样返回",
              pxc_reg::normalizeVersion("1.21rc2"));
        check(pxc_reg::normalizeVersion("") == "", "空串原样返回");

        check(pxc_reg::sameVersion("1.20.1", "1.20.1.0"), "sameVersion 1.20.1 == 1.20.1.0");
        check(pxc_reg::sameVersion("1.18.5", "1.18.5.0"), "sameVersion 1.18.5 == 1.18.5.0");
        check(!pxc_reg::sameVersion("1.20.1", "1.20.2"), "sameVersion patch 不同 -> 不相等");
        check(!pxc_reg::sameVersion("1.20.1", "1.21rc2"), "sameVersion 带后缀不按数字猜");
        check(!pxc_reg::sameVersion("", "1.20.1.0"), "空版本不等于任何版本");

        std::vector<pxc_store::ExeEntry> reg = {entry("D:/pxc/PixelComposer.exe", "1.20.1.0")};
        auto m = pxc_reg::findMatching(reg, "1.20.1");
        check(m.size() == 1, "findMatching 用省略 .0 的版本也能命中", std::to_string(m.size()));

        // 走得到 exe 那一步（exe 不在 -> ExeMissing），说明没被判成 NoVersion
        pxc_store::BackupSettings bk;
        pxc_project::StartRequest req;
        req.pxcPath = src;
        req.targetVersion = "1.20.1";
        auto out = pxc_project::startProject(reg, bk, {}, req);
        check(out.result == pxc_project::StartResult::ExeMissing,
              "文件 1.20.1 + 登记 1.20.1.0 -> 匹配成功（只在 exe 上失败）",
              std::to_string(static_cast<int>(out.result)));

        // 识别结果入库时就补成四段：显示、筛选、匹配共用一份值
        pxc::DetectionResult det;
        det.format = pxc::Format::JSON;
        det.version = "1.18.5";
        det.confidence = pxc::Confidence::HIGH;
        pxc_index::Item it;
        pxc_index::applyDetect(it, det);
        check(it.version == "1.18.5.0", "applyDetect 把识别值补成四段", it.version);

        std::vector<std::string> regv = {"1.18.5"};
        check(pxc_index::passFunnel(it, 1, regv), "passFunnel 与 findMatching 同口径：1.18.5.0 vs 1.18.5");
    }

    // ---------- 18. 四态 + 筛选（v4：识别不出红/识别中灰分开、筛选与排序正交、文件夹按路径去重） ----------
    {
        const std::vector<std::string> reg = {"1.18.5.0"};
        auto mk = [](const std::string& name, bool detected, const std::string& ver,
                     std::int64_t mt, bool folder = false) {
            pxc_index::Item it;
            it.name = name;
            it.path = "D:/工程/" + name;
            it.detected = detected;
            it.version = ver;
            it.mtime = mt;
            it.folder = folder;
            return it;
        };
        const auto itPend = mk("a", false, "", 100);            // 识别中
        const auto itOk   = mk("b", true, "1.18.5.0", 200);     // 可启动
        const auto itMiss = mk("c", true, "1.20.1.0", 300);     // 版本缺失
        const auto itUnk  = mk("d", true, "", 400);             // 识别不出

        // 四态判定：识别中(过程) 必须和 识别不出(结论) 分开
        check(pxc_index::stateOf(itPend, reg) == pxc_index::VerState::Pending, "未识别 -> Pending(识别中)");
        check(pxc_index::stateOf(itOk, reg) == pxc_index::VerState::Ok, "登记版本 -> Ok(可启动)");
        check(pxc_index::stateOf(itMiss, reg) == pxc_index::VerState::Missing, "已知但本机无 -> Missing(版本缺失)");
        check(pxc_index::stateOf(itUnk, reg) == pxc_index::VerState::Unreadable,
              "读到空版本 -> Unreadable(识别不出，红)");

        // chip 显示序 -> 位 的映射必须落在枚举上（踩过：点“识别不出”筛出 0 条）
        check(pxc_index::stateChipAt(0) == pxc_index::VerState::Ok, "chip0 = 可启动");
        check(pxc_index::stateChipAt(1) == pxc_index::VerState::Missing, "chip1 = 版本缺失");
        check(pxc_index::stateChipAt(2) == pxc_index::VerState::Unreadable, "chip2 = 识别不出");
        check(pxc_index::stateChipAt(3) == pxc_index::VerState::Pending, "chip3 = 识别中");
        check(pxc_index::stateChipBit(2) == (1 << (int)pxc_index::VerState::Unreadable),
              "chip2 的位 = 1<<Unreadable(8)，不是 1<<2",
              std::to_string(pxc_index::stateChipBit(2)));
        check(std::string(pxc_index::stateChipName(2)) == "识别不出", "chip2 文案 = 识别不出");

        // 状态筛选：只勾「识别不出」时，只有真坏的那条过
        pxc_index::Filter f;
        f.states = pxc_index::stateChipBit(2);
        check(pxc_index::passFilter(itUnk, f, reg, false), "筛选识别不出：坏文件通过");
        check(!pxc_index::passFilter(itOk, f, reg, false), "筛选识别不出：可启动的不通过");
        check(!pxc_index::passFilter(itMiss, f, reg, false), "筛选识别不出：版本缺失的不通过");
        check(!pxc_index::passFilter(itPend, f, reg, false), "筛选识别不出：识别中的不通过（未识别不算坏）");

        // 分区内部 OR：可启动 或 识别不出 都过
        pxc_index::Filter f2;
        f2.states = pxc_index::stateChipBit(0) | pxc_index::stateChipBit(2);
        check(pxc_index::passFilter(itOk, f2, reg, false) &&
                  pxc_index::passFilter(itUnk, f2, reg, false),
              "状态多选是并集（可启动 或 识别不出）");
        check(!pxc_index::passFilter(itMiss, f2, reg, false), "并集里没选的不过");

        // 文件夹没有版本属性，版本/状态筛选一旦生效就排掉它
        const auto itFolder = mk("dir", false, "", 500, true);
        pxc_index::Filter f3;
        f3.states = pxc_index::stateChipBit(3);
        check(!pxc_index::passFilter(itFolder, f3, reg, false), "状态筛选生效时文件夹被排掉");
        pxc_index::Filter f4;
        f4.versions = {"1.18.5.0"};
        check(!pxc_index::passFilter(itFolder, f4, reg, false), "版本筛选生效时文件夹被排掉");

        // 时间筛选：闭区间，只决定“进不进来”
        pxc_index::Filter f5;
        f5.timeRange = 5; f5.from = 150; f5.to = 350;
        check(!pxc_index::passFilter(itPend, f5, reg, false), "时间区间：100 不在 [150,350]");
        check(pxc_index::passFilter(itOk, f5, reg, false), "时间区间：200 在区间内");
        check(pxc_index::passFilter(itMiss, f5, reg, false), "时间区间：300 在区间内");
        check(!pxc_index::passFilter(itUnk, f5, reg, false), "时间区间：400 不在区间内");

        // 筛选与排序正交：筛选之后仍按同一 sortSpec 的次序输出（不因筛选重排）
        std::vector<pxc_index::Item> batch = {itOk, itMiss, itUnk, itPend};   // mtime 200/300/400/100
        std::vector<int> idx = {0, 1, 2, 3};
        pxc_index::sortIndices(idx, batch, {pxc_index::SortKey::Mtime, false});
        // 降序：400(d)/300(c)/200(b)/100(a) -> {2,1,0,3}
        check(idx[0] == 2 && idx[3] == 3, "排序与筛选互不干扰：mtime 降序仍是 400->100");
        std::vector<int> kept;
        for (int i = 0; i < (int)batch.size(); ++i)
            if (pxc_index::passFilter(batch[(size_t)idx[(size_t)i]], f5, reg, false)) kept.push_back(idx[(size_t)i]);
        check(kept.size() == 2 && kept[0] == 1 && kept[1] == 0,
              "筛掉两端后仍保持降序：300 在 200 之前（筛选不重排）");

        // 筛选按钮上的 ·N：activeGroups
        pxc_index::Filter f6;
        check(!f6.active() && f6.activeGroups() == 0, "空筛选不激活");
        f6.states = 2;
        check(f6.active() && f6.activeGroups() == 1, "只有状态 -> 1 组");
        f6.timeRange = 2;
        check(f6.activeGroups() == 2, "状态+时间 -> 2 组");
        f6.versions = {"1.18.5.0"};
        f6.onlyFav = true;
        check(f6.activeGroups() == 4, "四维都选 -> 4 组");
        f6.reset();
        check(!f6.active() && f6.activeGroups() == 0, "reset 清空四维");

        // 文件夹按路径去重（不做父子合并：同一路径只留一条，不同路径各留）
        pxc_index::Store st;
        pxc_index::addImported(st, "D:/同一目录", true);
        pxc_index::addImported(st, "D:/同一目录", true);
        check(st.imported.size() == 1, "文件夹按路径去重：同一路径只留一条");
        pxc_index::addImported(st, "D:/同一目录/子", true);
        check(st.imported.size() == 2, "不同路径（含子目录）各自保留 —— 不按父子关系合并");

        // 便携布局：exe 落在启动器父目录 -> 存成 `../PixelComposer.exe`。
        // 读回若只做字符串拼接，内存里就是 `...\launcher\..\PixelComposer.exe`，
        // 与扫描到的绝对路径不等 -> 每次启动重复发现一条（真机表现：卡片越重启越多）。
        {
            fs::path base = tmpDir / L"便携" / L"launcher";
            fs::create_directories(base, ec);
            fs::path cfgPath = base / L"launcher.json";
            const std::string raw =
                R"({"version":6,"registry":[)"
                R"({"exe_path":"../PixelComposer.exe","product_version":"1.18.5.0","display_name":"PixelComposer.exe","source":"auto","registered_at":"2026-11-01","channel":"","acked_version":""},)"
                R"({"exe_path":"../PixelComposer.exe","product_version":"","display_name":"PixelComposer.exe","source":"manual","registered_at":"2026-11-02","channel":"","acked_version":""}]})";
            std::ofstream(cfgPath, std::ios::binary) << raw;

            int dup = 0;
            auto [cfg2, ok2] = pxc_store::load(cfgPath, base, &dup);
            check(ok2, "相对路径配置可读");
            check(cfg2.registry.size() == 1, "同一 exe 的 ../ 写法读回合并为一条");
            check(dup == 1, "重复条数回填给上层（用来触发一次回写）");
            check(!cfg2.registry.empty() && cfg2.registry[0].exe_path.find("..") == std::string::npos,
                  "读回路径已词法归一（不含 ..）");
            const std::string want = (base.parent_path() / L"PixelComposer.exe").u8string();
            check(!cfg2.registry.empty() && pxc_reg::samePath(cfg2.registry[0].exe_path, want),
                  "归一后的路径与扫描到的绝对路径判为同一条");
            check(!cfg2.registry.empty() && cfg2.registry[0].product_version == "1.18.5.0",
                  "合并时保留已有的版本号");
        }

        // 写盘形态：**只有落在启动器目录（含子目录）里的 PXC 才写相对路径**，其它一律绝对。
        // 老实现还认 baseDir.parent_path()：启动器装在盘符顶层下一层时（D:\PXC Launcher），
        // parent 就是 D:\，于是整盘 PXC 都被写成 ../Soft/itch/apps/... ——
        // 这类条目离启动器很远、根本不在便携扫描范围里，但落盘形态与内存不同构，
        // 读回判不出是同一条 -> 每次启动整批重登一遍（用户看到的"每次重启翻倍"）。
        {
            fs::path base = tmpDir / L"顶层" / L"PXC Launcher";
            fs::create_directories(base, ec);
            pxc_store::AppConfig c;
            pxc_store::ExeEntry far = entry("", "1.20.0.2");
            far.exe_path = (base.parent_path() / L"far_away" / L"PixelComposer.exe").u8string();
            pxc_store::ExeEntry near = entry("", "1.18.5.0");
            near.exe_path = (base / L"PixelComposer.exe").u8string();
            c.registry = {far, near};

            const fs::path out = base / L"out.json";
            check(pxc_store::save(out, c, base), "写盘成功");
            std::ifstream in(out, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            check(text.find("\"../") == std::string::npos,
                  "启动器父目录下的 PXC 不再写成 ../ 相对路径（盘符顶层布局的根因）");
            check(text.find("far_away") != std::string::npos, "父目录外的 PXC 以绝对路径落盘");
            check(text.find("\"PixelComposer.exe\"") != std::string::npos,
                  "启动器目录内的 PXC 仍写相对路径（便携打包不退化）");
        }

        // 删除：一次清掉所有同路径条目（磁盘上可能躺着好几条一模一样的），写法差异不算不同
        {
            std::vector<pxc_store::ExeEntry> reg = {
                entry("D:/pxc/A/PixelComposer.exe", "1.21.0.2"),
                entry("D:\\pxc\\A\\PixelComposer.exe", "1.21.0.2"),
                entry("D:/pxc/B/PixelComposer.exe", "1.20.0.2"),
            };
            const int n = pxc_reg::removeExe(reg, "d:/PXC/a/PixelComposer.exe");
            check(n == 2, "删除一次清掉所有同路径重复（含斜杠/大小写写法差异）");
            check(reg.size() == 1 && reg[0].exe_path == "D:/pxc/B/PixelComposer.exe",
                  "其它路径的条目不受影响");
        }

        // 便携扫描：PXC 与启动器同目录（典型便携包）时，`collect(parent)` 会把启动器目录
        // 再收一次 —— 目录集合没去重的话，同一次扫描会给同一个 exe 加两条；
        // 而只要路径写法一变（相对路径没归一），findEntry 挡不住，就变成"每重启一次多一倍"。
        {
            fs::path dir = tmpDir / L"便携2";
            fs::create_directories(dir, ec);
            std::ofstream(dir / L"PixelComposer.exe", std::ios::binary) << "x";

            std::vector<pxc_store::ExeEntry> reg;
            const int n = pxc_reg::discoverPortable(reg, dir);
            check(n == 1 && reg.size() == 1, "便携扫描：同目录只登记一条（目录集合已去重）");
            const int n2 = pxc_reg::discoverPortable(reg, dir);
            check(n2 == 0 && reg.size() == 1, "便携扫描再跑一次不新增");
        }

        // 扫描范围只保留两种布局：启动器目录（含子目录）和父目录**本身**。
        // 不遍历父目录的子目录 —— 启动器装在盘符顶层下一层时 parent 就是盘符根，
        // 遍历它的子目录等于把整盘一级目录（D:\我的文档、D:\Games…）全扫一遍。
        {
            fs::path root = tmpDir / L"范围";
            fs::path launcher = root / L"launcher";
            fs::create_directories(launcher, ec);
            // 父目录本身 = PXC 安装目录、启动器在它的子文件夹里 → 应发现
            std::ofstream(root / L"PixelComposer.exe", std::ios::binary) << "x";
            // 父目录的子目录 = 与启动器平级的其它安装目录 → 不该发现
            fs::create_directories(root / L"远处", ec);
            std::ofstream(root / L"远处" / L"PixelComposer.exe", std::ios::binary) << "x";

            std::vector<pxc_store::ExeEntry> reg;
            const int n = pxc_reg::discoverPortable(reg, launcher);
            check(n == 1 && reg.size() == 1, "只发现父目录本身（启动器在 PXC 子文件夹里）");
            check(!reg.empty() && reg[0].exe_path.find("远处") == std::string::npos,
                  "不遍历父目录的子目录（盘符顶层布局不会扫整盘）");
        }
    }

    fs::remove_all(tmpDir, ec);

    std::printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
