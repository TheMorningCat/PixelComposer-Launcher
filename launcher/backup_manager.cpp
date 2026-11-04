// 备份实现。
#include "backup_manager.h"
#include "log.h"
#include "util.h"

#include <ctime>

namespace pxc_backup {

static int majorOf(const std::string& v) {
    if (v.empty()) return -1;
    try { return std::stoi(v); } catch (...) { return -1; }
}

bool shouldBackup(bool enabled, bool crossMajor, const std::string& confidence,
                  const std::string& detectedVersion, const std::string& targetVersion,
                  bool forcedOverride) {
    if (!enabled) return false;
    if (confidence == "低" || confidence == "无") return true;
    if (forcedOverride && detectedVersion != targetVersion) return true;
    if (crossMajor) {
        int d = majorOf(detectedVersion), t = majorOf(targetVersion);
        if (d >= 0 && t >= 0 && d != t) return true;
    }
    return false;
}

std::string backupFile(const std::filesystem::path& pxcPath, const std::filesystem::path& backupDir) {
    std::error_code ec;
    if (!std::filesystem::exists(pxc_util::toExtended(pxcPath), ec)) {
        pxc_log::write("ERROR", "Backup", "原文件不存在: " + pxcPath.u8string());
        return "";
    }
    std::filesystem::create_directories(pxc_util::toExtended(backupDir), ec);

    // UTC 时间戳（宽字符串拼接，避免窄串编码歧义）
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_s(&tm, &t);
    wchar_t ts[32];
    std::wcsftime(ts, 32, L"%Y%m%dT%H%M%SZ", &tm);

    std::filesystem::path bak = backupDir;
    bak /= (pxcPath.stem().wstring() + L"_" + std::wstring(ts) + L".pxc.bak");
    std::filesystem::copy_file(pxc_util::toExtended(pxcPath), pxc_util::toExtended(bak),
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        pxc_log::write("ERROR", "Backup", "复制失败: " + pxcPath.u8string() + " -> " + bak.u8string() +
                       " | " + ec.message());
        return "";
    }
    pxc_log::write("INFO", "Backup", "已备份: " + pxcPath.u8string() + " -> " + bak.u8string());
    return bak.u8string();
}

} // namespace pxc_backup
