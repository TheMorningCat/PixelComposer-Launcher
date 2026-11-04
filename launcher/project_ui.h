#pragma once
// 工程文件工作台的**语义色**与布局常量。
//
// 为什么单独抽出来：这些颜色现在只有九处绘制点用，看起来"写在哪儿都行"；
// 但一旦要做外观自定义（用户明确提过要跟随架构统一风格、后续可改），
// 散在二十个 ui.rect({r,g,b,a}) 里改一次要找二十处。抽成 token 后是"改九个字段"。
// 代价是多一层间接，而且 token 得跟着 uiTheme() 走 —— 换主题时重新派生一次即可。
//
// 绘制点只读 token，不再出现字面色值（除了纯黑/纯白的遮罩）。
#include "components/theme.h"
#include "core/dsl.h"

namespace wb {

struct Tokens {
    // 卡片
    core::Color rowBg, rowHover, rowSel, rowBorder;
    // 文字
    core::Color titleFg, subFg, metaFg, faintFg;
    // 版本 pill（v4 四态：绿=可启动 / 橙=本机没有 / 红=识别不出 / 灰=识别中）
    core::Color pillOkBg, pillOkFg;       // 已登记，双击能直接开
    core::Color pillWarnBg, pillWarnFg;   // 版本已知、本机没有（双击弹协商）
    core::Color pillBadBg, pillBadFg;     // 识别不出（读过了，读不出结构）—— 红
    core::Color pillUnkBg, pillUnkFg;     // 识别中（还没轮到）—— 灰
    // 星标 / 页签
    core::Color starOn, starOff;
    core::Color tabSelBg, tabSelFg;
    core::Color chipBg, chipFg, chipOnBg, chipOnFg;
    core::Color launchBg, launchFg;
    // 排序行底板（把「筛选 / 排序 / 刷新」框成一条工具条）
    core::Color barBg, barBorder;
};

inline Tokens make(const components::theme::ThemeColorTokens& th) {
    Tokens t;
    // 页面底之上亮一档（§4.7.4 实测值：面板 (38,38,46) → 卡片 (45,47,55)）
    t.rowBg = {0.175f, 0.185f, 0.215f, 1.0f};
    t.rowHover = {0.215f, 0.230f, 0.270f, 1.0f};
    t.rowSel = {0.190f, 0.280f, 0.400f, 1.0f};
    t.rowBorder = {0.255f, 0.275f, 0.320f, 1.0f};

    t.titleFg = {0.95f, 0.97f, 1.00f, 1.0f};
    t.subFg = {0.66f, 0.71f, 0.79f, 1.0f};
    t.metaFg = {0.55f, 0.60f, 0.68f, 1.0f};
    t.faintFg = {0.44f, 0.48f, 0.55f, 1.0f};

    t.pillOkBg = {0.118f, 0.357f, 0.271f, 1.0f};    // #1E5B45
    t.pillOkFg = {0.373f, 0.878f, 0.627f, 1.0f};    // #5FE0A0
    t.pillWarnBg = {0.420f, 0.306f, 0.078f, 1.0f};  // #6B4E14
    t.pillWarnFg = {0.941f, 0.753f, 0.376f, 1.0f};  // #F0C060
    t.pillBadBg = {0.380f, 0.145f, 0.145f, 1.0f};   // #612525
    t.pillBadFg = {1.000f, 0.498f, 0.498f, 1.0f};   // #FF7F7F
    t.pillUnkBg = {0.227f, 0.247f, 0.282f, 1.0f};   // #3A3F48
    t.pillUnkFg = {0.604f, 0.639f, 0.690f, 1.0f};   // #9AA3B0

    t.starOn = {0.910f, 0.725f, 0.235f, 1.0f};      // #E8B93C
    t.starOff = {0.40f, 0.44f, 0.51f, 1.0f};

    t.tabSelBg = th.primary;
    t.tabSelFg = {1.0f, 1.0f, 1.0f, 1.0f};
    t.chipBg = {0.160f, 0.175f, 0.205f, 1.0f};
    t.chipFg = {0.66f, 0.71f, 0.79f, 1.0f};
    t.chipOnBg = {0.180f, 0.300f, 0.450f, 1.0f};
    t.chipOnFg = {0.88f, 0.94f, 1.00f, 1.0f};

    t.launchBg = {0.20f, 0.55f, 0.45f, 0.95f};
    t.launchFg = {1.0f, 1.0f, 1.0f, 1.0f};

    // 工具条底板：夹在页面底 (33,38,43) 与卡片底 (45,47,55) 之间的一档，
    // 让这一行作为"一条工具条"看得出来，又不至于和卡片抢注意力（纯靠描边太弱，实测几乎看不出）。
    t.barBg = {0.158f, 0.170f, 0.204f, 1.0f};
    t.barBorder = {0.255f, 0.275f, 0.320f, 1.0f};
    return t;
}

// ---- 布局常量（docs/04 §8.3，改动请连同文档一起改）----
constexpr float kPad = 28.0f;
constexpr float kRowH = 80.0f;      // 行高 = 卡片 72 + 间距 8
constexpr float kCardH = 72.0f;
constexpr float kCardRadius = 10.0f;
constexpr float kCardInset = 12.0f;
constexpr float kTitleY = 24.0f;
constexpr float kTabsY = 72.0f;
// 排序 / 筛选 / 刷新 同一行（kSortY）：A1/A2 把筛选漏斗、刷新旋转箭头图标按钮提到这行，
// 不再单列筛选行。排序行与列表行间距拉到 ~40px，避免三段贴成一坨。
constexpr float kSortY = 110.0f;    // 排序 / 筛选 / 刷新行
constexpr float kListY = 150.0f;    // 列表：比 kSortY 低 40，给排序行留足

// 排序行底板：把「筛选 / 排序 / 刷新」框成一条工具条。上下各留 5，整条 34 高：
// 内容按 24 高的控件垂直居中（按钮与排序文字同一中线），底板比内容再外扩 5。
constexpr float kBarY = kSortY - 5.0f;
constexpr float kBarH = 34.0f;
// 图标按钮（漏斗 / 刷新）：28x24，里面是一枚 Font Awesome 字形
constexpr float kIconBtnW = 28.0f, kIconBtnH = 24.0f;
constexpr float kBarGap = 6.0f;

// 星标上提量：Font Awesome 的星形在 18px 行框里偏下，直接按行框居中会比标题低。
// 截图实测（1040 宽的窗口，工程卡）：标题 ink 中心 y=287.5、星标 ink 中心 y=290.0 -> 低 2.5px。
// 改字号 / 行高后要重新量这个值。
constexpr float kStarNudgeY = -2.5f;

// ---- 卡片内元素（坐标相对卡片左上角）----
constexpr float kIconX = 12.0f, kIconY = 16.0f, kIconSize = 40.0f;
constexpr float kStarX = 60.0f, kStarY = 16.0f, kStarSize = 18.0f;   // 星标与标题同基线（B3）
constexpr float kNameX = 84.0f, kNameY = 14.0f, kSubY = 42.0f;        // 标题加大、路径下移（B2）

// 版本列：徽章与时间**竖排同一列**，列左边缘 = 卡片右边缘 - 208。
// 徽章宽固定 92（版本串长度在 1.18.0.0 到 1.20.1.0 之间变，按内容撑宽会让整列左右跳）。
constexpr float kVerColRight = 208.0f;
constexpr float kVerColW = 92.0f;
constexpr float kVerBadgeY = 14.0f, kVerBadgeH = 22.0f;
constexpr float kVerDotX = 10.0f, kVerDotSize = 6.0f;
constexpr float kVerTextX = 22.0f, kVerTextW = 62.0f;
constexpr float kTimeY = 40.0f, kTimeH = 16.0f;

// 版本串用等宽字体（data/fonts/JetBrainsMono-Medium.ttf，绝对路径由 wbMonoFont() 拼）。
// 默认 UI 字体 NotoSansSC 的「1」带底部衬线，紧跟的小数点会被衬线吃掉 ——
// 截图实测 "1.20.1.0" 显示成 "120.10"、"1.18.5.0" 显示成 "118.5.0"，版本号直接读错。
// JetBrains Mono 的「1」无衬线、数字等宽，点与点之间分得开。
// 注：B1 要求卡片版本号改回默认字体（与其它文本一致），故卡片 pill 不再用等宽；
// 代价是上述小数点风险回归，必要时可在 composeWbRow 的 pillt 重新挂 wbVerFontOf。
constexpr const char* kVerMonoFile = "JetBrainsMono-Medium.ttf";

constexpr float kLaunchRight = 96.0f;
constexpr float kLaunchW = 84.0f;
constexpr float kLaunchH = 36.0f;

// 面包屑（进入态才出现）：占掉表头那行，表头与列表整体下移 kCrumbH + 6
constexpr float kCrumbH = 24.0f;
constexpr float kCrumbShift = kCrumbH + 6.0f;

// 页面内容坐标 -> 窗口坐标的偏移：左侧导航 200（kNavW）+ 页面内边距 8、标题区上边距 40。
// 根级 overlay（右键菜单 / 筛选浮窗）画的是**窗口坐标**，而列表是内容坐标，
// 少了这层换算浮窗会盖到左侧导航上。改 kNavW 或页面 padding 时要同步这里。
constexpr float kPageX = 208.0f;
constexpr float kPageY = 40.0f;

// 筛选浮窗
constexpr float kFilterPanelW = 380.0f;
constexpr float kFilterPanelH = 428.0f;

// 列表视口右侧被滚动条吃掉多少：virtualList 内部按
// contentWidth = viewportWidth - scrollbarWidth - scrollbarGap 算行宽，
// 也就是它回调给 row 的 width **已经是内容宽**。
// 表头是照页面坐标画的、数据是照内容坐标画的，两边拿同一个参考点才不会错位；
// 这个常量就是那根参考线（scrollbarWidth(8) + scrollbarGap(6)，改列表参数时一起改）。
constexpr float kListScrollReserve = 14.0f;

} // namespace wb
