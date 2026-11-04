#pragma once
// 启动前备份：判定是否需要备 + 复制 .pxc 到备份目录（带 UTC 时间戳，只读副本）。
#include <filesystem>
#include <string>

namespace pxc_backup {

// 判定是否备份。触发任一：低置信度 / 跨大版本 / 用户强制用非推荐版本 / 未关闭备份。
bool shouldBackup(bool enabled, bool crossMajor, const std::string& confidence,
                  const std::string& detectedVersion, const std::string& targetVersion,
                  bool forcedOverride);

// 复制原文件到备份目录，命名 <原名>_<UTC时间戳>.pxc.bak。成功返回目标路径，失败返回空。
std::string backupFile(const std::filesystem::path& pxcPath, const std::filesystem::path& backupDir);

} // namespace pxc_backup
