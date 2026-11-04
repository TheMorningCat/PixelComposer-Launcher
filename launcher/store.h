#pragma once
// 轻量 JSON：足够覆盖本启动器的扁平/浅层嵌套 schema（registry / locale / backup）。
// 不依赖第三方库，便于离线构建与头less 测试。中文以 UTF-8 原样读写（符合 JSON UTF-8 规范）。
// 路径一律用 std::filesystem::path（宽路径），避免 MSVC 对窄串的 GBK 解码错误。
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace pxc_store {

struct JsonValue {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double num = 0.0;
    // 原字面量是否写成浮点（含 . / e / E）。序列化时若为整数值也补 ".0"，
    // 保证回写 PXC 的 nodes.json 时数字形态与原文一致（PXC 对字段类型敏感）。
    bool numFloat = false;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj; // 保留插入顺序

    bool isObj() const { return type == Obj; }
    bool isStr() const { return type == Str; }
    bool isArr() const { return type == Arr; }
    const JsonValue* find(const std::string& k) const;
    JsonValue* find(const std::string& k);
};

// 解析；second=false 表示遇到无法解析的内容（调用方应安全降级）。
std::pair<JsonValue, bool> parse(const std::string& text);

// 序列化（2 空格缩进）。
std::string serialize(const JsonValue& v, int indent = 2);

// ---- 应用配置 schema ----
struct ExeEntry {
    std::string exe_path;
    std::string product_version;
    std::string display_name;
    std::string source;      // "auto" | "manual"
    std::string registered_at;
    // 分发渠道（docs/07 §11.6）："itch" | "steam" | ""（空 = 还没判定）。
    // 读盘时刻意留空 —— store::load 是纯解析，判"在不在 Steam 库里"要读文件系统，
    // 那是刷新（pxc_reg::refreshAll）的职责，这里不引入 IO 副作用。
    std::string channel;
    // 用户已确认过的版本（docs/07 §11.7）。Steam 版会自己变版本，角标 = 当前版本 != 这个值。
    // 刻意不叫 prev_version：连续更新两次而不确认时，prev 会被覆盖成"上一次读到的值"，
    // 用户就看不到"从 1.20 来的"；acked 只在点「知道了」时推进，语义不会漂。
    std::string acked_version;
};

struct LocaleSettings {
    std::string source_dir;  // 汉化包根目录（含 zh/ce-zh/ec-zh）或语言子目录本身
    std::string mode;        // zh | ce-zh | ec-zh
    std::string install_dir; // PXC 安装目录
    std::string font;        // 选中 ttf 文件名（可选）
    std::map<std::string, std::string> overrides; // words.json key -> 中文
};

struct BackupSettings {
    bool enabled = true;
    bool crossMajor = true;
    std::string dir;         // 空 => <启动器目录>/pxc_backups
};

// 字号基准（docs/06 §10.4.2.2）：EUI 的字号相对控件尺寸偏小（26 高的按钮配 12 px 字），
// 直接用 100% 看着吃力。所以本应用把**框架原始字号整体乘 1.4** 当作"正常大小"，
// 这个 1.4 就是滑块上那个 100% 的物理含义。它不进配置 —— 改它等于改设计基准，
// 不是用户该调的旋钮；用户可调的是这个基准上的相对倍率（0.70 ~ 1.06）。
// 提示窗（原生 GDI，不在 EUI text pipeline 里）**只吃相对倍率**，所以 100% 时它保持原设计尺寸。
constexpr float kTextBaseScale = 1.40f;

// 界面设置（docs/06 §10.5）。
struct UiSettings {
    float scale = 1.0f;       // 界面缩放 0.75 ~ 2.00，落值量化到 0.05（26 档）
    // 文本缩放：**相对 kTextBaseScale 的倍率**（v4 起语义变了，v3 存的是绝对倍率，
    // 读盘时除以基准迁移过来）。只允许离散档（见 docs/06 §10.4.2.1）。
    float text_scale = 1.0f;
    // 主题色（强调色）：只换 theme 里的 primary 一个 token。存成 "#RRGGBB" 是为了
    // 肉眼可读、能手改 —— 三个 0~1 浮点写在 json 里既看不懂也改不动。
    std::string accent = "#FF9166";
};

// 汉化生成设置（运行期：用户可见的选项）。
// 对照表本身是只读资源，路径固定为 <exe>/data/translation_db.json。
struct GenSettings {
    std::string user_dir;    // PXC 用户目录（含 Locale/ 的那一层）
    std::string lang_dir;    // 语言文件夹名（zh / ce-zh / ec-zh / 自定义）
    int preset = 0;          // 0 纯中文 / 1 中文在前 / 2 英文在前
    std::string ex_node_name = "%zh";
    std::string ex_node_tip = "%zh";
    std::string ex_in_name = "%zh";
    std::string ex_out_name = "%zh";
    std::string ex_dd = "%zh";
    std::string ex_text = "%zh";
    std::string ui_font;     // 界面字体文件路径（UTF-8）
    std::string code_font;   // 代码字体文件路径（UTF-8）
    // 目标版本（docs/08 §8.2.4）：生成时用哪个已装版本的英文骨架。四段版本号；空 = 未选，
    // 走默认规则（取已装版本里最高的）。一个包只能对一个版本正确 —— 端口按下标查。
    std::string target_version;
    // 启动前按「即将启动的那个 exe 的版本」重新生成一次（docs/08 §8.2.5）。默认关：
    // 它会在每次新起 PXC 进程前重写语言目录，属于"用户没点也改了文件"，得他自己开。
    bool auto_gen_on_launch = false;
    bool write_ui = true;
    bool write_fonts = true;
    bool write_notes = true;
    // 构建器（维护者用，隐藏入口）
    std::string baseline;                 // 构建对照表用的英文骨架目录
    std::vector<std::string> pack_sources; // 汉化包语言目录（顺序 = 优先级）
};

struct AppConfig {
    int version = 6;
    std::vector<ExeEntry> registry;
    // 启动偏好（docs/07 §11.6.2）：四段版本号 -> "itch" | "steam"。
    // 三种状态：**无键** = 没问过；**空串** = 问过但用户关掉了（走兜底，不再问）；
    // 值是渠道 = 已表态。键用版本号而不是渠道 —— 偏好只在"这一版同时有两张"时有意义。
    std::map<std::string, std::string> prefer;
    LocaleSettings locale;
    BackupSettings backup;
    UiSettings ui;
    GenSettings gen;
    std::string lastFolder;
};

// 读取；文件不存在或解析失败返回默认配置（ok=false）。
// baseDir 非空时，把落盘的相对 exe_path 按 baseDir 解析成绝对（便于和 PXC 一起打包）。
// dupRemoved 非空时回填"这次读盘合并掉的重复条目数"（>0 表示需要立刻回写一次，见 app 的 initOnce）。
std::pair<AppConfig, bool> load(const std::filesystem::path& path,
                                const std::filesystem::path& baseDir = {},
                                int* dupRemoved = nullptr);

// 写入；成功返回 true。
// baseDir 非空时，落在 baseDir 或其父级下的绝对 exe_path 会转成相对 baseDir 再写（docs/07 §11.2.6）。
bool save(const std::filesystem::path& path, const AppConfig& cfg,
          const std::filesystem::path& baseDir = {});

} // namespace pxc_store
