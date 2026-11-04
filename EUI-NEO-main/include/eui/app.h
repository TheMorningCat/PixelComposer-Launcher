// Modified from EUI-NEO by PXCLauncher (2026-11). See MODIFICATIONS.md in this directory.

#pragma once

#include "eui/dsl.h"
#include "eui/types.h"
#include "eui/window.h"

#include <functional>
#include <string>
#include <vector>

namespace app {

using DslWindowCompose = std::function<void(eui::Ui&, const eui::Screen&)>;

struct DslWindowRequest {
    std::string title = "Window";
    std::string pageId = "window";
    eui::Color clearColor = {0.16f, 0.18f, 0.20f, 1.0f};
    int width = 640;
    int height = 420;
    bool modal = false;
    std::function<void(const eui::KeyEvent&)> onKeyEvent;
    DslWindowCompose compose;
};

const char* windowTitle();
bool showDebugStatsInTitle();
double debugTitleUpdateInterval();
bool showDebugOverlay();
double frameRateLimit();
int initialWindowWidth();
int initialWindowHeight();
int initialWindowX();
int initialWindowY();
bool initialWindowPositionSet();
/// 建窗时是否立即显示（`GLFW_VISIBLE`）。false = 窗口存在但不可见，由应用自己 show。
bool initialWindowVisible();
int minimumWindowWidth();
int minimumWindowHeight();
int maximumWindowWidth();
int maximumWindowHeight();
bool windowResizable();
bool windowHighDpi();
bool windowDecorated();
bool windowAlwaysOnTop();
bool windowMaximized();
float uiScale();
/// 全局文本缩放系数（只影响字号与行高，不影响布局尺寸）。见 docs/06 §10.4.2。
float textScale();
bool trayEnabled();
const char* trayTitle();
const char* trayIconPath();
void requestUpdate();
/// 强制下一帧重新 compose。
/// 缩放类设置（文本缩放）改完必须调：元素上的字号是在 compose 里算好的，不重排就一直用旧的。
void requestCompose();
/// 设置全局文本缩放（离散档）。改完自动重排 —— 见 docs/06 §10.4.2.1 为什么不能连续。
void setTextScale(float scale);
bool initialize(eui::window::Handle window);
bool update(eui::window::Handle window, float deltaSeconds, int windowWidth, int windowHeight, float dpiScale, float pointerScale);
bool update(eui::window::Handle window, float deltaSeconds, int windowWidth, int windowHeight, float dpiScale, float pointerScale, bool updateRequested);
bool update(eui::window::Handle window, float deltaSeconds, int windowWidth, int windowHeight, float dpiScale, float pointerScale, bool updateRequested, bool inputEnabled);
bool isAnimating();
void render(int windowWidth, int windowHeight, float dpiScale);
void releaseGraphicsResources();
void shutdown();
std::vector<DslWindowRequest> consumeWindowRequests();

namespace detail {
void requestFullPaint();
}

} // namespace app
