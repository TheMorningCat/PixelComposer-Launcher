// 工程启动统一入口实现。
#include "project_launcher.h"

#include <system_error>

#include "backup_manager.h"
#include "launcher.h"
#include "log.h"
#include "pxc_detect.h"
#include "util.h"
#include "version_registry.h"

namespace fs = std::filesystem;

namespace pxc_project {

namespace {

std::string confStr(pxc::Confidence c) {
    switch (c) {
        case pxc::Confidence::HIGH:   return "高";
        case pxc::Confidence::MEDIUM: return "中";
        case pxc::Confidence::LOW:    return "低";
        default:                      return "未知";
    }
}

std::string u8(const fs::path& p) { return p.u8string(); }

// 取消收尾。**只删本次刚建的副本** —— 那是本函数刚刚 copy 出来的文件，用户取消启动后
// 没有任何人还会用到它，留在工程目录里只是垃圾（用户 2026-11-04 改判，原方案是保留）。
// 备份**不删**：它是原文件的安全副本，留着无害，删了反而少一层保险；
// 而且备份只发生在 Direct 模式，那种情况下本来就没有副本可删。
void applyCancel(StartOutcome& out, bool builtCopy) {
    if (builtCopy && !out.copyPath.empty()) {
        std::error_code rec;
        const bool gone = fs::remove(pxc_util::toExtended(out.copyPath), rec);
        out.copyRemoved = gone;
        if (gone) {
            pxc_log::write("INFO", "Project", "取消启动，已删除刚建的副本: " + u8(out.copyPath));
        } else {
            pxc_log::write("WARN", "Project",
                           "取消启动，副本没删掉（多半被别的程序占用）: " + u8(out.copyPath) +
                               " | " + rec.message());
        }
    }
    out.result = StartResult::Canceled;
    out.message = builtCopy ? (out.copyRemoved ? "已取消（刚建的副本已删除）"
                                              : "已取消（副本没删掉，可手动删除）")
                            : "已取消（未拉起 PXC）";
}

} // namespace

fs::path makeCopyPath(const fs::path& src, const std::string& tag) {
    fs::path dir = src.parent_path();
    if (dir.empty()) dir = fs::current_path();
    const std::wstring stem = src.stem().wstring();
    const std::wstring ext = src.extension().wstring();  // ".pxc"

    std::wstring base = stem + L" (副本";
    if (!tag.empty()) base += L" " + pxc_util::pathFromUtf8(tag).wstring();
    base += L")";

    fs::path p = dir / fs::path(base + ext);
    std::error_code ec;
    for (int i = 2; i <= 999 && fs::exists(pxc_util::toExtended(p), ec); ++i) {
        p = dir / fs::path(base + L" (" + std::to_wstring(i) + L")" + ext);
        ec.clear();
    }
    return p;
}

fs::path createCopy(const fs::path& src, const std::string& tag, std::string& err) {
    err.clear();
    fs::path dst = makeCopyPath(src, tag);
    std::error_code cec;
    fs::copy_file(pxc_util::toExtended(src), pxc_util::toExtended(dst),
                  fs::copy_options::none, cec);
    if (cec) {
        err = cec.message();
        return {};
    }
    return dst;
}

StartOutcome startProject(const std::vector<pxc_store::ExeEntry>& registry,
                          const pxc_store::BackupSettings& backup,
                          const fs::path& backupDir,
                          const StartRequest& req) {
    StartOutcome out;
    std::error_code ec;

    if (req.pxcPath.empty() || !fs::is_regular_file(req.pxcPath, ec)) {
        out.result = StartResult::LaunchFailed;
        out.message = "工程文件不存在：" + u8(req.pxcPath);
        pxc_log::write("ERROR", "Project", out.message);
        return out;
    }

    out.detection = pxc::detectPxc(req.pxcPath);
    // 识别值补成四段再去匹配：文件里常省掉尾部 .0（见 pxc_reg::normalizeVersion）。
    const std::string target =
        req.targetVersion.empty() ? pxc_reg::normalizeVersion(out.detection.version) : req.targetVersion;

    // ---- 定 exe ----
    std::string exeUtf8, exeVer;
    if (!req.exePath.empty()) {
        fs::path ep = pxc_util::pathFromUtf8(req.exePath);
        if (!fs::is_regular_file(ep, ec)) {
            out.result = StartResult::ExeMissing;
            out.message = "登记的 PXC 已经不在这个位置了：" + req.exePath;
            pxc_log::write("ERROR", "Project", out.message);
            return out;
        }
        exeUtf8 = req.exePath;
        exeVer = pxc::readExeVersion(ep);
        if (exeVer.empty()) exeVer = target;
    } else {
        auto matches = pxc_reg::findMatching(registry, target);
        if (matches.empty()) {
            out.result = StartResult::NoVersion;
            out.message = target.empty()
                              ? "识别不出这个工程的版本，本机也没有可选的已登记版本。"
                              : "本机没有登记 " + target + " 的 PXC。";
            out.exeVersion = target;
            pxc_log::write("WARN", "Project", out.message + " 文件=" + u8(req.pxcPath));
            return out;
        }
        exeUtf8 = matches[0].exe_path;
        exeVer = matches[0].product_version;
        if (!fs::is_regular_file(pxc_util::pathFromUtf8(exeUtf8), ec)) {
            out.result = StartResult::ExeMissing;
            out.message = "版本库里登记的 " + exeVer + " 已经不在 " + exeUtf8 + " 了。";
            pxc_log::write("ERROR", "Project", out.message);
            return out;
        }
    }
    out.exePath = exeUtf8;
    out.exeVersion = exeVer;

    // 取消检查点①：定完 exe、还没动任何文件 —— 停在这里零副作用。
    if (req.canceled && req.canceled()) {
        applyCancel(out, /*builtCopy=*/false);
        return out;
    }

    // ---- 决定的打开路径（副本模式先复制）----
    fs::path openPath = req.pxcPath;
    if (req.mode == StartMode::Copy) {
        if (!req.prebuiltCopy.empty()) {
            // 调用方已经建好了（它要先拿副本路径去复用已有实例）。这里只核对文件在不在 ——
            // 不存在说明中途被删了，绝不能自己再复制一份：那会留下两份副本、报两条日志。
            std::error_code pec;
            if (!fs::is_regular_file(pxc_util::toExtended(req.prebuiltCopy), pec)) {
                out.result = StartResult::CopyFailed;
                out.message = "预先创建的那份副本不见了：" + u8(req.prebuiltCopy);
                pxc_log::write("ERROR", "Project", out.message);
                return out;
            }
            openPath = req.prebuiltCopy;
            out.copyPath = req.prebuiltCopy;
            pxc_log::write("INFO", "Project", "沿用已建副本: " + u8(out.copyPath));
        } else {
            if (req.onStep) req.onStep(Step::Copy);
            std::string cerr;
            fs::path dst = createCopy(req.pxcPath, exeVer.empty() ? target : exeVer, cerr);
            if (dst.empty()) {
                out.result = StartResult::CopyFailed;
                out.message = "创建副本失败（" + cerr + "）：" + u8(req.pxcPath.parent_path());
                pxc_log::write("ERROR", "Project", out.message);
                return out;
            }
            openPath = dst;
            out.copyPath = dst;
            pxc_log::write("INFO", "Project", "已建副本: " + u8(dst));
        }

        // 取消检查点②：副本是**本函数刚建的**。用户取消启动 → 这份没人要的文件必须清掉，
        // 否则每点一次取消就在工程目录里多一个「(副本 1.20.1)」，而用户根本没用过它。
        // 沿用调用方预先建的那份时不删：删除责任在调用方（它建的它清，见 h 文件的约定）。
        if (req.canceled && req.canceled()) {
            applyCancel(out, /*builtCopy=*/req.prebuiltCopy.empty());
            return out;
        }
    }

    // ---- 备份（只对原文件、且只在直接打开时才需要）----
    // 副本模式下原文件从头到尾没被写，备份就只是白白多占一份磁盘。
    if (req.mode == StartMode::Direct && backup.enabled && !backupDir.empty()) {
        if (req.onStep) req.onStep(Step::Backup);
        // 用 sameVersion 而不是 !=：1.20.1 与 1.20.1.0 是同一个版本，
        // 按字符串比会把它当成"强制跨版本"，每次都白备份一份。
        const bool forced = !req.targetVersion.empty() && !out.detection.version.empty() &&
                            !pxc_reg::sameVersion(req.targetVersion, out.detection.version);
        if (pxc_backup::shouldBackup(backup.enabled, backup.crossMajor, confStr(out.detection.confidence),
                                     out.detection.version, target, forced)) {
            std::string bak = pxc_backup::backupFile(req.pxcPath, backupDir);
            if (!bak.empty()) pxc_log::write("INFO", "Project", "已备份到 " + bak);
        }
    }

    // 取消检查点③：备份之后、拉起之前。过了这一行就是把决定交给 PXC 了 ——
    // 之后取消不是"撤销"，而是"杀掉一个正在写文件的进程"，不做（§9.4）。
    if (req.canceled && req.canceled()) {
        // 预先建好的副本不在这里删：它由调用方管（同一份文件不能有两处删除责任，
        // 否则一边删掉、另一边再删一次就会把"已删除"报成"删失败"）。
        applyCancel(out, req.mode == StartMode::Copy && !out.copyPath.empty() &&
                            req.prebuiltCopy.empty());
        return out;
    }

    // ---- 拉起 ----
    // 从这一行起"决定"已经交给 PXC：先报阶段（按钮随之置灰），再 CreateProcess。
    if (req.onStep) req.onStep(Step::Launch);
    const auto sp = pxc_launch::launchExe(pxc_util::pathFromUtf8(exeUtf8), openPath);
    if (!sp.ok) {
        out.result = StartResult::LaunchFailed;
        out.message = sp.detail.empty() ? "启动失败，详见日志。" : sp.detail;
        return out;
    }
    out.pid = sp.pid;

    out.result = StartResult::Ok;
    out.openedPath = openPath;
    const std::string name = pxc_util::toUtf8(openPath.filename().c_str());
    if (req.mode == StartMode::Copy) {
        out.message = "已创建副本「" + name + "」并用 " + exeVer + " 启动（原文件没改动）。";
    } else if (!out.detection.version.empty() && out.detection.version != exeVer) {
        out.message = "已用 " + exeVer + " 打开 " + name + "（文件识别为 " + out.detection.version + "）。";
    } else {
        out.message = "已用 " + exeVer + " 启动：" + name;
    }
    return out;
}

} // namespace pxc_project
