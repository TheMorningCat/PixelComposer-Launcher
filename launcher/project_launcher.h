#pragma once
// 工程启动的统一入口。
//
// 为什么要有这么一层：双击 .pxc、列表里的「启动」、缺版本协商弹窗、以后命令行传参，
// 四条路最后做的事完全一样（识别 → 定版本 → 可选建副本 → 可选备份 → 拉起 exe）。
// 分散写在各个 UI 回调里，迟早会出现"从列表启动会备份、双击不备份"这种不一致 ——
// 而备份与副本恰恰是防止跨版本写坏原文件的唯一屏障，不能有分叉。
//
// 这一层不碰 UI：匹配不到版本时不自己弹框，而是返回 NoVersion 让调用方去问用户。
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "store.h"
#include "pxc_detect.h"

namespace pxc_project {

enum class StartMode {
    Direct,  // 用匹配到的 exe 直接打开原文件
    Copy,    // 先在工程目录里建一份副本，用副本打开（跨版本的安全路径）
};

/// 会被上报出去的步骤。顺序即执行顺序，最后一步 `Launch` 之后就不可取消了。
enum class Step {
    Copy,    // 正在建副本
    Backup,  // 正在备份原文件
    Launch,  // 即将 CreateProcess（提示窗在这一步把取消按钮置灰）
};

enum class StartResult {
    Ok,
    NoVersion,     // 识别/指定的版本在本机没有登记 → 调用方应弹协商窗
    ExeMissing,    // 登记的 exe 文件不见了
    CopyFailed,    // 建副本失败（磁盘/权限/超长路径）
    LaunchFailed,  // CreateProcess 失败
    Canceled,      // 走到拉起之前被取消（本次刚建的副本已由本函数删掉）
};

struct StartRequest {
    std::filesystem::path pxcPath;
    std::string targetVersion;  // 空 = 用文件识别出的版本
    std::string exePath;        // 空 = 按 targetVersion 去版本库里找
    StartMode mode = StartMode::Direct;

    /// 已经建好的副本（Copy 模式）。非空 = **不再复制**，直接拿它当打开目标。
    ///
    /// 为什么要能"预先建副本"：跨版本时要先用同版本实例复用（把副本塞进用户正在用的
    /// 那个 PXC，而不是再开一个进程），而复用发生在 `startProject` 之前 —— 副本必须
    /// 在那里就已经存在（Ctrl+O 打不开一个还不存在的文件）。谁建的谁负责：调用方建、
    /// 调用方在"最终没用上"时删。
    std::filesystem::path prebuiltCopy;

    /// 取消查询（可空）。**只在三个位置问**：定完 exe 后 / 建完副本后 / 拉起前。
    /// 拉起之后不再问 —— 那时取消等于杀掉一个正在写文件的进程（docs/05 §9.4）。
    std::function<bool()> canceled;
    /// 步骤上报（可空）。给提示窗显示"正在创建副本"，也让用户知道现在点取消还来得及。
    std::function<void(Step)> onStep;
};

struct StartOutcome {
    StartResult result = StartResult::NoVersion;
    std::string message;                 // 可直接展示的中文单行结论
    std::filesystem::path openedPath;    // 实际交给 PXC 的工程路径（副本模式 = 副本）
    std::filesystem::path copyPath;      // 副本路径（没建副本则为空）
    /// 因为取消而回滚时：本次刚建的副本是否已删掉。false 且 copyPath 非空 = 删失败（被占用）。
    bool copyRemoved = false;
    std::string exePath;
    std::string exeVersion;
    /// 拉起成功时 PXC 的 pid（0 = 没拉起）。调用方靠它等主窗出现 / 判断进程是否还活着。
    unsigned long pid = 0;
    pxc::DetectionResult detection;      // 回传识别结果，调用方算推荐时不必再读一遍文件
};

// 生成不冲突的副本路径：`<stem> (副本 <tag>).pxc`，重名依次加 " (2)" " (3)"。
// tag 为空时是 `<stem> (副本).pxc`。只算路径，不创建文件（调用方负责 copy）。
std::filesystem::path makeCopyPath(const std::filesystem::path& src, const std::string& tag);

// 在工程所在目录建一份副本，返回副本路径；失败返回空并把原因写进 err。
// 单独抽出来是为了能被 headless 测试直接覆盖 —— 这是"跨版本不毁原文件"的落地动作，
// 不能只靠 UI 上点一次来验证。
std::filesystem::path createCopy(const std::filesystem::path& src, const std::string& tag,
                                 std::string& err);

// 唯一启动入口。
//   backupDir 为空表示"不备份"（调用方没配备份目录时）；
//   backup 开关为 false 时同样跳过。
StartOutcome startProject(const std::vector<pxc_store::ExeEntry>& registry,
                          const pxc_store::BackupSettings& backup,
                          const std::filesystem::path& backupDir,
                          const StartRequest& req);

} // namespace pxc_project
