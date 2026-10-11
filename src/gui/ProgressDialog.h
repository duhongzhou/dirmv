#pragma once
// ---------------------------------------------------------------------------
// ProgressDialog.h - 迁移进度对话框（进度条 + 预计完成时间 + 当前文件）
// ---------------------------------------------------------------------------
#include <windows.h>
#include <string>
#include "core/MigrateEngine.h"

namespace dirmv::gui
{

    class ProgressDialog
    {
    public:
        static constexpr int IDD = 2001;

        enum : int
        {
            IDC_PD_BAR = 2101,
            IDC_PD_TEXT = 2102,
            IDC_PD_ETA = 2103,
            IDC_PD_SPEED = 2104,
            IDC_PD_CANCEL = 2105,
            IDC_PD_PHASE = 2106,
        };

        static void Register(HINSTANCE hInst);
        // 创建并显示；返回窗口句柄
        static HWND Show(HWND parent, const std::wstring &title, bool cancellable);

        // 更新进度（线程安全，内部 PostMessage）
        static void Update(HWND dlg, const ProgressInfo &info);
        // 关闭
        static void Close(HWND dlg);
        // 是否用户点击了取消
        static bool WasCancelled(HWND dlg);

        static constexpr UINT WM_APP_PD_UPDATE = WM_APP + 20;
    };

} // namespace dirmv::gui
