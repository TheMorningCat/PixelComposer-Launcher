#pragma once
#include <string>
#include <mutex>

// 轻量日志：每条行为自动落盘到「可执行文件所在目录/logs/pxc_launcher_YYYY-MM-DD.log」。
// 全程序共享一个文件句柄策略（每次写入追加打开），单线程 GUI 下安全。
namespace pxc_log {

// 惰性初始化日志目录（基于 GetModuleFileNameW 定位 exe 目录，与运行位置无关）。
void init();

// 写一条日志。level: "DEBUG"/"INFO"/"ERROR"；tag: 模块/动作标识；msg: UTF-8 内容。
// 连续完全相同的条目只会落盘一次（其余折叠计数），避免循环事件刷爆文件。
void write(const char* level, const char* tag, const std::string& msg);

// 安装未捕获异常处理器：崩溃前把异常码与地址写进日志（不加锁，避免崩溃时死锁）。
void installCrashHandler();

} // namespace pxc_log
