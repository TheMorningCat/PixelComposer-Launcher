#pragma once
// 内嵌数据包版本探测 + 缓存（docs/07 §11.8.2）。
//
// 为什么要这一层：`pxc::readEmbeddedVersion` 要把整个 exe 顺序扫一遍（190MB，冷盘几百毫秒），
// 而它每次刷新都会被问到同几条 exe。没有缓存就等于把"设置版本号"的修复成本摊到每一次刷新上 ——
// 真机上是能摸到的卡顿。
//
// 缓存键 = 路径 + 文件大小 + 修改时间：这三样任一变了就说明 exe 被换过（Steam 更新、用户换包），
// 必须重扫。三者都没变时，扫出来的结果不可能变。
#include <filesystem>
#include <string>

namespace pxc_probe {

// 探测 exe 内嵌数据包里的版本号（结果已按 peVersion 的主次版本过滤并取最大）。
// 缓存命中直接返回；peVersion 为空 → 返回 ""（没有参照就不猜）。
std::string embeddedVersion(const std::filesystem::path& exe, const std::string& peVersion);

// 读缓存文件（启动时一次）。文件不存在 / 解析失败 → 空缓存，不影响功能。
void load(const std::filesystem::path& file);

// 有改动才写回。失败只记日志，绝不抛 —— 缓存丢了顶多下次重扫。
void flush(const std::filesystem::path& file);

// 本次进程真正扫过几个文件（日志用：判断"卡顿是不是探测造成的"）。
int scanCount();

// 缓存的条数（日志用）。
std::size_t size();

} // namespace pxc_probe
