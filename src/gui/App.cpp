#include "gui/App.h"
#include "gui/MainWindow.h"
#include "gui/ProgressDialog.h"
#include "gui/TrayIcon.h"
#include "core/Log.h"
#include "core/PathUtil.h"

#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace dirmv::gui
{

    HWND g_mainWnd = nullptr;

    int RunApp(HINSTANCE hInstance, int nCmdShow)
    {
        // 启用视觉样式与通用控件
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);

        // 日志文件
        std::wstring logPath = Path::Combine(Path::ExeDir(), L"dirmv.log");
        Logger::Instance().SetFile(logPath);
        Logger::Instance().Info(L"GUI 启动");

        MainWindow::Register(hInstance);
        ProgressDialog::Register(hInstance);

        HWND hwnd = MainWindow::Create(hInstance, nCmdShow);
        if (!hwnd)
        {
            MessageBoxW(nullptr, L"创建主窗口失败", L"dirMover", MB_ICONERROR);
            return 1;
        }
        g_mainWnd = hwnd;

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(hwnd, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        return (int)msg.wParam;
    }

} // namespace dirmv::gui
