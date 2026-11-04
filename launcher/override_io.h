#pragma once
// 自定义译文文件（差异包）：只存"用户改过的那几处"，不含对照表内容。
//
// 用途有两个，格式是同一个：
//   1. 最终用户：把自己的个性化覆盖导出，带到另一台机器 / 分享给别人（启动器「导出偏好…」）；
//   2. 维护者：导出「待翻译」队列给外部翻译，翻完导回构建器合并
//      （构建器「导出待翻译 json / 导入已翻译 json」）。
// 两者都只是 key -> 文案，所以不需要两套格式。
//
// 键与对照表同构（nodes.<id>.name / words.<k> / ui.<k> / notes.<文件>），
// 换 PXC 版本、对照表更新后仍然对得上 —— 这是能跨设备的前提。
#include <filesystem>
#include <map>
#include <string>

namespace pxc_ovio {

extern const char* const kFormat;  // "pxc-launcher-overrides"

// 6 个显示表达式槽位的键名（顺序与启动器里的 6 个输入框一致）：
// node_name / node_tip / in_name / out_name / dropdown / text
extern const char* const kSlotKeys[6];

struct Bundle {
    std::string db_version;              // 导出时所用对照表的版本（仅提示用）
    std::string lang_dir;                // 语言目录名
    int preset = -1;                     // 显示预设；-1 = 文件里没有
    std::map<std::string, std::string> slots;      // 6 个显示表达式槽位
    std::map<std::string, std::string> overrides;  // key -> 文案
};

bool save(const std::filesystem::path& p, const Bundle& b);

// 读并校验 format；失败时 err 给出人话原因。
bool load(const std::filesystem::path& p, Bundle& out, std::string& err);

struct MergeStats {
    int added = 0;    // 本机没有 → 新增
    int updated = 0;   // 本机有但不同 → 覆盖
    int same = 0;      // 完全相同
};

// 合并进 dst：同键以导入值为准，本机独有的保留（不删）。
MergeStats merge(const Bundle& b, std::map<std::string, std::string>& dst);

} // namespace pxc_ovio
