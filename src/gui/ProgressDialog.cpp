#include "gui/ProgressDialog.h"
#include "core/PathUtil.h"

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <atomic>

namespace dirmv::gui
{

    namespace
    {
        std::atomic<bool> g_cancelled{false};
        HFONT g_pdFont = nullptr;

        struct PdUpdate
        {
            ProgressInfo info;
        };
    }

    LRESULT CALLBACK ProgressWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            g_cancelled = false;

            CreateWindowExW(0, L"STATIC", L"阶段：", WS_CHILD | WS_VISIBLE,
                            12, 10, 60, 20, hwnd, (HMENU)(INT_PTR)ProgressDialog::IDC_PD_PHASE,
                            nullptr, nullptr);
            HWND phase = CreateWindowExW(0, L"STATIC", L"准备中", WS_CHILD | WS_VISIBLE,
                                         72, 10, 380, 20, hwnd, nullptr, nullptr, nullptr);

            HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                       WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                       12, 38, 440, 24,
                                       hwnd, (HMENU)(INT_PTR)ProgressDialog::IDC_PD_BAR, nullptr, nullptr);
            SendMessageW(bar, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
            SendMessageW(bar, PBM_SETPOS, 0, 0);

            HWND text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        12, 70, 440, 36, hwnd,
                                        (HMENU)(INT_PTR)ProgressDialog::IDC_PD_TEXT, nullptr, nullptr);

            HWND eta = CreateWindowExW(0, L"STATIC", L"预计剩余：--", WS_CHILD | WS_VISIBLE,
                                       12, 110, 220, 20, hwnd,
                                       (HMENU)(INT_PTR)ProgressDialog::IDC_PD_ETA, nullptr, nullptr);

            HWND speed = CreateWindowExW(0, L"STATIC", L"速度：--", WS_CHILD | WS_VISIBLE,
                                         240, 110, 212, 20, hwnd,
                                         (HMENU)(INT_PTR)ProgressDialog::IDC_PD_SPEED, nullptr, nullptr);

            HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          380, 138, 72, 28, hwnd,
                                          (HMENU)(INT_PTR)ProgressDialog::IDC_PD_CANCEL, nullptr, nullptr);

            NONCLIENTMETRICSW ncm{};
            ncm.cbSize = sizeof(ncm);
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            g_pdFont = CreateFontIndirectW(&ncm.lfMessageFont);

            HWND ctrls[] = {phase, text, eta, speed, cancel,
                            GetDlgItem(hwnd, ProgressDialog::IDC_PD_PHASE)};
            for (HWND c : ctrls)
                if (c)
                    SendMessageW(c, WM_SETFONT, (WPARAM)g_pdFont, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == ProgressDialog::IDC_PD_CANCEL)
            {
                g_cancelled = true;
                EnableWindow(GetDlgItem(hwnd, ProgressDialog::IDC_PD_CANCEL), FALSE);
                SetDlgItemTextW(hwnd, ProgressDialog::IDC_PD_PHASE, L"正在取消…");
            }
            return 0;
        case ProgressDialog::WM_APP_PD_UPDATE:
        {
            auto *info = reinterpret_cast<ProgressInfo *>(lParam);
            if (info)
            {
                SendMessageW(GetDlgItem(hwnd, ProgressDialog::IDC_PD_BAR),
                             PBM_SETPOS, (WPARAM)(info->percent * 10.0), 0);
                SetDlgItemTextW(hwnd, ProgressDialog::IDC_PD_PHASE, info->phase.c_str());

                wchar_t buf[512];
                swprintf_s(buf, L"%d / %d  已传输 %s / %s\n当前：%s",
                           info->doneItems, info->totalItems,
                           Path::FormatBytes(info->doneBytes).c_str(),
                           Path::FormatBytes(info->totalBytes).c_str(),
                           info->currentItem.c_str());
                SetDlgItemTextW(hwnd, ProgressDialog::IDC_PD_TEXT, buf);

                if (info->etaSeconds >= 0)
                {
                    int sec = (int)info->etaSeconds;
                    swprintf_s(buf, L"预计剩余：%d:%02d:%02d  (%.1f%%)",
                               sec / 3600, (sec % 3600) / 60, sec % 60, info->percent);
                }
                else
                {
                    swprintf_s(buf, L"预计剩余：计算中…  (%.1f%%)", info->percent);
                }
                SetDlgItemTextW(hwnd, ProgressDialog::IDC_PD_ETA, buf);

                swprintf_s(buf, L"速度：%s/s", Path::FormatBytes((uint64_t)info->speedBps).c_str());
                SetDlgItemTextW(hwnd, ProgressDialog::IDC_PD_SPEED, buf);

                delete info;
            }
            return 0;
        }
        case WM_CLOSE:
            g_cancelled = true;
            return 0;
        case WM_DESTROY:
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void ProgressDialog::Register(HINSTANCE hInst)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ProgressWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"dirMoverProgressWnd";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassExW(&wc);
    }

    HWND ProgressDialog::Show(HWND parent, const std::wstring &title, bool cancellable)
    {
        HWND dlg = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            L"dirMoverProgressWnd", title.c_str(),
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, 480, 215,
            parent, nullptr, nullptr, nullptr);
        if (!dlg)
            return nullptr;
        if (!cancellable)
        {
            EnableWindow(GetDlgItem(dlg, IDC_PD_CANCEL), FALSE);
        }
        // 居中于父窗口
        RECT r, pr;
        GetWindowRect(dlg, &r);
        GetWindowRect(parent, &pr);
        int x = pr.left + ((pr.right - pr.left) - (r.right - r.left)) / 2;
        int y = pr.top + ((pr.bottom - pr.top) - (r.bottom - r.top)) / 2;
        SetWindowPos(dlg, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);
        UpdateWindow(dlg);
        return dlg;
    }

    void ProgressDialog::Update(HWND dlg, const ProgressInfo &info)
    {
        if (!dlg)
            return;
        auto *copy = new ProgressInfo(info);
        if (!PostMessageW(dlg, WM_APP_PD_UPDATE, 0, (LPARAM)copy))
        {
            delete copy;
        }
    }

    void ProgressDialog::Close(HWND dlg)
    {
        if (dlg)
            DestroyWindow(dlg);
    }

    bool ProgressDialog::WasCancelled(HWND dlg)
    {
        (void)dlg;
        return g_cancelled.load();
    }

} // namespace dirmv::gui
