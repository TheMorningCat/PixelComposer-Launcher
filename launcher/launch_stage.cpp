#include "launch_stage.h"

namespace pxc_stage {
namespace {

// 次行的统一形状：有版本就写「版本：x　」前缀，没有就只写后半句。
std::string line2Of(const std::string& version, const std::string& rest) {
    if (version.empty()) return rest;
    return "版本：" + version + "\u3000" + rest;
}

} // namespace

std::string fileNameOf(const std::string& path) {
    // 只认 / 和 \ 两种分隔符（Windows 上两种都会出现：配置里存的是 /，系统给的是 \）。
    const std::size_t cut = path.find_last_of("/\\");
    std::string name = (cut == std::string::npos) ? path : path.substr(cut + 1);
    // 末尾带分隔符的目录路径：去掉尾分隔符再取一次。
    if (name.empty() && cut != std::string::npos) {
        const std::string trimmed = path.substr(0, cut);
        return fileNameOf(trimmed);
    }
    return name;
}

const char* stageName(Stage s) {
    switch (s) {
    case Stage::Resolve: return "resolve";
    case Stage::Copy: return "copy";
    case Stage::Backup: return "backup";
    case Stage::Spawn: return "spawn";
    case Stage::WaitWindow: return "wait_window";
    case Stage::ReuseFind: return "reuse_find";
    case Stage::ReuseWaitWindow: return "reuse_wait_window";
    case Stage::ReuseRestore: return "reuse_restore";
    case Stage::ReuseWaitIdle: return "reuse_wait_idle";
    case Stage::ReuseHotkey: return "reuse_hotkey";
    case Stage::ReuseRetry: return "reuse_retry";
    case Stage::ReuseDialog: return "reuse_dialog";
    case Stage::ReuseFill: return "reuse_fill";
    case Stage::ReuseVerify: return "reuse_verify";
    case Stage::Ok: return "ok";
    case Stage::OkReuse: return "ok_reuse";
    case Stage::Warn: return "warn";
    case Stage::Failed: return "failed";
    case Stage::Canceled: return "canceled";
    case Stage::AskNewProcess: return "ask_new_process";
    }
    return "unknown";
}

bool isReuse(Stage s) {
    return s >= Stage::ReuseFind && s <= Stage::ReuseVerify;
}

bool cancelable(Stage s) {
    switch (s) {
    // 还没把决定交给 PXC：停在这里没有任何副作用，副本/备份留着即可。
    case Stage::Resolve:
    case Stage::Copy:
    case Stage::Backup:
    // 复用链路在点下「打开」之前都能停（ReuseDialog/ReuseFill 会顺手关掉我们弹的对话框）。
    case Stage::ReuseFind:
    case Stage::ReuseWaitWindow:
    case Stage::ReuseRestore:
    case Stage::ReuseWaitIdle:
    case Stage::ReuseHotkey:
    // 重发按键同样是"还没交给 PXC"：多发一次 Ctrl+O 不改任何文件。
    case Stage::ReuseRetry:
    case Stage::ReuseDialog:
    case Stage::ReuseFill:
        return true;
    default:
        return false;
    }
}

Text makeText(Stage s, const std::string& fileName, const std::string& version,
              int elapsedSec, const std::string& detail) {
    Text t;
    const std::string name = fileName.empty() ? std::string("工程") : fileName;
    t.cancelable = cancelable(s);
    t.primary = Action::Cancel;
    t.primaryLabel = "取消";   // 不可取消时置灰，但**不换字**：换字会让用户以为按钮变了

    switch (s) {
    case Stage::Resolve:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 启动中");
        break;
    case Stage::Copy:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 启动中（正在创建副本）");
        break;
    case Stage::Backup:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 启动中（正在备份原文件）");
        break;
    case Stage::Spawn:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 启动中");
        break;
    case Stage::WaitWindow:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 启动中\u3000已 " + std::to_string(elapsedSec) + " 秒");
        break;

    case Stage::ReuseFind:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 已启动，正在查找窗口");
        break;
    case Stage::ReuseWaitWindow:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 已启动，等它的窗口出现\u3000已 " +
                                       std::to_string(elapsedSec) + " 秒");
        break;
    case Stage::ReuseRestore:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 已启动，正在把窗口调到前台");
        break;
    case Stage::ReuseWaitIdle:
        // 卡住的 PXC 会把我们的按键排进队列，等它缓过来一次性当成几次按下 —— 所以先等它回话。
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 已启动但正在忙，等它响应\u3000已 " +
                                       std::to_string(elapsedSec) + " 秒");
        break;
    case Stage::ReuseHotkey:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 已启动，正在发送打开快捷键");
        break;
    case Stage::ReuseRetry:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 已启动，正在重试打开快捷键");
        break;
    case Stage::ReuseDialog:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 已启动，正在等待文件对话框");
        break;
    case Stage::ReuseFill:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 已启动，正在填入工程路径");
        break;
    case Stage::ReuseVerify:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "PXC 已启动，正在确认打开结果");
        break;

    case Stage::Ok:
        t.line1 = "已启动 " + name;
        t.line2 = line2Of(version, "PXC 窗口已出现");
        t.tone = Tone::Ok;
        t.primary = Action::Close;
        t.primaryLabel = "关闭";
        t.autoCloseMs = 3000;
        break;
    case Stage::OkReuse:
        t.line1 = "已在 PXC 里打开 " + name;
        t.line2 = line2Of(version, "新增了一个文件标签");
        t.tone = Tone::Ok;
        t.primary = Action::Close;
        t.primaryLabel = "关闭";
        t.autoCloseMs = 3000;
        break;
    case Stage::Warn:
        t.line1 = "正在启动 " + name;
        t.line2 = line2Of(version, "比预期慢（已 " + std::to_string(elapsedSec) + " 秒）");
        t.tone = Tone::Warn;
        break;
    case Stage::Failed:
        t.line1 = "启动失败" + (fileName.empty() ? std::string() : " " + name);
        t.line2 = line2Of(version, detail.empty() ? "原因不明，可查看日志" : detail);
        t.tone = Tone::Fail;
        t.primary = Action::Close;
        t.primaryLabel = "关闭";
        break;
    case Stage::Canceled:
        t.line1 = "已取消";
        t.line2 = line2Of(version, detail.empty() ? "启动已中止" : detail);
        t.tone = Tone::Muted;
        t.primary = Action::Close;
        t.primaryLabel = "关闭";
        break;
    case Stage::AskNewProcess:
        // 由 makeAskNewProcess 统一构造，这里给一个不会用到的兜底。
        t.line1 = "复用没成功";
        t.line2 = line2Of(version, "要重试还是开新进程？");
        t.tone = Tone::Warn;
        t.cancelable = true;
        t.primary = Action::Retry;
        t.primaryLabel = "重试";
        t.secondary = Action::NewProcess;
        t.secondaryLabel = "开启新进程";
        t.tertiary = Action::Cancel;
        t.tertiaryLabel = "取消";
        break;
    }
    return t;
}

Text makeAskNewProcess(const std::string& fileName, const std::string& version,
                       const std::string& reason, bool allowNewProcess) {
    Text t;
    const std::string name = fileName.empty() ? std::string("工程") : fileName;
    t.line1 = "复用没成功 " + name;
    t.line2 = line2Of(version, (reason.empty() ? "没能在已开的 PXC 里打开" : reason) +
                                   (allowNewProcess ? "，要重试还是开新进程？" : "，要重试吗？"));
    t.tone = Tone::Warn;
    t.cancelable = true;
    // 按钮从左到右：取消 / 开启新进程 / 重试。重试放最右（主位）：
    // 它是唯一不会带来副作用的选择 —— 再试一次只是重复"让已开的 PXC 打开这个文件"；
    // 而「开启新进程」会真的多开一个 PXC 去抢同一份 autosave，所以它退到次位。
    t.primary = Action::Retry;
    t.primaryLabel = "重试";
    if (allowNewProcess) {
        t.secondary = Action::NewProcess;
        t.secondaryLabel = "开启新进程";
        t.tertiary = Action::Cancel;
        t.tertiaryLabel = "取消";
    } else {
        t.secondary = Action::Cancel;
        t.secondaryLabel = "取消";
    }
    return t;
}

Stage waitWindowStage(int elapsedSec) {
    if (elapsedSec >= kFailAfterSec) return Stage::Failed;
    if (elapsedSec >= kWarnAfterSec) return Stage::Warn;
    return Stage::WaitWindow;
}

} // namespace pxc_stage
