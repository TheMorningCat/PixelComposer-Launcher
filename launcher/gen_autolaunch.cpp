// 启动前自动生成汉化 —— 实现（见 gen_autolaunch.h 的三条纪律）。
#include "gen_autolaunch.h"

#include "io_util.h"
#include "log.h"
#include "util.h"

namespace pxc_autogen {

namespace fs = std::filesystem;
using pxc_store::JsonValue;

namespace {

const char* kStampName = ".pxc_gen_stamp.json";

fs::path stampPath(const fs::path& outRoot, const std::string& langDir) {
    return pxc_util::joinUtf8(outRoot, langDir) / kStampName;
}

std::string getStr(const JsonValue& v, const char* k) {
    const JsonValue* f = v.find(k);
    return (f && f->isStr()) ? f->str : std::string();
}

} // namespace

void writeStamp(const fs::path& outRoot, const std::string& langDir,
                const std::string& skeletonVersion, const std::string& dbVersion) {
    if (langDir.empty()) return;
    JsonValue v;
    v.type = JsonValue::Obj;
    auto put = [&](const char* k, const std::string& s) {
        JsonValue x;
        x.type = JsonValue::Str;
        x.str = s;
        v.obj.emplace_back(k, x);
    };
    put("skeleton_version", skeletonVersion);
    put("db_version", dbVersion);
    pxc_io::writeJson(stampPath(outRoot, langDir), v);
}

Result beforeLaunch(const Request& req, const fs::path& exe, const std::string& version) {
    Result r;
    if (exe.empty() || version.empty()) return r;
    if (!req.db || req.langDir.empty() || req.userDir.empty()) return r;
    r.ran = true;

    const fs::path outRoot = req.userDir / "Locale";

    // 语言不是本启动器管的 → 整个跳过。否则每次启动都在重写一份没人读的包。
    if (!req.installedLang.empty() && req.installedLang != req.langDir) {
        r.message = "当前界面语言是 " + req.installedLang + "（不是本启动器生成的 " + req.langDir +
                    "），跳过自动生成";
        pxc_log::write("INFO", "Gen", r.message);
        return r;
    }

    r.base = pxc_base::resolveForExe(exe, version, req.registry, req.userDir, req.cacheRoot);
    if (!r.base.hasNodes && !r.base.hasWords) {
        r.message = "目标 " + version + " 取不到英文骨架：" + r.base.note;
        pxc_log::write("WARN", "Gen", r.message);
        return r;
    }

    // 内容没变就跳过：同一个版本每次启动前都要拿一次骨架，一个字没变就不该重解/重写
    {
        auto [v, ok] = pxc_io::readJson(stampPath(outRoot, req.langDir));
        if (ok && v.isObj()) {
            const std::string skv = getStr(v, "skeleton_version");
            const std::string dbv = getStr(v, "db_version");
            const bool fileOk = fs::exists(pxc_util::joinUtf8(outRoot, req.langDir) / "nodes.json");
            if (fileOk && skv == r.base.version && dbv == req.db->version) {
                r.ok = true;
                r.skipped = true;
                r.message = "骨架未变（" + skv + "），跳过生成";
                pxc_log::write("INFO", "Gen", r.message);
                return r;
            }
        }
    }

    pxc_gen::Stats st = pxc_gen::generate(r.base, *req.db, req.expr, req.font, req.opt, outRoot,
                                          req.langDir);
    r.stats = st;
    r.ok = st.ok;
    if (st.ok) {
        writeStamp(outRoot, req.langDir, r.base.version, req.db->version);
        r.message = "启动前生成：" + req.langDir + " | 骨架 " + r.base.version + "（" +
                    r.base.origin + "）| 命中 " + std::to_string(st.translated) + "/" +
                    std::to_string(st.fields) + (st.normHits ? "，归一化 " + std::to_string(st.normHits)
                                                            : "");
    } else {
        r.message = "启动前生成失败：" + st.error;
    }
    pxc_log::write(st.ok ? "INFO" : "ERROR", "Gen", r.message);
    return r;
}

} // namespace pxc_autogen
