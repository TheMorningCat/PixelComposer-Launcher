// Modified from EUI-NEO by PXCLauncher (2026-11). See MODIFICATIONS.md in this directory.

#pragma once

namespace core::window {

using Handle = void*;
using ContextKey = void*;
using CursorHandle = void*;

enum class CursorType {
    Arrow,
    Hand
};

enum class RenderApi {
    OpenGL,
    Vulkan
};

struct WindowCreateRequest {
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    bool positionSet = false;
    int minWidth = 0;
    int minHeight = 0;
    int maxWidth = 0;
    int maxHeight = 0;
    const char* title = "";
    bool resizable = true;
    bool highDpi = true;
    bool decorated = true;
    /// 建窗时是否立即显示。false = 窗口存在但不可见（`GLFW_VISIBLE=false`），
    /// 由应用自己决定何时 show。用途：需要"先建窗、首帧后才露脸"的场合
    /// （例如启动器双击 .pxc 的静默启动，否则 GLFW 会在首帧 show 出整块窗口，
    /// 而我们来不及在第一句 compose 里藏起来）。默认 true，行为不变。
    bool visible = true;
    bool alwaysOnTop = false;
    bool maximized = false;
    bool modal = false;
    Handle parent = nullptr;
    RenderApi renderApi = RenderApi::OpenGL;
};

struct NativeWindowInfo {
    Handle handle = nullptr;
    void* platformWindow = nullptr;
    void* platformDisplay = nullptr;
    void* platformView = nullptr;
};

} // namespace core::window
