#pragma once
// ---------------------------------------------------------------------------
// App.h - GUI 应用入口与主窗口创建
// ---------------------------------------------------------------------------
#include <windows.h>

namespace dirmv::gui
{

    // 运行 GUI 消息循环，返回进程退出码
    int RunApp(HINSTANCE hInstance, int nCmdShow);

    // 主窗口句柄（供其他模块使用）
    extern HWND g_mainWnd;

} // namespace dirmv::gui
