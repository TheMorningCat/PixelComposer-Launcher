// Modified from EUI-NEO by PXCLauncher (2026-11). See MODIFICATIONS.md in this directory.

#pragma once

#include "eui/app.h"
#include "eui/async.h"

#include <cmath>
#include <functional>
#include <string>
#include <utility>

namespace app {

struct DslAppConfig {
    std::string titleValue = "App";
    std::string pageIdValue = "app";
    eui::Color clearColorValue = {0.16f, 0.18f, 0.20f, 1.0f};
    int windowWidthValue = 800;
    int windowHeightValue = 600;
    int windowXValue = 0;
    int windowYValue = 0;
    bool windowPositionSetValue = false;
    int minWindowWidthValue = 0;
    int minWindowHeightValue = 0;
    int maxWindowWidthValue = 0;
    int maxWindowHeightValue = 0;
    bool resizableValue = true;
    bool highDpiValue = true;
    bool decoratedValue = true;
    /// 建窗时是否立即显示（`GLFW_VISIBLE`）。false 时窗口存在但不可见，由应用自己 show。
    /// 仅用于"必须等首帧准备好才露脸"的场景，默认 true。
    bool visibleValue = true;
    bool alwaysOnTopValue = false;
    bool maximizedValue = false;
    float uiScaleValue = 1.0f;
    /// 全局文本缩放（只乘字号与行高）。运行期请用 `app::setTextScale()`，它会顺带重排。
    float textScaleValue = 1.0f;
#if defined(EUI_DEBUG_BUILD)
    bool showDebugStatsInTitleValue = true;
    bool showDebugOverlayValue = true;
#else
    bool showDebugStatsInTitleValue = false;
    bool showDebugOverlayValue = false;
#endif
    double debugTitleIntervalValue = 1.0;
    std::function<void(eui::Ui&, const eui::Screen&)> debugOverlayCompose;
    double fpsValue = 90.0;
    std::string iconPathValue = "assets/icon.png";
    std::string textFontFileValue;
    std::string iconFontFileValue;
    bool trayEnabledValue = false;
    std::string trayTitleValue;
    std::string trayIconPathValue;
    std::function<void(const eui::KeyEvent&)> keyEventHandler;
    std::function<void()> shutdownHandler;

    DslAppConfig& title(std::string value) { titleValue = std::move(value); return *this; }
    DslAppConfig& pageId(std::string value) { pageIdValue = std::move(value); return *this; }
    DslAppConfig& clearColor(const eui::Color& value) { clearColorValue = value; return *this; }
    DslAppConfig& background(const eui::Color& value) { return clearColor(value); }
    DslAppConfig& windowSize(int width, int height) {
        windowWidthValue = width;
        windowHeightValue = height;
        return *this;
    }
    DslAppConfig& windowWidth(int value) { windowWidthValue = value; return *this; }
    DslAppConfig& windowHeight(int value) { windowHeightValue = value; return *this; }
    DslAppConfig& windowPosition(int x, int y) {
        windowXValue = x;
        windowYValue = y;
        windowPositionSetValue = true;
        return *this;
    }
    DslAppConfig& centerWindow() {
        windowPositionSetValue = false;
        return *this;
    }
    DslAppConfig& minWindowSize(int width, int height) {
        minWindowWidthValue = width;
        minWindowHeightValue = height;
        return *this;
    }
    DslAppConfig& maxWindowSize(int width, int height) {
        maxWindowWidthValue = width;
        maxWindowHeightValue = height;
        return *this;
    }
    DslAppConfig& resizable(bool value = true) { resizableValue = value; return *this; }
    DslAppConfig& highDpi(bool value = true) { highDpiValue = value; return *this; }
    DslAppConfig& decorated(bool value = true) { decoratedValue = value; return *this; }
    /// 建窗时是否立即显示。false 时窗口存在但不可见（`GLFW_VISIBLE=false`），需应用自己 show。
    DslAppConfig& visible(bool value = true) { visibleValue = value; return *this; }
    DslAppConfig& alwaysOnTop(bool value = true) { alwaysOnTopValue = value; return *this; }
    DslAppConfig& maximized(bool value = true) { maximizedValue = value; return *this; }
    DslAppConfig& uiScale(float value) {
        uiScaleValue = value > 0.0f ? value : 1.0f;
        return *this;
    }
    DslAppConfig& textScale(float value) {
        textScaleValue = value > 0.0f ? value : 1.0f;
        return *this;
    }
    DslAppConfig& showDebugStatsInTitle(bool value = true) {
        showDebugStatsInTitleValue = value;
        return *this;
    }
    DslAppConfig& debugTitleInterval(double value) {
        debugTitleIntervalValue = std::isfinite(value) && value > 0.0 ? value : 1.0;
        return *this;
    }
    DslAppConfig& showDebugOverlay(bool value = true) {
        showDebugOverlayValue = value;
        return *this;
    }
    DslAppConfig& onDebugOverlay(std::function<void(eui::Ui&, const eui::Screen&)> callback) {
        debugOverlayCompose = std::move(callback);
        return *this;
    }
    DslAppConfig& fps(double value) { fpsValue = value; return *this; }
    DslAppConfig& iconPath(std::string value) { iconPathValue = std::move(value); return *this; }
    DslAppConfig& textFont(std::string value) { textFontFileValue = std::move(value); return *this; }
    DslAppConfig& iconFont(std::string value) { iconFontFileValue = std::move(value); return *this; }
    DslAppConfig& fonts(std::string textFont, std::string iconFont = {}) {
        textFontFileValue = std::move(textFont);
        iconFontFileValue = std::move(iconFont);
        return *this;
    }
    DslAppConfig& tray(bool value = true) {
        trayEnabledValue = value;
        return *this;
    }
    DslAppConfig& trayTitle(std::string value) {
        trayTitleValue = std::move(value);
        return *this;
    }
    DslAppConfig& trayIcon(std::string value) {
        trayIconPathValue = std::move(value);
        return *this;
    }
    DslAppConfig& onKeyEvent(std::function<void(const eui::KeyEvent&)> handler) {
        keyEventHandler = std::move(handler);
        return *this;
    }
    /** @brief UI/渲染线程退出回调，在主窗口 GPU 设备销毁前释放应用资源引用。 */
    DslAppConfig& onShutdown(std::function<void()> handler) {
        shutdownHandler = std::move(handler);
        return *this;
    }
};

struct DslWindowConfig {
    std::string titleValue = "Window";
    std::string pageIdValue = "window";
    eui::Color clearColorValue = {0.16f, 0.18f, 0.20f, 1.0f};
    int windowWidthValue = 640;
    int windowHeightValue = 420;
    bool modalValue = false;
    std::function<void(const eui::KeyEvent&)> keyEventHandler;

    DslWindowConfig& title(std::string value) { titleValue = std::move(value); return *this; }
    DslWindowConfig& pageId(std::string value) { pageIdValue = std::move(value); return *this; }
    DslWindowConfig& clearColor(const eui::Color& value) { clearColorValue = value; return *this; }
    DslWindowConfig& background(const eui::Color& value) { return clearColor(value); }
    DslWindowConfig& windowSize(int width, int height) {
        windowWidthValue = width;
        windowHeightValue = height;
        return *this;
    }
    DslWindowConfig& windowWidth(int value) { windowWidthValue = value; return *this; }
    DslWindowConfig& windowHeight(int value) { windowHeightValue = value; return *this; }
    DslWindowConfig& modal(bool value = true) { modalValue = value; return *this; }
    DslWindowConfig& onKeyEvent(std::function<void(const eui::KeyEvent&)> handler) {
        keyEventHandler = std::move(handler);
        return *this;
    }
};

const DslAppConfig& dslAppConfig();
/// 可写视图：**只**用于运行期改缩放类字段（uiScale），改完下一帧 logicalWidth 就变、框架自动重排。
/// 其它字段（窗口尺寸/位置/标题）只在建窗时读一次，改这里没有效果 —— 别用它当"配置中心"。
DslAppConfig& mutableDslAppConfig();
void compose(eui::Ui& ui, const eui::Screen& screen);

void openWindow(const DslWindowConfig& config, DslWindowCompose composeFn);
void openWindow(const char* title, int width, int height, DslWindowCompose composeFn);

} // namespace app
