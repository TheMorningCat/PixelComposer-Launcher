#pragma once
// Windows 原生窗口集成层（应用层，不修改 EUI-NEO 框架）：
//   1. OS 级文件拖放：EUI-NEO 只处理指针事件，不接收 WM_DROPFILES，
//      故在这里取到主窗口 HWND，注册 DragAcceptFiles 并子类化窗口过程接收拖放量。
//   2. 通用文件/文件夹选择对话框：IFileDialog（带地址栏与左侧快速访问导航窗格），
//      替代只有树形视图、不能输入地址的 SHBrowseForFolderW。
//
// 本头文件刻意不引入 windows.h（app.cpp 中 EUI 头文件必须排在 Windows 头文件之前）。
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pxc_nui {

// 安装拖放支持；成功返回 true（幂等，已安装也返回 true）。
// 窗口尚未创建时返回 false，调用方可在后续帧重试。
// 回调在 UI 线程（窗口过程内）触发，参数是被拖入的文件/文件夹绝对宽路径。
bool installDrop(std::function<void(std::vector<std::filesystem::path>)> onDrop);

// 接收单实例转发过来的路径（WM_COPYDATA）。和拖放共用同一次子类化。
// 回调同样在 UI 线程窗口过程内触发 —— 回调里只该"记下待处理"，不要做重活。
bool installCopyData(std::function<void(std::vector<std::filesystem::path>)> onCopyData);

// 接收 worker 的"启动任务有结果了"通知（自定义消息）。
// 为什么走窗口过程而不是 compose：主窗最小化时 compose 一次都不跑（框架遇 GLFW_ICONIFIED 直接 continue），
// 而启动恰恰发生在最小化的时候 —— 挂在 compose 上的回调会永远收不到。
// 回调在主线程（窗口过程内）执行，可以安全改 g_wbStore 并同步写盘。
bool installLaunchDone(unsigned int msg, std::function<void()> onDone);

// 给主窗 Post 一条自定义消息（跨线程唤醒用：worker 干完活、第二实例转发进来）。
// 只做投递，不等处理 —— 调用方可以是从别的线程，也可以是窗口过程里（不会自锁）。
void postToMainWindow(unsigned int msg);

// 本进程主窗（未创建返回 nullptr）。
void* mainWindow();

// 给主窗按当前 DPI 重设大/小图标（exe 资源 `GLFW_ICON`，见 launcher/res/app.rc）。
// 为什么还要这一步：GLFW 只给窗口类设了 32px 的大图标，hIconSm 留空 → 标题栏/Alt+Tab 的
// 16px 图标由它硬缩，边缘糊。幂等；窗口未创建或没有资源时返回 false，调用方后续帧重试。
bool applyWindowIcon();
// 隐藏主窗（不再用于静默启动；保留给"拉起来就退"的旧行为）。
bool hideMainWindow();

// 最小化到任务栏且**不抢焦点**（双击 .pxc 的常驻形态）。
// 为什么不直接 ShowWindow(SW_MINIMIZE)：那会激活窗口，把刚拉起的 PXC 挤到后面，
// 而且复用链路要求 PXC 保持前台（否则 Ctrl+O 打不出去）。SW_SHOWMINNOACTIVE 两样都不犯。
bool minimizeToTaskbar();
// 重新显示主窗（静默启动途中发现需要用户决定，例如缺版本协商，就得把窗口还给他）。
bool showMainWindow();

// ---- 窗口几何（界面缩放用，docs/06 §10.4.1.1）----
// 读主窗外框矩形（含标题栏，GetWindowRect 语义）。窗口未创建返回 false。
bool getMainWindowRect(int& x, int& y, int& w, int& h);
// 原样恢复外框矩形（回滚用，不做夹取/居中 —— 回到用户按"保留"之前的那个位置）。
bool setMainWindowRect(int x, int y, int w, int h);
// 把主窗**客户区**设成 cw×ch（GLFW windowSize 语义），居中到当前显示器，超工作区按工作区夹取。
// outW/outH 回填实际客户区尺寸（可能因夹取小于目标）。最大化/最小化会先还原（异步，需轮询）。
bool setMainWindowClientSize(int cw, int ch, int& outW, int& outH);

// 在资源管理器里定位文件 / 打开文件夹。
bool revealInExplorer(const std::filesystem::path& path, bool selectFile);

// 用户可变数据根目录：exe 同级的 userdata/（便携，随程序走；分发时排除此目录即可）。
// exe 目录写不进（如装到 Program Files）时退 %LOCALAPPDATA%\PixelComposer-Launcher\。
// 与分发用的程序 + 只读资源（data/ 字体·对照表·图标、assets/）彻底分离，
// 首次运行会自动创建；分发时只要不打包 userdata/，就不会把自己的设置发出去。
std::filesystem::path userDataDir();

// 文件夹选择对话框（IFileDialog + FOS_PICKFOLDERS）；取消/失败返回空路径。
std::filesystem::path pickFolder(const wchar_t* title);

// 文件选择对话框；filters = {描述, 通配符}，如 {L"PXC 工程", L"*.pxc"}；取消/失败返回空路径。
std::filesystem::path pickFile(const wchar_t* title,
                               const std::vector<std::pair<std::wstring, std::wstring>>& filters);

// 另存为对话框（IFileDialog + FOS_OVERWRITEPROMPT，带覆盖确认）；取消/失败返回空路径。
std::filesystem::path pickSave(const wchar_t* title, const std::wstring& defaultName,
                               const std::vector<std::pair<std::wstring, std::wstring>>& filters,
                               const wchar_t* defaultExt = nullptr);

} // namespace pxc_nui
