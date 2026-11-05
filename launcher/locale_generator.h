#pragma once
// 语言文件生成引擎（运行期，最终用户用）。
//
// 输入：目标版本的英文骨架 + 对照表 + 显示表达式 + 字体
// 输出：完整的语言目录（写进 PXC 用户目录 Locale/<lang>/）
//
// 三条硬约束（防闪退 / 防缺字）：
//   1. 结构照抄骨架：骨架有的 key 一个不少；
//   2. 类型不动：基准字段是非字符串（{} / [] / bool）时原样复制 —— 官方 en 自带这类值，
//      PXC 能正常读，擅自改成字符串反而改变加载行为；
//   3. 不写空串：无译文回退英文。
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "baseline_scanner.h"
#include "translation_db.h"

namespace pxc_gen {

// 显示表达式：按内容分项配置（§5.4）。占位符 %en / %zh。
struct ExprSet {
    std::string nodeName = "%zh";
    std::string nodeTip = "%zh";
    std::string inName = "%zh";
    std::string outName = "%zh";
    std::string dd = "%zh";
    std::string text = "%zh";  // words.json + UI.json

    // 预设：0 纯中文 / 1 中文在前 / 2 英文在前（只改节点名，其余保持纯中文）
    static ExprSet preset(int which);
};

struct FontPick {
    std::filesystem::path uiFont;    // 界面字体 ttf/otf（空 = 不写 fonts/）
    std::filesystem::path codeFont;  // 代码字体（空 = 复用 uiFont）
    std::string label;               // 展示名
};

struct Options {
    bool perCharacterLineBreak = true;  // 中文必须 true（官方 LOCALIZATION GUIDES）
    bool writeUi = true;                // 输出 UI.json（骨架有，或对照表有补充键）
    bool writeFonts = true;
    bool writeNotes = true;
    std::filesystem::path notesZhDir;   // 中文 notes 目录（含 _map.json：英文文件名 -> 中文文件名）

    // 自定义译文层（个性化微调）：键与对照表完全一致（nodes.<id>.name / words.<k> / ui.<k> …），
    // 值是该键的**中文文案**。查找优先级最高 —— 用户只改自己不习惯的那几处，
    // 对照表本身仍然只读、随程序分发。空指针 = 没有自定义译文层。
    const std::map<std::string, std::string>* overrides = nullptr;
};

struct Stats {
    bool ok = false;
    std::string error;
    std::string outDir;   // 实际写入的语言目录

    int files = 0;
    int nodes = 0;         // 节点数
    int words = 0;         // 词条数
    int ui = 0;            // UI 键数
    int fields = 0;        // 渲染的字段总数
    int translated = 0;    // 命中译文的字段数
    int fallbackEn = 0;    // 未命中、回退英文的字段数
    int missingNodes = 0;  // 完全没有译文的节点数
    int ovHits = 0;        // 被「自定义译文层」改写的字段数（0 = 覆盖没生效或没设）
    int normHits = 0;      // 端口键走「归一化回退」命中的条数（§8.1.6：只对端口键生效）

    // 未命中的键。只作诊断记录：补译走构建器的「导出待翻译 / 导入译文」闭环，
    // 早期那个「导出未译缺口」按钮已删除（它导全量未译，噪音比信号多）。
    std::vector<std::string> missingKeys;
};

// 生成：先写到 <outRoot>/.pxc_gen_tmp/<lang>，自检通过后原子替换旧目录（**不留备份**，
// 见 .cpp 里替换段的注释：汉化是生成物，留备份只会让用户目录无限膨胀）。
Stats generate(const pxc_base::Baseline& base, const pxc_tdb::Db& db, const ExprSet& ex,
               const FontPick& font, const Options& opt, const std::filesystem::path& outRoot,
               const std::string& langDir);

} // namespace pxc_gen
