#include "launch_toast.h"

#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

// nanosvg 是 EUI 的既有依赖（EUI-NEO-main/3rd，EUI 根目录已在 eui::neo 的 PUBLIC include 里），
// 这里只取它的**声明** —— 实现体在 EUI 的 image_source.cpp 里已经编进库了，
// 自己再 #define NANOSVG_IMPLEMENTATION 会和库里重复符号。
#include "3rd/nanosvg.h"
#include "3rd/nanosvgrast.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pxc_toast {
namespace {

constexpr wchar_t kClassName[] = L"PxcLaunchToast";
constexpr UINT WM_TOAST_REFRESH = WM_APP + 0x51;
constexpr UINT WM_TOAST_HIDE = WM_APP + 0x52;
constexpr UINT_PTR kAutoCloseTimer = 1;

// 圆角之外的"色键"色：品红。深色 UI 里不会自然出现这个颜色，抠掉不会误伤内容。
constexpr COLORREF kKeyColor = RGB(255, 0, 255);

// 400 宽（初始值；真正宽度按内容算，见 preferredWidth）：主行 + 次行 + 右侧按钮，左右各留 16。
// 比 docs/05 §9.6.1 的 360 宽一些 —— 实测 360 装不下「版本：1.18.5.0　比预期慢（已 27 秒）」，
// 会折行并把逗号顶到行首。
constexpr int kWidthDip = 400;
// 高度**不再定死**：文本缩放拉到 150% 时两行字要占更多竖向空间，钉死 104 会把次行裁掉。
// 104 这个数是「16 + 24 + 4 + 44 + 16」（上留白 / 主行 / 间隙 / 次行两行余量 / 下留白），
// 现在改由 preferredHeightPx() 按当前字号现算，100% 时结果仍是 104。

// ---- 跨线程状态 ----
std::mutex g_mtx;
pxc_stage::Text g_text;                  // 待显示内容（受 g_mtx 保护）
bool g_dirty = false;
pxc_stage::Action g_action = pxc_stage::Action::None;
std::wstring g_fontDir;

std::atomic<bool> g_visible{false};
std::atomic<bool> g_quit{false};
HWND g_hwnd = nullptr;
HANDLE g_readyEvent = nullptr;
std::thread g_thread;

// 描边色（主题色）。提示窗是原生窗，不在 EUI 的 UI 树里，拿不到 `uiTheme()`，
// 所以主题色只能由 app 显式传进来（`setAccent`）。默认 = 启动器的默认主题色。
std::atomic<unsigned int> g_accentRgb{RGB(0xEC, 0x74, 0x49)};

// ---- 线程局部（只在该线程的消息过程里读写）----
// 绘制读 `g_current` 而不是 `g_text`：`g_text` 由 worker 线程写，跨线程直接读会撕裂
// （std::string 的缓冲区可能在读的过程中被换掉）。刷新消息里拷一份过来，之后只读副本。
pxc_stage::Text g_current;
HFONT g_fontMain = nullptr;
HFONT g_fontSub = nullptr;
HFONT g_fontBtn = nullptr;
int g_dpi = 96;
pxc_stage::Action g_hoverAction = pxc_stage::Action::None;

// ---- 侧面滑入 ----
// 只在"隐藏 → 显示"那一次做：内容刷新（等窗口时每秒刷"已 N 秒"）不参与，
// 否则窗口每秒被拽回屏幕外一次，看着像在抽搐。
constexpr UINT_PTR kSlideTimer = 2;
constexpr int kSlideMs = 220;
bool g_sliding = false;
ULONGLONG g_slideStart = 0;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((std::size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// 文本缩放倍率（设置页「文本大小」）。提示窗是原生 GDI，**不吃 EUI 的 `fontSize()` 系数**
// —— 那是个纯 Dsl 层的乘数，GDI 这条路上没有 text pipeline，只能显式传一份倍率过来自己乘。
// 跨线程：main 线程写（设置页松手 / 首帧初始化），toast 线程在每次刷新时读，故用 atomic。
std::atomic<float> g_textScale{1.0f};

int scale(int dip) { return MulDiv(dip, g_dpi, 96); }

// 字号专用：DPI 缩放之后再乘文本缩放，四舍五入到整数像素（GDI 的 lfHeight 是整数）。
int textPx(int dip) {
    return (int)std::lround((double)scale(dip) * (double)g_textScale.load());
}

COLORREF toneColor(pxc_stage::Tone t) {
    switch (t) {
    case pxc_stage::Tone::Ok: return RGB(0x7B, 0xD8, 0x8F);
    case pxc_stage::Tone::Warn: return RGB(0xE8, 0xC0, 0x60);
    case pxc_stage::Tone::Fail: return RGB(0xE0, 0x6C, 0x6C);
    case pxc_stage::Tone::Muted: return RGB(0x8A, 0x8F, 0x9A);
    default: return RGB(0xE6, 0xE6, 0xE6);
    }
}

// ---- 布局 ----
struct Layout {
    RECT icon{};
    RECT line1{};
    RECT line2{};
    RECT primary{};
    RECT secondary{};
    RECT tertiary{};
    bool hasSecondary = false;
    bool hasTertiary = false;
    /// 三个动作时按钮不再挤在文字右侧，而是自成一行贴底（见 computeLayout）。
    bool buttonRow = false;
};

// 「这个动作要不要画」的判据只有一处：有动作 + 有文案。
bool hasSecondaryAction() {
    return g_current.secondary != pxc_stage::Action::None &&
           g_current.secondaryLabel && g_current.secondaryLabel[0] != '\0';
}
bool hasTertiaryAction() {
    return g_current.tertiary != pxc_stage::Action::None && g_current.tertiaryLabel &&
           g_current.tertiaryLabel[0] != '\0';
}

void measureBtn(HDC dc, const std::wstring& label, RECT& out, int right, int cy, int h) {
    SIZE sz{};
    HGDIOBJ old = SelectObject(dc, g_fontBtn);
    GetTextExtentPoint32W(dc, label.c_str(), (int)label.size(), &sz);
    SelectObject(dc, old);
    const int w = sz.cx + scale(24);
    out = {right - w, cy - h / 2, right, cy + h / 2};
}

int buttonHeight() { return std::max(scale(26), textPx(13) + scale(12)); }

// 窗口高度按当前字号现算：100% 时 = 16 + 24 + 4 + 44 + 16 = 104，与原来定死的那版逐像素一致。
// 三个按钮时多一整行按钮（+ 间隙 + 按钮高）。
int preferredHeightPx() {
    const int pad = scale(16);
    int h = pad + textPx(24) + scale(4) + textPx(44) + pad;
    if (hasTertiaryAction()) h += scale(10) + buttonHeight();
    return h;
}

Layout computeLayout(HDC dc, int w, int h) {
    Layout L;
    const int pad = scale(16);
    const int iconSize = scale(22);
    // 行高跟着字号走（24 / 13 是 100% 时的原值）：字号放大后行框不跟着长，次行会被压到窗口外。
    const int line1Top = scale(16);
    const int line1H = textPx(24);
    const int btnH = buttonHeight();

    // 图标与主行同中线（原来是 17..39 对 16..40，居中）。
    L.icon = {pad, line1Top + (line1H - iconSize) / 2, pad + iconSize,
              line1Top + (line1H - iconSize) / 2 + iconSize};

    const int textLeft = L.icon.right + scale(12);

    // 三个动作：按钮自成一行贴底、右对齐（从右往左：主 / 次 / 第三）。
    //
    // 为什么不沿用"全部挤在文字右边"：三个按钮（取消 / 开启新进程 / 重试）加起来 ≈205 px，
    // 扣掉图标与按钮后文字只剩两百多像素，长工程名会被截成省略号；更要紧的是
    // 「重试」和「开启新进程」会紧挨着 —— 点错一次就是凭空多一个 PXC 进程，
    // 而这个浮窗存在的意义恰恰是避免多开。所以让它们占满一行、彼此拉开。
    if (hasTertiaryAction()) {
        const int rowTop = h - pad - btnH;
        const int cy = rowTop + btnH / 2;
        int right = w - pad;
        measureBtn(dc, widen(g_current.primaryLabel), L.primary, right, cy, btnH);
        measureBtn(dc, widen(g_current.secondaryLabel), L.secondary, L.primary.left - scale(8),
                   cy, btnH);
        measureBtn(dc, widen(g_current.tertiaryLabel), L.tertiary, L.secondary.left - scale(8), cy,
                   btnH);
        L.hasSecondary = true;
        L.hasTertiary = true;
        L.buttonRow = true;
        L.line1 = {textLeft, line1Top, w - pad, line1Top + line1H};
        L.line2 = {textLeft, line1Top + line1H + scale(4), w - pad, rowTop - scale(10)};
        return L;
    }

    // 按钮从右往左排：主按钮最右，次按钮在它左边。竖向对齐主行（原来 24 + 26/2 = 37）。
    const int cy = line1Top + scale(8) + btnH / 2;
    int right = w - pad;
    std::wstring l1 = widen(g_current.primaryLabel);
    std::wstring l2 = widen(g_current.secondaryLabel);
    measureBtn(dc, l1, L.primary, right, cy, btnH);
    if (hasSecondaryAction()) {
        L.hasSecondary = true;
        measureBtn(dc, l2, L.secondary, L.primary.left - scale(8), cy, btnH);
    }

    const int textRight = (L.hasSecondary ? L.secondary.left : L.primary.left) - scale(12);
    L.line1 = {textLeft, line1Top, textRight, line1Top + line1H};
    L.line2 = {textLeft, line1Top + line1H + scale(4), textRight, h - pad};
    return L;
}

// 宽度按内容算，而不是钉死一个数：
// 中文文案长度差别很大（"正在发送打开快捷键" vs "复用没成功 … 要新开一个 PXC 吗？"），
// 钉死 400 就会在长的那条上折行、把"话框"这种词拆到第二行。
// 上下限：太窄装不下，太宽在右下角占地方。
int preferredWidth(HDC dc) {
    const int pad = scale(16);
    const int iconSize = scale(22);

    int btnW = 0;
    SIZE sz{};
    HGDIOBJ old = SelectObject(dc, g_fontBtn);
    {
        const std::wstring l = widen(g_current.primaryLabel);
        GetTextExtentPoint32W(dc, l.c_str(), (int)l.size(), &sz);
        btnW = sz.cx + scale(24);
        if (hasSecondaryAction()) {
            const std::wstring l2 = widen(g_current.secondaryLabel);
            GetTextExtentPoint32W(dc, l2.c_str(), (int)l2.size(), &sz);
            btnW += sz.cx + scale(24) + scale(8);
        }
        if (hasTertiaryAction()) {
            const std::wstring l3 = widen(g_current.tertiaryLabel);
            GetTextExtentPoint32W(dc, l3.c_str(), (int)l3.size(), &sz);
            btnW += sz.cx + scale(24) + scale(8);
        }
    }

    int textW = 0;
    {
        const std::wstring l2 = widen(g_current.line2);
        GetTextExtentPoint32W(dc, l2.c_str(), (int)l2.size(), &sz);
        textW = sz.cx;
        SelectObject(dc, g_fontMain);
        const std::wstring l1 = widen(g_current.line1);
        GetTextExtentPoint32W(dc, l1.c_str(), (int)l1.size(), &sz);
        if (sz.cx > textW) textW = sz.cx;
    }
    SelectObject(dc, old);

    // 三个动作时按钮自成一行，所以宽度取"文字需要"与"按钮行需要"的较大者；
    // 一两个按钮时仍挤在文字右侧，按两者相加算。
    const int w = hasTertiaryAction()
                      ? std::max(pad + iconSize + scale(12) + textW + pad, pad + btnW + pad)
                      : pad + iconSize + scale(12) + textW + scale(14) + btnW + pad;
    // 上限跟着文本缩放走：字号放大后文字本身就更宽，钉死 560 会一律折成省略号。
    // 下限不跟 —— 那是"别缩成一个窄条"的兜底，放大了页面也不需要更宽的兜底。
    const int maxW = (int)std::lround((double)scale(560) * (double)g_textScale.load());
    return std::clamp(w, scale(400), maxW);
}

// ---- 绘制 ----
// ---- 图标：SVG 光栅化 ----
// 原来是 GDI 的 Ellipse/LineTo 描边画"圆 + 符号"，两个毛病：
// ① GDI 描边不抗锯齿，20 px 的圆圈在 125% / 150% 缩放下毛边明显；
// ② 进行中和已完成都是同一个空心圈，用户看不出"这个圈到底是不是在转"。
// 现在直接解析 SVG 再光栅化（nanosvg 已在工程里），状态靠「环 + 中间符号 + 颜色」区分：
// 进行中 = 白环，完成 = 绿环 + 勾，失败 = 红环 + 叉。
// 24 视箱、半径 9、描边 2.2：留出余量给抗锯齿，缩放到 20~22 px 时不裁边。

std::string iconSvg(pxc_stage::Tone tone) {
    const COLORREF c = (tone == pxc_stage::Tone::Busy) ? RGB(0xFF, 0xFF, 0xFF) : toneColor(tone);
    char col[16] = {0};
    std::snprintf(col, sizeof(col), "#%02X%02X%02X", (int)GetRValue(c), (int)GetGValue(c),
                  (int)GetBValue(c));
    const std::string C(col);

    std::string body;
    switch (tone) {
    case pxc_stage::Tone::Ok:   // 环 + 勾
        body = "<path d=\"M7.8 12.3 L10.7 15.2 L16.4 9.0\" fill=\"none\" stroke=\"" + C +
               "\" stroke-width=\"2.2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>";
        break;
    case pxc_stage::Tone::Fail: // 环 + 叉
        body = "<path d=\"M9.1 9.1 L14.9 14.9\" fill=\"none\" stroke=\"" + C +
               "\" stroke-width=\"2.2\" stroke-linecap=\"round\"/>"
               "<path d=\"M14.9 9.1 L9.1 14.9\" fill=\"none\" stroke=\"" + C +
               "\" stroke-width=\"2.2\" stroke-linecap=\"round\"/>";
        break;
    case pxc_stage::Tone::Warn: // 环 + 感叹号
        body = "<path d=\"M12 7.4 L12 12.6\" fill=\"none\" stroke=\"" + C +
               "\" stroke-width=\"2.2\" stroke-linecap=\"round\"/>"
               "<circle cx=\"12\" cy=\"16\" r=\"1.3\" fill=\"" + C + "\"/>";
        break;
    case pxc_stage::Tone::Muted: // 环 + 横线（已取消 / 无动作）
        body = "<path d=\"M8.6 12 L15.4 12\" fill=\"none\" stroke=\"" + C +
               "\" stroke-width=\"2.2\" stroke-linecap=\"round\"/>";
        break;
    default:                    // 进行中：只有白环
        break;
    }

    return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" "
           "viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"9\" fill=\"none\" "
           "stroke=\"" + C + "\" stroke-width=\"2.2\"/>" + body + "</svg>";
}

// 每个 tone 一份缓存：提示窗重绘不频繁，但 hover 变化也会触发，
// 20 ms 一次的 SVG 解析没必要重来。DPI 变了（窗口被拖到另一块屏）整体重建。
struct IconSlot {
    bool used = false;
    pxc_stage::Tone tone = pxc_stage::Tone::Busy;
    HBITMAP bmp = nullptr;
};
constexpr int kIconSlots = 5;
IconSlot g_icons[kIconSlots];
int g_iconPx = 0;

void clearIcons() {
    for (auto& s : g_icons) {
        if (s.bmp) DeleteObject(s.bmp);
        s = IconSlot{};
    }
    g_iconPx = 0;
}

// SVG -> 32bpp 预乘 ARGB 位图（AlphaBlend 要预乘，nsvg 输出的是非预乘 RGBA，这里顺手换序）。
HBITMAP iconBitmap(pxc_stage::Tone tone, int px) {
    if (px <= 0) return nullptr;
    if (px != g_iconPx) {
        clearIcons();
        g_iconPx = px;
    }
    for (const auto& s : g_icons)
        if (s.used && s.tone == tone) return s.bmp;

    std::string svg = iconSvg(tone);
    std::vector<char> src(svg.begin(), svg.end());
    src.push_back('\0');
    NSVGimage* img = nsvgParse(src.data(), "px", 96.0f);
    if (!img) {
        pxc_log::write("WARN", "Toast", "SVG 解析失败，图标退化为空白");
        return nullptr;
    }

    std::vector<unsigned char> pix((std::size_t)px * (std::size_t)px * 4u, 0u);
    NSVGrasterizer* ras = nsvgCreateRasterizer();
    if (!ras) {
        nsvgDelete(img);
        return nullptr;
    }
    const float sc = std::min((float)px / img->width, (float)px / img->height);
    nsvgRasterize(ras, img, ((float)px - img->width * sc) * 0.5f,
                  ((float)px - img->height * sc) * 0.5f, sc, pix.data(), px, px, px * 4);
    nsvgDeleteRasterizer(ras);
    nsvgDelete(img);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = -px;   // top-down：与 nsvg 的输出行序一致，省一次翻转
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return nullptr;
    }

    unsigned char* d = (unsigned char*)bits;
    for (std::size_t i = 0; i < (std::size_t)px * (std::size_t)px; ++i) {
        const unsigned int a = pix[i * 4 + 3];
        d[i * 4 + 0] = (unsigned char)(pix[i * 4 + 2] * a / 255u);   // B
        d[i * 4 + 1] = (unsigned char)(pix[i * 4 + 1] * a / 255u);   // G
        d[i * 4 + 2] = (unsigned char)(pix[i * 4 + 0] * a / 255u);   // R
        d[i * 4 + 3] = (unsigned char)a;
    }

    for (auto& s : g_icons) {
        if (!s.used) {
            s.used = true;
            s.tone = tone;
            s.bmp = bmp;
            return bmp;
        }
    }
    return bmp;   // 槽满了（理论不会）：不缓存，本次照样能画
}

void drawIcon(HDC dc, const RECT& r, pxc_stage::Tone tone) {
    const int px = r.right - r.left;
    HBITMAP bmp = iconBitmap(tone, px);
    if (!bmp) return;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ ob = SelectObject(mem, bmp);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, r.left, r.top, px, r.bottom - r.top, mem, 0, 0, px, px, bf);
    SelectObject(mem, ob);
    DeleteDC(mem);
}

void drawButton(HDC dc, const RECT& r, const std::wstring& label, bool hot, bool enabled) {
    const COLORREF bg = enabled ? (hot ? RGB(0x48, 0x4E, 0x5E) : RGB(0x3A, 0x3E, 0x4A))
                               : RGB(0x32, 0x35, 0x3E);
    HBRUSH br = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, enabled ? RGB(0x54, 0x5A, 0x6A) : RGB(0x3E, 0x42, 0x4C));
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, scale(6), scale(6));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? RGB(0xDC, 0xDF, 0xE6) : RGB(0x6E, 0x73, 0x7E));
    HGDIOBJ of = SelectObject(dc, g_fontBtn);
    RECT tr = r;
    DrawTextW(dc, label.c_str(), (int)label.size(), &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

// 圆角矩形实心填充（自己按行扫描）。
// 为什么不用 RoundRect：**椭圆宽高 > 0 时它只填到 right-2 / bottom-2**
// （实测 tmp/probe_gdi2.py：RoundRect(0,0,400,104,10,10) 在 x=399、y=103 处仍是底色，没被覆盖；
//  而椭圆宽高 = 0 时走矩形快路径却是填满的 —— 所以早期用 e=0 测"填满"的结论不能套到圆角上）。
// 为补那 1px 把参数传成 w+1/h+1 能补上，但同一次 +1 会把右下两张圆角弧的圆心也推 1px，
// 内圈圆心没动 → 四边厚薄不等（实测过 左2/上2/右3/下3）。FillPath 就更差，恒定缺 1px。
// 这里改由像素中心自行判定：填充范围含最右列/最下行，四个圆心严格对称，环厚处处等于半径差。
void fillRoundRect(HDC dc, HBRUSH br, int x0, int y0, int x1, int y1, int r) {
    if (x1 <= x0 || y1 <= y0) return;
    r = std::min(r, std::min(x1 - x0, y1 - y0) / 2);
    HGDIOBJ old = SelectObject(dc, br);
    for (int y = y0; y < y1; ++y) {
        const double yc = y + 0.5;   // 像素中心
        double cut = 0.0;
        if (yc < y0 + r) {
            const double dy = (y0 + r) - yc;
            cut = r - std::sqrt(std::max(0.0, (double)r * r - dy * dy));
        } else if (yc > y1 - r) {
            const double dy = yc - (y1 - r);
            cut = r - std::sqrt(std::max(0.0, (double)r * r - dy * dy));
        }
        // 按像素中心判定左右两端：x+0.5 落在 [左边界, 右边界] 内才算内部。
        // 两端必须用同一套取整方向（左 ceil、右 floor 各减/加半个像素），
        // 否则左边会漏一个像素、右边多一个像素，圆角看着"一边方一边圆"。
        const int lx = (int)std::ceil(x0 + cut - 0.5);
        const int rx = (int)std::floor(x1 - cut - 0.5) + 1;   // 排他边界
        if (rx <= lx) continue;
        RECT rr{lx, y, rx, y + 1};   // FillRect 填 [left,right-1]×[top,bottom-1]，高度 1 正好一行
        FillRect(dc, &rr, br);
    }
    SelectObject(dc, old);
}

void paint(HWND hwnd, HDC target) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    // 圆角外整块刷成"色键"色（品红），由 LWA_COLORKEY 抠成透明。
    {
        HBRUSH keyBr = CreateSolidBrush(kKeyColor);
        RECT fullRc{0, 0, w, h};
        FillRect(dc, &fullRc, keyBr);
        DeleteObject(keyBr);
    }

    const int e = scale(10);               // 圆角椭圆宽高
    const int bw = std::max(1, scale(3));  // 描边宽度

    // 两层实心圆角：外圈主题色、内圈底色内缩 bw。
    // 内圈的圆角半径必须同步缩成 (外半径 - bw)，这样四个圆心与外圈完全重合，
    // 直边和圆角上的环厚才都恰好是 bw。绘制细节与理由见 fillRoundRect 上方注释。
    {
        HBRUSH accent = CreateSolidBrush((COLORREF)g_accentRgb.load());
        HBRUSH bg = CreateSolidBrush(RGB(0x2D, 0x2F, 0x37));   // 同工作台面板 (45,47,55)
        fillRoundRect(dc, accent, 0, 0, w, h, e / 2);                       // 外圈
        fillRoundRect(dc, bg, bw, bw, w - bw, h - bw, std::max(1, e / 2 - bw));  // 内圈
        DeleteObject(accent);
        DeleteObject(bg);
    }

    const Layout L = computeLayout(dc, w, h);
    drawIcon(dc, L.icon, g_current.tone);

    SetBkMode(dc, TRANSPARENT);

    // 主行
    SetTextColor(dc, toneColor(g_current.tone));
    HGDIOBJ of = SelectObject(dc, g_fontMain);
    RECT r1 = L.line1;
    const std::wstring line1 = widen(g_current.line1);
    DrawTextW(dc, line1.c_str(), (int)line1.size(), &r1,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    // 次行
    SetTextColor(dc, RGB(0xA8, 0xAE, 0xBC));
    SelectObject(dc, g_fontSub);
    RECT r2 = L.line2;
    const std::wstring line2 = widen(g_current.line2);
    DrawTextW(dc, line2.c_str(), (int)line2.size(), &r2,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);

    // 按钮：cancelable=false 时置灰（位置不动，避免宽度跳变）
    const bool primaryEnabled = !(g_current.primary == pxc_stage::Action::Cancel && !g_current.cancelable);
    drawButton(dc, L.primary, widen(g_current.primaryLabel),
               g_hoverAction == g_current.primary, primaryEnabled);
    if (L.hasSecondary) {
        drawButton(dc, L.secondary, widen(g_current.secondaryLabel),
                   g_hoverAction == g_current.secondary, true);
    }
    if (L.hasTertiary) {
        drawButton(dc, L.tertiary, widen(g_current.tertiaryLabel),
                   g_hoverAction == g_current.tertiary, true);
    }
    BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

RECT workArea() {
    RECT wa{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    return wa;
}

// 停靠位：右下角，避开任务栏。宽度每次现取 —— 内容变了宽度就会变，右对齐的 x 跟着变。
void restPos(HWND hwnd, int& x, int& y) {
    const RECT wa = workArea();
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    const int margin = scale(16);
    x = wa.right - w - margin;
    y = wa.bottom - h - margin;
}

void placeWindow(HWND hwnd) {
    int x = 0, y = 0;
    restPos(hwnd, x, y);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// 屏外起点：整块贴在屏幕右缘之外（左边缘 = 工作区右边界），完全看不见。
int offscreenX() { return workArea().right; }

void moveTo(HWND hwnd, int x, int y) {
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// 开始滑入。调用前窗口应已摆到屏外、且已 Show —— 否则第一帧会闪在错误位置。
void beginSlide(HWND hwnd) {
    g_sliding = true;
    g_slideStart = GetTickCount64();
    SetTimer(hwnd, kSlideTimer, 16, nullptr);
}

void tickSlide(HWND hwnd) {
    int tx = 0, ty = 0;
    restPos(hwnd, tx, ty);
    // 目标每帧现算：滑入途中内容可能又变了一次（宽度变 → 右对齐的 x 变），
    // 按最新目标重插值就不会跳一下。
    const int sx = offscreenX();
    const double t = std::min(1.0, (double)(GetTickCount64() - g_slideStart) / (double)kSlideMs);
    const double eased = 1.0 - std::pow(1.0 - t, 3.0);   // easeOutCubic：快进慢停
    moveTo(hwnd, sx + (int)std::lround((tx - sx) * eased), ty);
    if (t >= 1.0) {
        KillTimer(hwnd, kSlideTimer);
        g_sliding = false;
        moveTo(hwnd, tx, ty);   // 收尾对齐，避免插值取整差 1px
    }
}

pxc_stage::Action hitTest(POINT pt, const RECT& client) {
    HDC dc = GetDC(g_hwnd);
    const Layout L = computeLayout(dc, client.right, client.bottom);
    ReleaseDC(g_hwnd, dc);
    if (PtInRect(&L.primary, pt)) {
        // 取消不可点时按钮置灰，点击不产生动作（位置不动，避免宽度跳变）。
        if (g_current.primary == pxc_stage::Action::Cancel && !g_current.cancelable)
            return pxc_stage::Action::None;
        return g_current.primary;
    }
    if (L.hasSecondary && PtInRect(&L.secondary, pt)) return g_current.secondary;
    if (L.hasTertiary && PtInRect(&L.tertiary, pt)) return g_current.tertiary;
    return pxc_stage::Action::None;
}

void rebuildFonts() {
    if (g_fontMain) DeleteObject(g_fontMain);
    if (g_fontSub) DeleteObject(g_fontSub);
    if (g_fontBtn) DeleteObject(g_fontBtn);
    // 字体名走 ADD 进来的私有字体族；找不到会自动回落到系统字体，不至于画不出字。
    // 字号用 textPx（DPI × 文本缩放）：提示窗不在 EUI 的 text pipeline 里，
    // 设置页的 fontSize 系数对它无效，只能在这里自己乘（docs/05 §9.7）。
    const wchar_t* family = L"Noto Sans SC Medium";
    g_fontMain = CreateFontW(-textPx(16), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
    g_fontSub = CreateFontW(-textPx(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
    g_fontBtn = CreateFontW(-textPx(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
}

void loadFont() {
    if (g_fontDir.empty()) return;
    const std::wstring path = g_fontDir + L"\\NotoSansSC-Medium.ttf";
    // FR_PRIVATE：只对本进程有效，不污染系统字体列表。
    if (AddFontResourceExW(path.c_str(), FR_PRIVATE, 0) == 0) {
        pxc_log::write("WARN", "Toast", "提示窗字体挂载失败，回落系统字体");
    }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TOAST_REFRESH: {
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_current = g_text;   // 拷一份：之后整帧只读副本，不再碰受锁保护的 g_text
            g_dirty = false;
        }
        // DPI 可能在两次提示之间变过（窗口被拖到另一块屏），字号要跟着重算。
        const UINT dpi = GetDpiForWindow(hwnd);
        g_dpi = dpi ? (int)dpi : 96;
        rebuildFonts();
        HDC measureDc = GetDC(hwnd);
        const int wantW = preferredWidth(measureDc);
        ReleaseDC(hwnd, measureDc);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, wantW, preferredHeightPx(),
                     SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        // 只在"隐藏 → 显示"时滑入；已经在屏上（等窗口时每秒刷"已 N 秒"）就只重新贴右下角。
        const bool wasVisible = g_visible.load();
        if (!wasVisible) {
            // 先摆到屏外再 Show：反过来的话第一帧会先闪在右下角，然后才"跳"出去滑进来。
            int ty = 0, tx = 0;
            restPos(hwnd, tx, ty);
            moveTo(hwnd, offscreenX(), ty);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            beginSlide(hwnd);
        } else if (!g_sliding) {
            placeWindow(hwnd);
        }
        g_visible = true;
        if (g_current.autoCloseMs > 0)
            SetTimer(hwnd, kAutoCloseTimer, (UINT)g_current.autoCloseMs, nullptr);
        else
            KillTimer(hwnd, kAutoCloseTimer);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_TOAST_HIDE:
        KillTimer(hwnd, kAutoCloseTimer);
        KillTimer(hwnd, kSlideTimer);
        g_sliding = false;
        ShowWindow(hwnd, SW_HIDE);
        g_visible = false;
        return 0;
    case WM_TIMER:
        if (wp == kSlideTimer) {
            tickSlide(hwnd);
            return 0;
        }
        if (wp == kAutoCloseTimer) {
            KillTimer(hwnd, kAutoCloseTimer);
            KillTimer(hwnd, kSlideTimer);
            g_sliding = false;
            ShowWindow(hwnd, SW_HIDE);
            g_visible = false;
        }
        return 0;
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT cr{};
        GetClientRect(hwnd, &cr);
        const pxc_stage::Action hit = hitTest(pt, cr);
        if (hit != g_hoverAction) {
            g_hoverAction = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hoverAction != pxc_stage::Action::None) {
            g_hoverAction = pxc_stage::Action::None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT cr{};
        GetClientRect(hwnd, &cr);
        const pxc_stage::Action hit = hitTest(pt, cr);
        if (hit != pxc_stage::Action::None) {
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_action = hit;
            }
            if (hit == pxc_stage::Action::Close || hit == pxc_stage::Action::Cancel) {
                KillTimer(hwnd, kAutoCloseTimer);
                KillTimer(hwnd, kSlideTimer);
                g_sliding = false;
                ShowWindow(hwnd, SW_HIDE);
                g_visible = false;
            }
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        paint(hwnd, ps.hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   // 全窗自绘，不要系统刷底（会闪）
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void threadMain() {
    loadFont();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // WS_POPUP = 无标题栏无边框；EX 三件套：置顶 / 不激活 / 不占 Alt+Tab 与任务栏。
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                kClassName, L"PXC 启动", WS_POPUP, 0, 0, scale(kWidthDip),
                                preferredHeightPx(),
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        pxc_log::write("ERROR", "Toast", "提示窗创建失败，错误码=" + std::to_string(GetLastError()));
        SetEvent(g_readyEvent);
        return;
    }
    // 圆角之外刷成色键色，由 LWA_COLORKEY 抠成透明（二值化足够：圆角边缘不需要半透明）。
    SetLayeredWindowAttributes(hwnd, kKeyColor, 0, LWA_COLORKEY);
    g_hwnd = hwnd;
    g_dpi = 96;
    rebuildFonts();
    // 先离屏定位，避免出现在 (0,0) 闪一下。
    SetWindowPos(hwnd, HWND_TOPMOST, -32000, -32000, scale(kWidthDip), preferredHeightPx(),
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetEvent(g_readyEvent);

    MSG msg{};
    while (!g_quit && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_fontMain) DeleteObject(g_fontMain);
    if (g_fontSub) DeleteObject(g_fontSub);
    if (g_fontBtn) DeleteObject(g_fontBtn);
    g_fontMain = g_fontSub = g_fontBtn = nullptr;
    clearIcons();
}

} // namespace

bool start(const std::wstring& fontDir) {
    if (g_thread.joinable()) return true;
    g_fontDir = fontDir;
    g_quit = false;
    g_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = std::thread(threadMain);
    // 等窗口建好（正常 <50 ms）。超时也不阻塞调用方 —— show() 会补发刷新。
    WaitForSingleObject(g_readyEvent, 1500);
    return g_hwnd != nullptr;
}

void stop() {
    if (!g_thread.joinable()) return;
    g_quit = true;
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    g_thread.join();
    if (g_readyEvent) {
        CloseHandle(g_readyEvent);
        g_readyEvent = nullptr;
    }
    g_hwnd = nullptr;
    g_visible = false;
}

void show(const pxc_stage::Text& text) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_text = text;
        g_dirty = true;
    }
    if (g_hwnd) PostMessageW(g_hwnd, WM_TOAST_REFRESH, 0, 0);
}

void hide() {
    if (g_hwnd) PostMessageW(g_hwnd, WM_TOAST_HIDE, 0, 0);
}

void setTextScale(float s) {
    // 限幅只防配置手改出离谱值；正常路径来的就是设置页那 10 档。
    if (!(s > 0.0f)) s = 1.0f;
    g_textScale.store(std::clamp(s, 0.7f, 2.0f));
}

void setAccent(unsigned int rgb) {
    if (g_accentRgb.exchange(rgb) == rgb) return;
    // 窗口在屏上就立刻重画（换主题色时可能正开着一条提示）。跨线程不能直接调 paint()，
    // 投递一条刷新消息给提示窗线程即可。
    if (g_hwnd && g_visible.load()) PostMessageW(g_hwnd, WM_TOAST_REFRESH, 0, 0);
}

void setPendingForTest(const pxc_stage::Text& text) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_text = text;
}

pxc_stage::Action takeAction() {
    std::lock_guard<std::mutex> lk(g_mtx);
    const pxc_stage::Action a = g_action;
    g_action = pxc_stage::Action::None;
    return a;
}

bool visible() { return g_visible.load(); }

} // namespace pxc_toast
