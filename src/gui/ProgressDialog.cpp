#include "gui/ProgressDialog.h"
#include "gui/Theme.h"
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
        HWND g_pdPhaseVal = nullptr;
    }

    LRESULT CALLBACK ProgressWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            g_cancelled = false;

            HWND phaseLabel = CreateWindowExW(0, L"STATIC", L"阶段：", WS_CHILD | WS_VISIBLE,
                                              theme::S(20), theme::S(18), theme::S(50), theme::S(20),
                                              hwnd, (HMENU)(INT_PTR)ProgressDialog::IDC_PD_PHASE,
                                              nullptr, nullptr);
            HWND phase = CreateWindowExW(0, L"STATIC", L"准备中", WS_CHILD | WS_VISIBLE,
                                         theme::S(72), theme::S(18), theme::S(388), theme::S(20),
                                         hwnd, nullptr, nullptr, nullptr);
            g_pdPhaseVal = phase;
            SetPropW(phase, L"dirmv_pd_clr", (HANDLE)(INT_PTR)(theme::Accent + 1));

            HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                       WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                       theme::S(20), theme::S(46), theme::S(440), theme::S(10),
                                       hwnd, (HMENU)(INT_PTR)ProgressDialog::IDC_PD_BAR, nullptr,
                                       nullptr);
            SendMessageW(bar, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
            SendMessageW(bar, PBM_SETPOS, 0, 0);
            SendMessageW(bar, PBM_SETBARCOLOR, 0, (LPARAM)theme::Accent);
            SendMessageW(bar, PBM_SETBKCOLOR, 0, (LPARAM)theme::SegBg);

            HWND text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        theme::S(20), theme::S(66), theme::S(440), theme::S(44),
                                        hwnd, (HMENU)(INT_PTR)ProgressDialog::IDC_PD_TEXT, nullptr,
                                        nullptr);

            HWND eta = CreateWindowExW(0, L"STATIC", L"预计剩余：--", WS_CHILD | WS_VISIBLE,
                                       theme::S(20), theme::S(116), theme::S(212), theme::S(20),
                                       hwnd, (HMENU)(INT_PTR)ProgressDialog::IDC_PD_ETA, nullptr,
                                       nullptr);

            HWND speed = CreateWindowExW(0, L"STATIC", L"速度：--", WS_CHILD | WS_VISIBLE,
                                         theme::S(240), theme::S(116), theme::S(220), theme::S(20),
                                         hwnd, (HMENU)(INT_PTR)ProgressDialog::IDC_PD_SPEED, nullptr,
                                         nullptr);

            HWND cancel = theme::CreateButton(hwnd, ProgressDialog::IDC_PD_CANCEL, L"取消",
                                              theme::BtnRole::BtnDefault, theme::S(364),
                                              theme::S(158), theme::S(96), theme::S(32));

            HWND ctrls[] = {phaseLabel, phase, text, eta, speed, cancel};
            for (HWND c : ctrls)
                theme::ApplyFont(c, theme::FontBody());
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
                if (IsWindow(g_pdPhaseVal))
                    SetWindowTextW(g_pdPhaseVal, info->phase.c_str());

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
        case WM_ERASEBKGND:
        {
            HDC dc = (HDC)wParam;
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, theme::CardBrush());
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        {
            HDC dc = (HDC)wParam;
            HWND c = (HWND)lParam;
            SetBkMode(dc, TRANSPARENT);
            COLORREF clr = theme::TextSub;
            HANDLE prop = GetPropW(c, L"dirmv_pd_clr");
            if (prop)
                clr = (COLORREF)((INT_PTR)prop - 1);
            else if (GetDlgCtrlID(c) == ProgressDialog::IDC_PD_TEXT)
                clr = theme::Text;
            SetTextColor(dc, clr);
            return (LRESULT)theme::CardBrush();
        }
        case WM_CTLCOLORBTN:
            return (LRESULT)theme::CardBrush();
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
        wc.hbrBackground = nullptr;
        RegisterClassExW(&wc);
    }

    HWND ProgressDialog::Show(HWND parent, const std::wstring &title, bool cancellable)
    {
        HWND dlg = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            L"dirMoverProgressWnd", title.c_str(),
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, theme::S(480), theme::S(240),
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
