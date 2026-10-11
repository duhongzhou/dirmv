#pragma once
// ---------------------------------------------------------------------------
// TrayIcon.h - 系统托盘图标与右键菜单（最小化到托盘）
// ---------------------------------------------------------------------------
#include <windows.h>
#include <string>

namespace dirmv::gui
{

    class TrayIcon
    {
    public:
        static bool Add(HWND hwnd, HINSTANCE hInst, const std::wstring &tip);
        static void Remove(HWND hwnd);
        static void SetTip(HWND hwnd, const std::wstring &tip);
        // 显示气泡通知
        static void Notify(HWND hwnd, const std::wstring &title, const std::wstring &msg);

        static constexpr UINT WM_TRAYICON = WM_APP + 100;

        enum : int
        {
            IDM_SHOW = 3001,
            IDM_MIGRATE = 3002,
            IDM_RESTORE = 3003,
            IDM_EXIT = 3004,
        };

        // 处理托盘消息；返回 true 表示已处理
        static bool HandleMessage(HWND hwnd, WPARAM wParam, LPARAM lParam);
    };

} // namespace dirmv::gui
