#include "gui/TrayIcon.h"
#include "gui/MainWindow.h"
#include "resource.h"

#include <shellapi.h>

namespace dirmv::gui
{

    static NOTIFYICONDATAW g_nid{};
    static bool g_added = false;

    bool TrayIcon::Add(HWND hwnd, HINSTANCE hInst, const std::wstring &tip)
    {
        g_nid = {};
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
                                        IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
        if (!g_nid.hIcon)
            g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
        g_added = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
        return g_added;
    }

    void TrayIcon::Remove(HWND hwnd)
    {
        if (g_added)
        {
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            g_added = false;
        }
        (void)hwnd;
    }

    void TrayIcon::SetTip(HWND hwnd, const std::wstring &tip)
    {
        if (!g_added)
            return;
        wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
        g_nid.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        (void)hwnd;
    }

    void TrayIcon::Notify(HWND hwnd, const std::wstring &title, const std::wstring &msg)
    {
        if (!g_added)
            return;
        NOTIFYICONDATAW nid = g_nid;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(nid.szInfo, msg.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        (void)hwnd;
    }

    bool TrayIcon::HandleMessage(HWND hwnd, WPARAM wParam, LPARAM lParam)
    {
        if (wParam != 1)
            return false;
        UINT ev = LOWORD(lParam);
        if (ev == WM_LBUTTONDBLCLK)
        {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            return true;
        }
        if (ev == WM_RBUTTONUP)
        {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDM_SHOW, L"显示主窗口(&S)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_MIGRATE, L"立即迁移(&M)");
            AppendMenuW(menu, MF_STRING, IDM_RESTORE, L"恢复(&R)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出(&X)");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            return true;
        }
        return false;
    }

} // namespace dirmv::gui
