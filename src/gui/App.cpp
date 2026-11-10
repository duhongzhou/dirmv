#include "gui/App.h"
#include "gui/MainWindow.h"
#include "gui/ProgressDialog.h"
#include "gui/Theme.h"
#include "gui/TrayIcon.h"
#include "core/Log.h"
#include "core/PathUtil.h"

#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")
// 启用 Common-Controls v6 视觉样式（按钮/列表/标题栏现代化，不再是 Win95 外观）
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace dirmv::gui
{

    HWND g_mainWnd = nullptr;

    // 系统 DPI 感知：文字清晰不模糊（125%/150% 缩放下尤为重要）
    static void EnableDpiAwareness()
    {
        // System DPI aware：与 96 DPI 布局按比例缩放，无需处理逐显示器缩放迁移
        using SetCtxFn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32)
        {
            auto setCtx = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
            if (setCtx && setCtx(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
                return;
        }
        SetProcessDPIAware();
    }

    int RunApp(HINSTANCE hInstance, int nCmdShow)
    {
        EnableDpiAwareness();

        // 启用视觉样式与通用控件
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);

        theme::Init(hInstance);
        theme::RegisterControls(hInstance);

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
