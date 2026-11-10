#include "gui/MainWindow.h"
#include "gui/ProgressDialog.h"
#include "gui/TrayIcon.h"
#include "gui/App.h"
#include "gui/Theme.h"
#include "core/PathUtil.h"
#include "core/Log.h"
#include "core/MigrateEngine.h"
#include "core/Scheduler.h"
#include "core/PipeServer.h"
#include "service/ServiceMain.h"

#include <windows.h>
#include <winver.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <thread>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

#pragma comment(lib, "comctl32.lib")

namespace dirmv::gui
{

    namespace
    {
        HINSTANCE g_hInst = nullptr;
        Config g_cfg;
        std::wstring g_iniPath;

        HWND g_hNavButtons[4] = {nullptr, nullptr, nullptr, nullptr}; // 左侧导航按钮
        HWND g_hRuleList = nullptr;
        HWND g_hLog = nullptr;
        HWND g_hProgress = nullptr;
        HWND g_hEta = nullptr;
        HWND g_hActLabel = nullptr; // 规则页「选中规则」标签
        HWND g_hTaskLabel = nullptr;
        HWND g_hLogLabel = nullptr;
        HWND g_hSvcLogPath = nullptr; // 服务页日志路径
        std::vector<bool> g_ruleChecked;   // 与 g_cfg.rules 对应
        HIMAGELIST g_hCheckImgs = nullptr; // 复选框图像列表
        // 分页子窗口（0=规则 1=计划 2=服务 3=关于），提前声明供按钮查找使用
        HWND g_pageHwnd[4] = {nullptr, nullptr, nullptr, nullptr};

        // 计划页控件
        HWND g_hSchedPeriod = nullptr;
        HWND g_hSchedHour = nullptr;
        HWND g_hSchedMin = nullptr;
        HWND g_hSchedDom = nullptr;
        HWND g_hSchedNext = nullptr;
        HWND g_hSchedTimeLabel = nullptr;
        HWND g_hSchedColon = nullptr;
        HWND g_hSchedWeekLabel = nullptr;
        HWND g_hSchedWeekCombo = nullptr;
        HWND g_hSchedDomLabel = nullptr;
        HWND g_hSchedNDaysLabel = nullptr;
        HWND g_hSchedNDays = nullptr;
        HWND g_hThreads = nullptr;
        HWND g_hLinkType = nullptr;
        HWND g_hWatcher = nullptr;
        HWND g_hMoveMode = nullptr;
        HWND g_hLockDetect = nullptr;
        HWND g_hRetryMin = nullptr;

        // 关于页控件
        HWND g_hAbout = nullptr;
        HWND g_hSvcState = nullptr;
        COLORREF g_svcColor = theme::TextSub; // 服务状态文字颜色

        std::thread g_worker;
        std::atomic<bool> g_cancel{false};
        std::atomic<bool> g_busy{false};
        HWND g_progressDlg = nullptr;
        HFONT g_font = nullptr;

        int g_curTab = 0;
        int g_selectedRule = -1;

        const int PAGE_RULES = 0;
        const int PAGE_PLAN = 1;
        const int PAGE_SERVICE = 2;
        const int PAGE_ABOUT = 3;

        void SetFont(HWND h)
        {
            if (g_font)
                SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
        }

        void SetLabelColor(HWND h, COLORREF c); // 定义见下方布局段

        // 在父窗口绘制 1px 描边，包住无边框的子控件（输入框/列表框/列表视图）
        void DrawChildFrame(HWND child, HDC dc, HWND parent)
        {
            if (!child || !IsWindowVisible(child))
                return;
            RECT r;
            GetWindowRect(child, &r);
            MapWindowPoints(HWND_DESKTOP, parent, (POINT *)&r, 2);
            RECT parts[4] = {
                {r.left - 1, r.top - 1, r.right + 1, r.top},
                {r.left - 1, r.bottom, r.right + 1, r.bottom + 1},
                {r.left - 1, r.top, r.left, r.bottom},
                {r.right, r.top, r.right + 1, r.bottom}};
            HBRUSH br = CreateSolidBrush(theme::BorderStrong);
            for (auto &p : parts)
                FillRect(dc, &p, br);
            DeleteObject(br);
        }

        void FramePageFields(HWND page, HDC dc)
        {
            EnumChildWindows(
                page,
                [](HWND c, LPARAM l) -> BOOL
                {
                    wchar_t cls[64]{};
                    GetClassNameW(c, cls, 64);
                    if (lstrcmpiW(cls, L"Edit") == 0 || lstrcmpiW(cls, L"SysListView32") == 0)
                        DrawChildFrame(c, (HDC)l, GetParent(c));
                    return TRUE;
                },
                (LPARAM)dc);
        }

        void AppendLog(const std::wstring &line)
        {
            if (!g_hLog)
                return;
            int len = GetWindowTextLengthW(g_hLog);
            SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
            std::wstring l = line + L"\r\n";
            SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)l.c_str());
            SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
        }

        void PostLog(const std::wstring &line)
        {
            if (!g_mainWnd)
                return;
            auto *s = new std::wstring(line);
            PostMessageW(g_mainWnd, MainWindow::WM_APP_LOG, 0, (LPARAM)s);
        }

        std::wstring GetText(HWND h)
        {
            int len = GetWindowTextLengthW(h);
            std::wstring s((size_t)len, L'\0');
            if (len > 0)
                GetWindowTextW(h, &s[0], len + 1);
            return s;
        }

        std::wstring PickFolder(HWND owner, const wchar_t *title)
        {
            std::wstring result;
            BROWSEINFOW bi{};
            bi.hwndOwner = owner;
            bi.lpszTitle = title ? title : L"选择文件夹";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl)
            {
                wchar_t path[MAX_PATH];
                if (SHGetPathFromIDListW(pidl, path))
                    result = path;
                CoTaskMemFree(pidl);
            }
            return result;
        }

        // 简单多选一对话框：返回按钮索引（0..n-1），取消/关闭返回 -1。
        // labels 为按钮文本（最多 4 个），title 为窗口标题，body 为说明文字。
        int ChooseOneDialog(HWND owner, const wchar_t *title, const std::wstring &body,
                            const std::vector<std::wstring> &labels)
        {
            static const wchar_t *kCls = L"dirMoverChooseDlg";
            static bool registered = false;
            struct Ctx
            {
                int result = -1;
                const std::vector<std::wstring> *labels = nullptr;
                const std::wstring *body = nullptr;
            };
            Ctx ctx;
            ctx.labels = &labels;
            ctx.body = &body;

            if (!registered)
            {
                WNDCLASSEXW wc{};
                wc.cbSize = sizeof(wc);
                wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT
                {
                    auto *c = (Ctx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
                    switch (msg)
                    {
                    case WM_CREATE:
                    {
                        c = (Ctx *)((CREATESTRUCTW *)lp)->lpCreateParams;
                        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
                        HWND bodyH = CreateWindowExW(0, L"STATIC", c->body->c_str(),
                                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                                     theme::S(16), theme::S(12), theme::S(424),
                                                     theme::S(200), hwnd, nullptr, nullptr, nullptr);
                        theme::ApplyFont(bodyH, theme::FontBody());
                        SetPropW(bodyH, L"dirmv_clr", (HANDLE)(INT_PTR)(theme::TextSub + 1));
                        int n = (int)c->labels->size();
                        int bw = 130, gap = 8;
                        int totalW = n * bw + (n - 1) * gap;
                        int x = (456 - totalW) / 2;
                        for (int i = 0; i < n; ++i)
                        {
                            theme::CreateButton(hwnd, i + 1, (*c->labels)[i],
                                                i == 0 ? theme::BtnRole::BtnPrimary
                                                       : theme::BtnRole::BtnDefault,
                                                theme::S(x), theme::S(222), theme::S(bw),
                                                theme::S(32));
                            x += bw + gap;
                        }
                        return 0;
                    }
                    case WM_COMMAND:
                    {
                        int id = LOWORD(wp);
                        if (id >= 1)
                        {
                            c->result = id - 1;
                            DestroyWindow(hwnd);
                        }
                        return 0;
                    }
                    case WM_CLOSE:
                        c->result = -1;
                        DestroyWindow(hwnd);
                        return 0;
                    case WM_ERASEBKGND:
                    {
                        HDC dc = (HDC)wp;
                        RECT rc;
                        GetClientRect(hwnd, &rc);
                        FillRect(dc, &rc, theme::CardBrush());
                        return 1;
                    }
                    case WM_CTLCOLORSTATIC:
                    {
                        HDC dc = (HDC)wp;
                        SetBkMode(dc, TRANSPARENT);
                        SetTextColor(dc, theme::TextSub);
                        return (LRESULT)theme::CardBrush();
                    }
                    }
                    return DefWindowProcW(hwnd, msg, wp, lp);
                };
                wc.hInstance = g_hInst;
                wc.lpszClassName = kCls;
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wc.hbrBackground = nullptr;
                RegisterClassExW(&wc);
                registered = true;
            }

            RECT rc{};
            GetWindowRect(owner, &rc);
            HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kCls, title ? title : L"请选择",
                                       WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                       rc.left + theme::S(80), rc.top + theme::S(60),
                                       theme::S(470), theme::S(300), owner, nullptr, g_hInst,
                                       &ctx);
            EnableWindow(owner, FALSE);
            MSG msg{};
            while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                if (!IsDialogMessageW(dlg, &msg))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            EnableWindow(owner, TRUE);
            SetForegroundWindow(owner);
            return ctx.result;
        }

        // ---- 规则列表 ----
        void UpdateRuleButtons(); // 前置声明
        // 复选框图像：0=未选 1=已选
        void EnsureCheckImages()
        {
            if (g_hCheckImgs)
                return;
            const int kIcon = theme::S(24);
            const int kBox = theme::S(15);
            g_hCheckImgs = ImageList_Create(kIcon, kIcon, ILC_COLOR32 | ILC_MASK, 2, 0);
            for (int checked = 0; checked < 2; ++checked)
            {
                HDC hdc = GetDC(nullptr);
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, kIcon, kIcon);
                HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
                RECT rc{0, 0, kIcon, kIcon};
                HBRUSH bg = CreateSolidBrush(theme::Card);
                FillRect(mem, &rc, bg);
                DeleteObject(bg);

                int bx = (kIcon - kBox) / 2;
                int by = (kIcon - kBox) / 2;
                RECT box{bx, by, bx + kBox, by + kBox};
                if (checked)
                {
                    theme::RoundRectEx(mem, box, theme::S(4), theme::Accent, theme::Accent, 1);
                    HPEN gp = CreatePen(PS_SOLID, theme::S(2), RGB(255, 255, 255));
                    HPEN ogp = (HPEN)SelectObject(mem, gp);
                    MoveToEx(mem, bx + theme::S(3), by + theme::S(8), nullptr);
                    LineTo(mem, bx + theme::S(6), by + theme::S(11));
                    LineTo(mem, bx + theme::S(12), by + theme::S(4));
                    SelectObject(mem, ogp);
                    DeleteObject(gp);
                }
                else
                {
                    theme::RoundRectEx(mem, box, theme::S(4), theme::Card, theme::BorderStrong, 1);
                }
                SelectObject(mem, old);
                DeleteDC(mem);
                ReleaseDC(nullptr, hdc);
                ImageList_AddMasked(g_hCheckImgs, bmp, RGB(255, 255, 255));
                DeleteObject(bmp);
            }
        }

        void RefreshRuleList()
        {
            if (!g_hRuleList)
                return;
            // 保留勾选状态（按规则数量对齐）
            g_ruleChecked.resize(g_cfg.rules.size(), false);

            // 记住当前选中规则的标识（按 watchDir），重建后恢复选中，避免选中丢失
            std::wstring selWatch;
            if (g_selectedRule >= 0 && g_selectedRule < (int)g_cfg.rules.size())
                selWatch = g_cfg.rules[g_selectedRule].watchDir;

            SendMessageW(g_hRuleList, LVM_DELETEALLITEMS, 0, 0);
            for (size_t i = 0; i < g_cfg.rules.size(); ++i)
            {
                const Rule &r = g_cfg.rules[i];
                LVITEMW it{};
                it.mask = LVIF_TEXT | LVIF_IMAGE;
                it.iItem = (int)i;
                it.iSubItem = 0;
                it.iImage = g_ruleChecked[i] ? 1 : 0;
                it.pszText = const_cast<LPWSTR>(L"");
                SendMessageW(g_hRuleList, LVM_INSERTITEMW, 0, (LPARAM)&it);

                ListView_SetItemText(g_hRuleList, (int)i, 1, const_cast<LPWSTR>(r.watchDir.c_str()));

                std::wstring tgt = r.targetDir.empty() ? L"(未设置)" : r.targetDir;
                ListView_SetItemText(g_hRuleList, (int)i, 2, const_cast<LPWSTR>(tgt.c_str()));

                std::wstring ex = std::to_wstring(r.exclude.size());
                ListView_SetItemText(g_hRuleList, (int)i, 3, const_cast<LPWSTR>(ex.c_str()));

                std::wstring st = r.enabled ? L"已启用" : L"未启用";
                if (r.pendingDelete)
                    st = L"待删除(还原失败)";
                else if (r.enabled && !g_cfg.ValidateOneRule(r).empty())
                    st = L"不可用";
                ListView_SetItemText(g_hRuleList, (int)i, 4, const_cast<LPWSTR>(st.c_str()));

                // 第 5 列「操作」：显示「编辑」文本（点击该列即编辑本行规则）
                ListView_SetItemText(g_hRuleList, (int)i, 5, const_cast<LPWSTR>(L"编辑"));
            }

            // 恢复选中（按 watchDir 精确匹配；找不到则清空）
            int newSel = -1;
            if (!selWatch.empty())
            {
                for (size_t i = 0; i < g_cfg.rules.size(); ++i)
                    if (Path::Equals(g_cfg.rules[i].watchDir, selWatch))
                    {
                        newSel = (int)i;
                        break;
                    }
            }
            g_selectedRule = newSel;
            if (newSel >= 0)
            {
                ListView_SetItemState(g_hRuleList, newSel,
                                      LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }

            UpdateRuleButtons();
        }

        // 当前勾选的行索引
        std::vector<int> CheckedRows()
        {
            std::vector<int> rows;
            for (size_t i = 0; i < g_ruleChecked.size(); ++i)
                if (g_ruleChecked[i])
                    rows.push_back((int)i);
            return rows;
        }

        bool IsAllChecked()
        {
            if (g_cfg.rules.empty())
                return false;
            for (bool b : g_ruleChecked)
                if (!b)
                    return false;
            return true;
        }

        void SetAllChecked(bool checked)
        {
            g_ruleChecked.assign(g_cfg.rules.size(), checked);
            RefreshRuleList();
        }

        // 根据当前选中规则刷新按钮可用性
        void UpdateRuleButtons()
        {
            bool hasSel = (g_selectedRule >= 0 && g_selectedRule < (int)g_cfg.rules.size());
            bool canMigrate = false;
            if (hasSel)
                canMigrate = g_cfg.ValidateOneRule(g_cfg.rules[g_selectedRule]).empty();

            auto anyChecked = !CheckedRows().empty();

            struct
            {
                int id;
                bool enable;
            } items[] = {
                {MainWindow::IDC_BTN_MIGRATE_RULE, canMigrate},
                {MainWindow::IDC_BTN_RESTORE_RULE, hasSel},
                {MainWindow::IDC_BTN_EDIT_RULE, hasSel},
                {MainWindow::IDC_BTN_ENABLE_RULE, hasSel || anyChecked},
                {MainWindow::IDC_BTN_DISABLE_RULE, hasSel || anyChecked},
                {MainWindow::IDC_BTN_DELETE_RULE, hasSel},
                {MainWindow::IDC_BTN_ADOPT, hasSel},
                {MainWindow::IDC_BTN_REBUILD, hasSel},
            };
            for (auto &it : items)
            {
                HWND page = g_pageHwnd[PAGE_RULES];
                HWND h = page ? GetDlgItem(page, it.id) : nullptr;
                if (!h)
                    h = GetDlgItem(g_mainWnd, it.id);
                if (h)
                    EnableWindow(h, it.enable ? TRUE : FALSE);
            }
        }

        // ---- 计划页 <-> 配置 ----
        // 根据所选「定时间隔」显示对应的编辑项（只显示相关的）
        void UpdateScheduleVisibility()
        {
            int period = (int)SendMessageW(g_hSchedPeriod, CB_GETCURSEL, 0, 0);
            bool isDisabled = (period == (int)SchedulePeriod::Disabled);
            bool isWeekly = (period == (int)SchedulePeriod::Weekly);
            bool isMonthly = (period == (int)SchedulePeriod::Monthly);
            bool isNDays = (period == (int)SchedulePeriod::EveryNDays);
            bool showTime = !isDisabled; // 每天/每周/每月/每N天 都需时刻

            auto show = [](HWND h, bool vis)
            { if (h) ShowWindow(h, vis ? SW_SHOW : SW_HIDE); };

            show(g_hSchedTimeLabel, showTime);
            show(g_hSchedHour, showTime);
            show(g_hSchedColon, showTime);
            show(g_hSchedMin, showTime);

            show(g_hSchedWeekLabel, isWeekly);
            show(g_hSchedWeekCombo, isWeekly);

            show(g_hSchedDomLabel, isMonthly);
            show(g_hSchedDom, isMonthly);

            show(g_hSchedNDaysLabel, isNDays);
            show(g_hSchedNDays, isNDays);
        }

        Schedule ReadScheduleFromUI()
        {
            Schedule s = g_cfg.schedule;
            s.period = (SchedulePeriod)SendMessageW(g_hSchedPeriod, CB_GETCURSEL, 0, 0);
            s.hour = GetDlgItemInt(g_mainWnd, MainWindow::IDC_SCHED_HOUR, nullptr, FALSE);
            s.minute = GetDlgItemInt(g_mainWnd, MainWindow::IDC_SCHED_MIN, nullptr, FALSE);
            s.dayOfMonth = GetDlgItemInt(g_mainWnd, MainWindow::IDC_SCHED_DOM, nullptr, FALSE);
            s.everyNDays = GetDlgItemInt(g_mainWnd, MainWindow::IDC_SCHED_N, nullptr, FALSE);
            // 周几下拉：0=周日 .. 6=周六
            int wd = (int)SendMessageW(g_hSchedWeekCombo, CB_GETCURSEL, 0, 0);
            if (wd < 0)
                wd = 1;
            s.weekdayMask = (1 << wd);
            return s;
        }

        void WriteScheduleToUI(const Schedule &s)
        {
            SendMessageW(g_hSchedPeriod, CB_SETCURSEL, (WPARAM)s.period, 0);
            SetDlgItemInt(g_mainWnd, MainWindow::IDC_SCHED_HOUR, s.hour, FALSE);
            SetDlgItemInt(g_mainWnd, MainWindow::IDC_SCHED_MIN, s.minute, FALSE);
            SetDlgItemInt(g_mainWnd, MainWindow::IDC_SCHED_DOM, s.dayOfMonth, FALSE);
            SetDlgItemInt(g_mainWnd, MainWindow::IDC_SCHED_N, s.everyNDays, FALSE);
            // 周几下拉：取 weekdayMask 中最低置位对应的星期
            int wd = 1;
            for (int i = 0; i < 7; ++i)
                if (s.weekdayMask & (1 << i))
                {
                    wd = i;
                    break;
                }
            SendMessageW(g_hSchedWeekCombo, CB_SETCURSEL, (WPARAM)wd, 0);
            UpdateScheduleVisibility();
        }

        void UpdateNextRunLabel()
        {
            NextRun nr = Scheduler::ComputeNextRun(ReadScheduleFromUI(), time(nullptr));
            if (nr.valid)
            {
                std::tm lt{};
                localtime_s(&lt, &nr.when);
                wchar_t buf[128];
                swprintf_s(buf, L"下次运行：%04d-%02d-%02d %02d:%02d",
                           lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
                SetWindowTextW(g_hSchedNext, buf);
            }
            else
            {
                SetWindowTextW(g_hSchedNext, L"下次运行：未启用定时");
            }
        }

        // 计划页设置写入 g_cfg
        void ReadPlanIntoConfig()
        {
            g_cfg.schedule = ReadScheduleFromUI();
            g_cfg.workerThreads = GetDlgItemInt(g_mainWnd, MainWindow::IDC_THREADS, nullptr, FALSE);
            g_cfg.linkType = (LinkType)SendMessageW(g_hLinkType, CB_GETCURSEL, 0, 0);
            g_cfg.enableWatcher = SendMessageW(g_hWatcher, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_cfg.moveMode = SendMessageW(g_hMoveMode, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_cfg.lockDetectEnabled = SendMessageW(g_hLockDetect, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_cfg.retryDelayMinutes = GetDlgItemInt(g_mainWnd, MainWindow::IDC_RETRYMIN, nullptr, FALSE);
        }

        void WritePlanToUI(const Config &c)
        {
            WriteScheduleToUI(c.schedule);
            SetDlgItemInt(g_mainWnd, MainWindow::IDC_THREADS, c.workerThreads, FALSE);
            SendMessageW(g_hLinkType, CB_SETCURSEL, (WPARAM)c.linkType, 0);
            SendMessageW(g_hWatcher, BM_SETCHECK, c.enableWatcher ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(g_hMoveMode, BM_SETCHECK, c.moveMode ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(g_hLockDetect, BM_SETCHECK, c.lockDetectEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
            SetDlgItemInt(g_mainWnd, MainWindow::IDC_RETRYMIN, c.retryDelayMinutes, FALSE);
            UpdateNextRunLabel();
        }

        void UpdateServiceState()
        {
            int st = ServiceInstaller::QueryState(L"dirMoverSvc");
            const wchar_t *txt = L"未知";
            COLORREF color = theme::TextSub;
            switch (st)
            {
            case -2:
                txt = L"未安装";
                color = theme::TextSub;
                break;
            case -1:
                txt = L"无法查询（权限不足）";
                color = theme::Warning;
                break;
            case 0:
                txt = L"已安装（已停止）";
                color = theme::TextSub;
                break;
            case 1:
                txt = L"运行中";
                color = theme::Success;
                break;
            case 3:
                txt = L"已暂停";
                color = theme::Warning;
                break;
            case 2:
                txt = L"状态切换中";
                color = theme::TextSub;
                break;
            default:
                txt = L"未知";
                break;
            }
            g_svcColor = color;
            SetWindowTextW(g_hSvcState, txt);
            SetLabelColor(g_hSvcState, color);
            if (g_hSvcState)
                InvalidateRect(g_hSvcState, nullptr, TRUE);

            // 按当前状态启用/禁用按钮，避免对 SCM 发起必然失败的操作
            HWND page = g_pageHwnd[PAGE_SERVICE];
            if (!page)
                return;
            auto enable = [page](int id, bool on)
            {
                HWND h = GetDlgItem(page, id);
                if (h)
                    EnableWindow(h, on ? TRUE : FALSE);
            };
            enable(MainWindow::IDC_BTN_SVCINSTALL, st == -2);
            enable(MainWindow::IDC_BTN_SVCSTART, st == 0);
            enable(MainWindow::IDC_BTN_SVCSTOP, st == 1 || st == 3);
            enable(MainWindow::IDC_BTN_SVCREMOVE, st == 0 || st == 3);
        }

        // 读取自身 exe 的版本号（以资源版本信息为准，避免界面里硬编码过期）
        std::wstring GetAppVersion()
        {
            wchar_t path[MAX_PATH]{};
            if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
                return L"?";
            DWORD dummy = 0;
            DWORD size = GetFileVersionInfoSizeW(path, &dummy);
            if (!size)
                return L"?";
            std::vector<BYTE> buf(size);
            if (!GetFileVersionInfoW(path, 0, size, buf.data()))
                return L"?";
            VS_FIXEDFILEINFO *fi = nullptr;
            UINT len = 0;
            if (!VerQueryValueW(buf.data(), L"\\", (void **)&fi, &len) || !fi)
                return L"?";
            return std::to_wstring(HIWORD(fi->dwFileVersionMS)) + L"." +
                   std::to_wstring(LOWORD(fi->dwFileVersionMS)) + L"." +
                   std::to_wstring(HIWORD(fi->dwFileVersionLS)) + L"." +
                   std::to_wstring(LOWORD(fi->dwFileVersionLS));
        }

        // 保存配置（应用配置目录 + 监视目录 + 目标目录）
        void SaveConfig()
        {
            ConfigStore::Save(g_iniPath, g_cfg);
            PipeClient::Send(PipeServer::DefaultPipeName(), PipeCommand::Reload);
        }

        // ---- 规则编辑对话框（弹出窗口）
        //   新建：可编辑监视目录 + 目标目录 + 双面板白名单
        //   修改：监视目录只读；目标目录 + 双面板白名单可改
        //   双面板白名单：左=非白名单(监视目录下一级子目录/链接)，右=白名单
        //                 点击某一项即移动到另一侧
        // 返回 true=确定；rule 就地修改（含 exclude）
        bool RuleEditDialog(HWND owner, Rule &rule, bool isNew)
        {
            struct DlgCtx
            {
                Rule *rule;
                bool ok;
                bool isNew;
                bool dirty; // 是否有未保存修改
            };
            DlgCtx ctx{&rule, false, isNew, false};

            const int IDC_WATCH = 1;
            const int IDC_TARGET = 2;
            const int IDC_LIST_EXCL = 4; // 右：白名单

            static const wchar_t *kCls = L"dirMoverRuleEditDlg";
            static bool registered = false;
            if (!registered)
            {
                WNDCLASSEXW wc{};
                wc.cbSize = sizeof(wc);
                wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT
                {
                    auto *c = (DlgCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
                    switch (msg)
                    {
                    case WM_CREATE:
                    {
                        c = (DlgCtx *)((CREATESTRUCTW *)lp)->lpCreateParams;
                        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
                        auto L = [&](const wchar_t *t, int x, int y, int w, int h)
                        {
                            HWND s = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE,
                                                     theme::S(x), theme::S(y), theme::S(w),
                                                     theme::S(h), hwnd, nullptr, nullptr, nullptr);
                            theme::ApplyFont(s, theme::FontBody());
                            SetPropW(s, L"dirmv_clr", (HANDLE)(INT_PTR)(theme::TextSub + 1));
                            return s;
                        };
                        auto E = [&](int id, const std::wstring &t, int x, int y, int w, int h, bool ro)
                        {
                            HWND e = CreateWindowExW(0, L"EDIT", t.c_str(),
                                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL |
                                                         (ro ? ES_READONLY : 0),
                                                     theme::S(x), theme::S(y), theme::S(w),
                                                     theme::S(h), hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
                            theme::ApplyFont(e, theme::FontBody());
                            return e;
                        };
                        auto B = [&](int id, const wchar_t *t, int x, int y, int w, int h,
                                     theme::BtnRole role)
                        {
                            return theme::CreateButton(hwnd, id, t, role, theme::S(x), theme::S(y),
                                                       theme::S(w), theme::S(h));
                        };

                        L(L"监视目录：", 12, 15, 70, 20);
                        E(1, c->rule->watchDir, 86, 12, 330, 26, !c->isNew);
                        if (c->isNew)
                            B(101, L"浏览…", 420, 12, 64, 26, theme::BtnRole::BtnGhost);

                        L(L"目标目录：", 12, 49, 70, 20);
                        E(2, c->rule->targetDir, 86, 46, 330, 26, false);
                        B(102, L"浏览…", 420, 46, 64, 26, theme::BtnRole::BtnGhost);

                        L(L"点击条目可在两栏间移动（右侧为白名单，不被迁移）：", 12, 84, 400, 20);
                        L(L"非白名单（会迁移）", 12, 108, 180, 18);
                        L(L"白名单（不迁移）", 258, 108, 180, 18);

                        HWND lstAll = CreateWindowExW(0, L"LISTBOX", L"",
                                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                                          LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                                      theme::S(12), theme::S(128), theme::S(230),
                                                      theme::S(176), hwnd, (HMENU)(INT_PTR)3,
                                                      nullptr, nullptr);
                        HWND lstExcl = CreateWindowExW(0, L"LISTBOX", L"",
                                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                                           LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                                       theme::S(258), theme::S(128), theme::S(230),
                                                       theme::S(176), hwnd, (HMENU)(INT_PTR)4,
                                                       nullptr, nullptr);
                        theme::ApplyFont(lstAll, theme::FontBody());
                        theme::ApplyFont(lstExcl, theme::FontBody());

                        // 填充：左 = 监视目录下一级子目录/链接（排除已在白名单中的），右 = 白名单
                        if (!c->rule->watchDir.empty() && Path::DirectoryExists(c->rule->watchDir))
                        {
                            for (auto &sub : Path::ListSubDirectories(c->rule->watchDir))
                            {
                                bool isExcl = false;
                                for (auto &e : c->rule->exclude)
                                    if (Path::Equals(e, sub))
                                    {
                                        isExcl = true;
                                        break;
                                    }
                                if (!isExcl)
                                    SendMessageW(lstAll, LB_ADDSTRING, 0, (LPARAM)sub.c_str());
                            }
                        }
                        // 白名单中不在磁盘上的项也要显示（保留用户设置）
                        for (auto &e : c->rule->exclude)
                            SendMessageW(lstExcl, LB_ADDSTRING, 0, (LPARAM)e.c_str());

                        B(IDOK, L"保存", 336, 316, 76, 30, theme::BtnRole::BtnPrimary);
                        B(IDCANCEL, L"取消", 420, 316, 76, 30, theme::BtnRole::BtnDefault);
                        return 0;
                    }
                    case WM_COMMAND:
                    {
                        int id = LOWORD(wp);
                        int code = HIWORD(wp);
                        auto *cc = (DlgCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

                        // 先处理「保存 / 取消」——注意 监视/目标 Edit 的 id 分别是 1/2，
                        // 与 IDOK(1)/IDCANCEL(2) 数值相同；用通知码(BN_CLICKED)区分按钮。
                        if (id == IDOK && code == BN_CLICKED)
                        {
                            std::wstring w = GetText(GetDlgItem(hwnd, IDC_WATCH));
                            std::wstring t = GetText(GetDlgItem(hwnd, IDC_TARGET));

                            Rule tmp = *cc->rule;
                            if (cc->isNew)
                                tmp.watchDir = Path::Normalize(Path::Trim(w));
                            tmp.targetDir = Path::Normalize(Path::Trim(t));

                            tmp.exclude.clear();
                            HWND lstExcl = GetDlgItem(hwnd, IDC_LIST_EXCL);
                            int nEx = (int)SendMessageW(lstExcl, LB_GETCOUNT, 0, 0);
                            for (int i = 0; i < nEx; ++i)
                            {
                                int len = (int)SendMessageW(lstExcl, LB_GETTEXTLEN, i, 0);
                                std::wstring s(len, L'\0');
                                SendMessageW(lstExcl, LB_GETTEXT, i, (LPARAM)&s[0]);
                                if (!s.empty())
                                    tmp.exclude.push_back(s);
                            }

                            if (tmp.watchDir.empty())
                            {
                                MessageBoxW(hwnd, L"请设置监视目录。", L"规则", MB_ICONWARNING);
                                return 0;
                            }
                            if (tmp.targetDir.empty())
                            {
                                if (MessageBoxW(hwnd, L"目标目录为空，是否仍保存？\n（保存后该规则不会执行迁移）",
                                                L"规则", MB_YESNO | MB_ICONQUESTION) != IDYES)
                                    return 0;
                            }
                            *cc->rule = tmp;
                            cc->ok = true;
                            cc->dirty = false;
                            DestroyWindow(hwnd);
                            return 0;
                        }
                        if (id == IDCANCEL && code == BN_CLICKED)
                        {
                            if (cc && cc->dirty)
                            {
                                int r = MessageBoxW(hwnd, L"有未保存的修改，是否保存？",
                                                    L"未保存的修改", MB_YESNOCANCEL | MB_ICONQUESTION);
                                if (r == IDCANCEL)
                                    return 0;
                                if (r == IDYES)
                                {
                                    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
                                    return 0;
                                }
                            }
                            DestroyWindow(hwnd);
                            return 0;
                        }

                        if (id == 101)
                        {
                            std::wstring p = PickFolder(hwnd, L"选择监视目录");
                            if (!p.empty())
                            {
                                SetWindowTextW(GetDlgItem(hwnd, 1), p.c_str());
                                if (cc)
                                    cc->dirty = true;
                                // 刷新左面板
                                HWND lstAll = GetDlgItem(hwnd, 3);
                                HWND lstExcl = GetDlgItem(hwnd, 4);
                                SendMessageW(lstAll, LB_RESETCONTENT, 0, 0);
                                int nEx = (int)SendMessageW(lstExcl, LB_GETCOUNT, 0, 0);
                                std::vector<std::wstring> excl;
                                for (int i = 0; i < nEx; ++i)
                                {
                                    int len = (int)SendMessageW(lstExcl, LB_GETTEXTLEN, i, 0);
                                    std::wstring s(len, L'\0');
                                    SendMessageW(lstExcl, LB_GETTEXT, i, (LPARAM)&s[0]);
                                    excl.push_back(s);
                                }
                                if (Path::DirectoryExists(p))
                                {
                                    for (auto &sub : Path::ListSubDirectories(p))
                                    {
                                        bool isExcl = false;
                                        for (auto &e : excl)
                                            if (Path::Equals(e, sub))
                                            {
                                                isExcl = true;
                                                break;
                                            }
                                        if (!isExcl)
                                            SendMessageW(lstAll, LB_ADDSTRING, 0, (LPARAM)sub.c_str());
                                    }
                                }
                            }
                        }
                        else if (id == 102)
                        {
                            std::wstring p = PickFolder(hwnd, L"选择目标目录");
                            if (!p.empty())
                            {
                                SetWindowTextW(GetDlgItem(hwnd, 2), p.c_str());
                                if (cc)
                                    cc->dirty = true;
                            }
                        }
                        else if (id == 1 || id == 2)
                        {
                            // 文本框编辑
                            if (code == EN_CHANGE && cc)
                                cc->dirty = true;
                        }
                        else if ((id == 3 || id == 4) && code == LBN_SELCHANGE)
                        {
                            // 点击左侧项 → 移到右侧(加入白名单)
                            // 点击右侧项 → 移到左侧(移出白名单)
                            HWND from = GetDlgItem(hwnd, id);
                            HWND to = GetDlgItem(hwnd, id == 3 ? 4 : 3);
                            int sel = (int)SendMessageW(from, LB_GETCURSEL, 0, 0);
                            if (sel != LB_ERR)
                            {
                                int len = (int)SendMessageW(from, LB_GETTEXTLEN, sel, 0);
                                std::wstring s(len, L'\0');
                                SendMessageW(from, LB_GETTEXT, sel, (LPARAM)&s[0]);
                                SendMessageW(from, LB_DELETESTRING, sel, 0);
                                SendMessageW(to, LB_ADDSTRING, 0, (LPARAM)s.c_str());
                                if (cc)
                                    cc->dirty = true;
                            }
                        }
                        return 0;
                    }
                    case WM_CLOSE:
                    {
                        // 关闭按钮 / 标题栏 X：等同取消（有修改则提示）
                        auto *cc2 = (DlgCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
                        if (cc2 && cc2->dirty)
                        {
                            int r = MessageBoxW(hwnd,
                                                L"有未保存的修改，是否保存？",
                                                L"未保存的修改",
                                                MB_YESNOCANCEL | MB_ICONQUESTION);
                            if (r == IDCANCEL)
                                return 0;
                            if (r == IDYES)
                            {
                                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
                                return 0;
                            }
                        }
                        DestroyWindow(hwnd);
                        return 0;
                    }
                    case WM_ERASEBKGND:
                        return 1;
                    case WM_PAINT:
                    {
                        PAINTSTRUCT ps;
                        HDC dc = BeginPaint(hwnd, &ps);
                        RECT rc;
                        GetClientRect(hwnd, &rc);
                        FillRect(dc, &rc, theme::CardBrush());
                        HWND fields[] = {GetDlgItem(hwnd, 1), GetDlgItem(hwnd, 2),
                                         GetDlgItem(hwnd, 3), GetDlgItem(hwnd, 4)};
                        for (HWND f : fields)
                            DrawChildFrame(f, dc, hwnd);
                        EndPaint(hwnd, &ps);
                        return 0;
                    }
                    case WM_CTLCOLORDLG:
                        return (LRESULT)theme::CardBrush();
                    case WM_CTLCOLORLISTBOX:
                    {
                        HDC dc = (HDC)wp;
                        SetBkColor(dc, theme::InputBg);
                        SetTextColor(dc, theme::Text);
                        return (LRESULT)theme::InputBrush();
                    }
                    case WM_CTLCOLOREDIT:
                    {
                        HDC dc = (HDC)wp;
                        SetBkColor(dc, theme::InputBg);
                        SetTextColor(dc, theme::Text);
                        return (LRESULT)theme::InputBrush();
                    }
                    case WM_CTLCOLORSTATIC:
                    {
                        HDC dc = (HDC)wp;
                        SetBkMode(dc, TRANSPARENT);
                        SetTextColor(dc, theme::TextSub);
                        return (LRESULT)theme::CardBrush();
                    }
                    }
                    return DefWindowProcW(hwnd, msg, wp, lp);
                };
                wc.hInstance = g_hInst;
                wc.lpszClassName = kCls;
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wc.hbrBackground = nullptr;
                RegisterClassExW(&wc);
                registered = true;
            }

            RECT rc{};
            GetWindowRect(owner, &rc);
            HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kCls,
                                       isNew ? L"新建规则" : L"修改规则",
                                       WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                       rc.left + theme::S(60), rc.top + theme::S(40),
                                       theme::S(512), theme::S(392), owner, nullptr, g_hInst,
                                       &ctx);
            EnableWindow(owner, FALSE);
            MSG msg{};
            while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                if (!IsDialogMessageW(dlg, &msg))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            EnableWindow(owner, TRUE);
            SetForegroundWindow(owner);
            return ctx.ok;
        }

        // ---- 规则操作流程 ----
        void AddRuleFlow()
        {
            Rule r;
            r.watchDir.clear();
            r.targetDir.clear();
            r.exclude = Config::DefaultExclude();
            r.enabled = true;

            if (!RuleEditDialog(g_mainWnd, r, true))
                return;

            g_cfg.rules.push_back(r);

            std::wstring err = g_cfg.ValidateRules();
            if (!err.empty())
            {
                g_cfg.rules.pop_back();
                MessageBoxW(g_mainWnd, err.c_str(), L"规则无效", MB_ICONWARNING);
                return;
            }

            SaveConfig();
            RefreshRuleList();
            AppendLog(L"[规则] 已添加: " + r.watchDir + L" -> " + r.targetDir +
                      L"（白名单 " + std::to_wstring(r.exclude.size()) + L" 项）");
            MessageBoxW(g_mainWnd, L"规则已添加并启用。点击「开始迁移」执行迁移。",
                        L"添加规则", MB_ICONINFORMATION);
        }

        // 导入规则：从某个目录（监视目录/目标目录）的 .dirmv\dirmv.ini 读入规则
        void ImportRuleFlow()
        {
            std::wstring dir = PickFolder(g_mainWnd,
                                          L"选择要导入规则的目录（该目录下应有 .dirmv\\dirmv.ini，"
                                          L"如监视目录、目标目录或任一含配置的目录）");
            if (dir.empty())
                return;
            dir = Path::Normalize(dir);

            std::vector<Rule> rules;
            ConfigStore::ImportKind kind = ConfigStore::ImportKind::None;
            bool tampered = false, watchMatched = false;
            std::wstring err = ConfigStore::ImportFromDir(dir, rules, kind, tampered, watchMatched);
            if (!err.empty())
            {
                MessageBoxW(g_mainWnd, err.c_str(), L"导入规则", MB_ICONWARNING);
                return;
            }

            // 自动识别到的类型（作为默认选项展示）
            const wchar_t *autoText =
                (kind == ConfigStore::ImportKind::WatchConfig)    ? L"监视目录配置"
                : (kind == ConfigStore::ImportKind::TargetConfig) ? L"目标目录配置"
                : (kind == ConfigStore::ImportKind::Relocated)    ? L"复制/搬迁的目录配置"
                                                                  : L"未知来源";

            // ── 让用户明确选择导入类型（处理方式）──
            //   0 = 监视目录配置：本目录为监视目录，逐条确认目标目录
            //   1 = 目标目录配置：本目录为目标目录，逐条确认监视目录
            //   2 = 完整配置（多规则）：按配置导入；不存在的监视/目标目录 → 剔除
            std::vector<std::wstring> choices = {
                L"监视目录配置", L"目标目录配置", L"完整配置(多规则)"};
            int typeAns = ChooseOneDialog(
                g_mainWnd, L"导入规则 - 选择类型",
                L"所选目录：\r\n" + dir + L"\r\n" +
                    L"自动识别：" + autoText + L"\r\n" +
                    L"共 " + std::to_wstring(rules.size()) + L" 条规则\r\n\r\n" +
                    L"请输入该目录在配置中的「类型」，以决定处理方式：\r\n\r\n" +
                    L"  • 监视目录配置：监视目录 = 本目录；逐条确认/补全目标目录。\r\n" +
                    L"  • 目标目录配置（含复制/搬迁过来的数据目录）：\r\n"
                    L"      目标目录 = 本目录；逐条确认/补全监视目录。\r\n" +
                    L"  • 完整配置（多规则）：按配置原样导入；\r\n"
                    L"      本机不存在的监视目录/目标目录将自动剔除。",
                choices);
            if (typeAns < 0)
                return; // 取消

            const int kWatch = 0, kTarget = 1, kFull = 2;
            const bool asWatchRole = (typeAns == kWatch);
            const bool asTargetRole = (typeAns == kTarget);
            const bool asFull = (typeAns == kFull);

            // 监视/目标目录类型：把「本目录」对应的路径修正为当前真实目录
            if (!asFull)
            {
                for (auto &r : rules)
                {
                    if (asWatchRole)
                        r.watchDir = dir;
                    else
                        r.targetDir = dir;
                }
            }
            watchMatched = asWatchRole; // 监视目录类型：监视已匹配

            // ── 完整配置类型：先剔除本机不存在的监视/目标目录 ──
            std::wstring dropped;
            if (asFull)
            {
                std::vector<Rule> kept;
                for (auto &r : rules)
                {
                    std::wstring why;
                    if (!Path::DirectoryExists(r.watchDir))
                        why = L"监视目录不存在";
                    else if (r.targetDir.empty())
                        why = L"未设置目标目录";
                    else if (!Path::DirectoryExists(r.targetDir))
                        why = L"目标目录不存在";
                    if (!why.empty())
                    {
                        dropped += L"  • " + (r.watchDir.empty() ? L"(空)" : r.watchDir) +
                                   L"  →  " + (r.targetDir.empty() ? L"(空)" : r.targetDir) +
                                   L"   [" + why + L"]\r\n";
                        continue;
                    }
                    kept.push_back(r);
                }
                if (kept.empty())
                {
                    MessageBoxW(g_mainWnd,
                                (L"完整配置中的规则在本机都不可用，已全部剔除：\r\n\r\n" + dropped)
                                    .c_str(),
                                L"导入规则", MB_ICONWARNING);
                    return;
                }
                rules = kept;
            }

            std::wstring preview;
            preview += L"来源目录：" + dir + L"\r\n";
            preview += std::wstring(L"导入类型：") +
                       (asWatchRole ? L"监视目录配置"
                                    : (asTargetRole ? L"目标目录配置" : L"完整配置(多规则)")) +
                       L"\r\n";
            preview += std::wstring(L"自动识别：") + autoText + L"\r\n";
            if (tampered)
                preview += L"⚠ 签名校验不通过（配置可能被改动）\r\n";
            preview += L"\r\n共 " + std::to_wstring(rules.size()) + L" 条规则：\r\n";
            for (size_t i = 0; i < rules.size() && i < 8; ++i)
            {
                preview += L"  " + std::to_wstring(i + 1) + L". " + rules[i].watchDir +
                           L"  →  " + (rules[i].targetDir.empty() ? L"(无目标)" : rules[i].targetDir) +
                           L"\r\n";
            }
            if (rules.size() > 8)
                preview += L"  ...（其余略）\r\n";
            if (!dropped.empty())
                preview += L"\r\n已剔除 " + std::to_wstring(std::count(dropped.begin(), dropped.end(), L'\n')) +
                           L" 条不可用规则。\r\n";

            preview += L"\r\n";
            if (asWatchRole)
                preview += L"接下来将为每条规则确认/补全「目标目录」。";
            else if (asTargetRole)
                preview += L"接下来将为每条规则确认/补全「监视目录」。";
            else
                preview += L"将按配置原样导入（已剔除不可用规则）。";
            preview += L"\r\n\r\n是否导入？";
            if (MessageBoxW(g_mainWnd, preview.c_str(), L"导入规则",
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return;

            // ── 逐条处理 ──
            //   监视目录类型：目标目录缺失/不存在 → 让用户指定目标目录
            //   目标目录类型：监视目录不存在 → 让用户重新指定监视目录
            //   完整配置类型：不做交互（已剔除），直接校验导入
            int imported = 0, skipped = 0;
            std::wstring skipDetail;
            for (auto &r : rules)
            {
                auto needsFix = [](const std::wstring &p)
                {
                    return p.empty() || !Path::DirectoryExists(p);
                };

                if (asWatchRole)
                {
                    if (needsFix(r.targetDir))
                    {
                        std::wstring nt = PickFolder(
                            g_mainWnd,
                            (L"为规则指定「目标目录」：\r\n监视目录：" + r.watchDir +
                             (r.targetDir.empty() ? L"\r\n(原目标目录：未设置)"
                                                  : L"\r\n(原目标目录不存在：" + r.targetDir + L")"))
                                .c_str());
                        if (nt.empty())
                        {
                            skipped++;
                            skipDetail += L"  • " + r.watchDir + L"（未指定目标目录）\r\n";
                            continue;
                        }
                        r.targetDir = Path::Normalize(nt);
                    }
                }
                else if (asTargetRole)
                {
                    if (needsFix(r.watchDir))
                    {
                        std::wstring nw = PickFolder(
                            g_mainWnd,
                            (L"为规则指定「监视目录」：\r\n目标目录：" + r.targetDir +
                             (r.watchDir.empty() ? L"\r\n(原监视目录：未设置)"
                                                 : L"\r\n(原监视目录不存在：" + r.watchDir + L")"))
                                .c_str());
                        if (nw.empty())
                        {
                            skipped++;
                            skipDetail += L"  • " + r.targetDir + L"（未指定监视目录）\r\n";
                            continue;
                        }
                        r.watchDir = Path::Normalize(nw);
                    }
                }

                // 去重：监视目录+目标目录完全相同则跳过
                bool dup = false;
                for (auto &e : g_cfg.rules)
                    if (Path::Equals(e.watchDir, r.watchDir) &&
                        Path::Equals(e.targetDir, r.targetDir))
                    {
                        dup = true;
                        break;
                    }
                if (dup)
                {
                    skipped++;
                    continue;
                }

                Rule trial = r;
                g_cfg.rules.push_back(trial);
                std::wstring verr = g_cfg.ValidateRules();
                if (!verr.empty())
                {
                    g_cfg.rules.pop_back();
                    AppendLog(L"[导入] 跳过无效规则 " + r.watchDir + L" : " + verr);
                    skipped++;
                    skipDetail += L"  • " + r.watchDir + L"：" + verr + L"\r\n";
                    continue;
                }
                imported++;
                // 若源目录中已存在链接，尝试接管其迁移清单
                MigrateEngine eng(g_cfg);
                eng.AdoptExisting(g_cfg.rules.back());
            }

            SaveConfig();
            RefreshRuleList();
            AppendLog(L"[导入] 从 " + dir + L" 导入 " + std::to_wstring(imported) +
                      L" 条，跳过 " + std::to_wstring(skipped) + L" 条");
            std::wstring done = L"导入完成，共导入 " + std::to_wstring(imported) + L" 条规则";
            if (skipped > 0)
                done += L"（跳过 " + std::to_wstring(skipped) + L" 条）";
            done += L"。";
            if (!dropped.empty())
                done += L"\r\n\r\n因本机不存在而剔除：\r\n" + dropped;
            if (!skipDetail.empty())
                done += L"\r\n\r\n跳过的规则：\r\n" + skipDetail;
            MessageBoxW(g_mainWnd, done.c_str(), L"导入规则", MB_ICONINFORMATION);
        }

        // 修改选中规则（弹出窗口；监视目录只读，可改目标目录/白名单）
        void EditRuleFlow()
        {
            if (g_selectedRule < 0 || g_selectedRule >= (int)g_cfg.rules.size())
            {
                MessageBoxW(g_mainWnd, L"请先在列表中选择一条规则。", L"修改规则", MB_ICONINFORMATION);
                return;
            }
            Rule r = g_cfg.rules[g_selectedRule];
            Rule before = r;

            if (!RuleEditDialog(g_mainWnd, r, false))
                return;

            bool targetChanged = !Path::Equals(before.targetDir, r.targetDir);

            if (targetChanged && !before.targetDir.empty() && !r.targetDir.empty())
            {
                if (MessageBoxW(g_mainWnd,
                                (L"目标目录已更改：\r\n\r\n旧：" + before.targetDir +
                                 L"\r\n新：" + r.targetDir +
                                 L"\r\n\r\n是否把旧目标的数据整体移动到新目标并重建链接？")
                                    .c_str(),
                                L"修改规则", MB_YESNO | MB_ICONQUESTION) == IDYES)
                {
                    MigrateEngine engine(g_cfg);
                    std::wstring err = engine.ChangeTargetDir(before, r.targetDir, nullptr, nullptr);
                    if (!err.empty())
                        MessageBoxW(g_mainWnd, err.c_str(), L"更换目标目录失败", MB_ICONERROR);
                }
            }

            // 新增的白名单项：若已迁移则还原
            std::vector<std::wstring> added;
            for (auto &e : r.exclude)
            {
                bool existed = false;
                for (auto &o : before.exclude)
                    if (Path::Equals(o, e))
                    {
                        existed = true;
                        break;
                    }
                if (!existed)
                    added.push_back(e);
            }
            if (!added.empty())
            {
                MigrateEngine engine(g_cfg);
                std::wstring log;
                for (auto &name : added)
                {
                    std::wstring err = engine.RestoreExcluded(r, name, nullptr);
                    if (!err.empty())
                        log += L"  " + name + L" : " + err + L"\r\n";
                }
                if (!log.empty())
                    MessageBoxW(g_mainWnd, (L"白名单还原异常：\r\n" + log).c_str(),
                                L"修改规则", MB_ICONWARNING);
            }

            g_cfg.rules[g_selectedRule] = r;
            SaveConfig();
            RefreshRuleList();
            AppendLog(L"[规则] 已更新: " + r.watchDir + L" -> " + r.targetDir);
        }

        void DeleteRuleFlow()
        {
            if (g_selectedRule < 0 || g_selectedRule >= (int)g_cfg.rules.size())
            {
                MessageBoxW(g_mainWnd, L"请先在列表中选择一条规则。", L"删除规则", MB_ICONINFORMATION);
                return;
            }
            Rule &r = g_cfg.rules[g_selectedRule];

            // 待删除且之前有失败：只重试还原失败的目录
            bool resume = r.pendingDelete && !r.pendingDeleteFailed.empty();
            std::wstring prompt = resume
                                      ? (L"该规则上次删除时部分目录还原失败：\r\n  " +
                                         [&]
                                         { std::wstring s; for (auto &d : r.pendingDeleteFailed) { if (!s.empty()) s += L"、"; s += d; } return s; }() +
                                         L"\r\n\r\n是否重试还原这些目录？全部成功后将删除规则。")
                                      : (L"删除规则前将先还原所有已迁移的目录（搬回源目录、删除链接）。\r\n\r\n" +
                                         r.watchDir + L"\r\n\r\n还原成功后才删除规则；若有失败会标记并保留规则。是否继续？");

            if (MessageBoxW(g_mainWnd, prompt.c_str(), L"删除规则",
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return;

            g_busy = true;
            Config cfgCopy = g_cfg;
            Rule ruleCopy = r;
            std::vector<std::wstring> onlyDirs = resume ? r.pendingDeleteFailed
                                                        : std::vector<std::wstring>{};
            int ruleIndex = g_selectedRule;
            HWND dlg = ProgressDialog::Show(g_mainWnd, L"删除规则 - 还原数据", true);
            g_progressDlg = dlg;
            g_cancel = false;

            std::thread th([cfgCopy, ruleCopy, onlyDirs, ruleIndex, dlg]()
                           {
                MigrateEngine eng(cfgCopy);
                auto rep = eng.RestoreRuleDetailed(ruleCopy, onlyDirs,
                    [dlg](const ProgressInfo &pi) { ProgressDialog::Update(dlg, pi); },
                    &g_cancel);
                auto *payload = new std::pair<int, MigrateEngine::RestoreReport>(ruleIndex, rep);
                PostMessageW(g_mainWnd, MainWindow::WM_APP_DONE, 102, (LPARAM)payload); });
            th.detach();
        }

        // 删除规则-还原完成回调
        void OnDeleteRestoreDone(std::pair<int, MigrateEngine::RestoreReport> *info)
        {
            g_busy = false;
            if (g_progressDlg)
            {
                ProgressDialog::Close(g_progressDlg);
                g_progressDlg = nullptr;
            }
            int idx = info->first;
            MigrateEngine::RestoreReport rep = info->second;
            delete info;

            if (idx < 0 || idx >= (int)g_cfg.rules.size())
            {
                RefreshRuleList();
                return;
            }

            if (rep.failedDirs.empty())
            {
                // 全部还原成功 → 删除规则
                std::wstring wd = g_cfg.rules[idx].watchDir;
                g_cfg.rules.erase(g_cfg.rules.begin() + idx);
                g_selectedRule = -1;
                SaveConfig();
                RefreshRuleList();
                AppendLog(L"[规则] 已删除: " + wd +
                          L"（还原 " + std::to_wstring(rep.succeeded) + L" 成功）");
                MessageBoxW(g_mainWnd,
                            (L"已还原全部目录（成功 " + std::to_wstring(rep.succeeded) +
                             L" 个），规则已删除。")
                                .c_str(),
                            L"删除规则成功", MB_ICONINFORMATION);
            }
            else
            {
                // 有失败：标记规则为待删除，保留，记录失败目录
                Rule &r = g_cfg.rules[idx];
                r.pendingDelete = true;
                r.pendingDeleteFailed = rep.failedDirs;
                SaveConfig();
                RefreshRuleList();

                std::wstring detail = L"以下目录还原失败（已标记，规则保留为「待删除」）：\r\n\r\n";
                for (size_t i = 0; i < rep.failedDirs.size(); ++i)
                {
                    detail += L"  • " + rep.failedDirs[i];
                    if (i < rep.failMessages.size() && !rep.failMessages[i].empty())
                        detail += L"  —— " + rep.failMessages[i];
                    detail += L"\r\n";
                }
                detail += L"\r\n处理方式：关闭占用程序后，再次点击「删除」重试还原。";
                AppendLog(L"[规则] 删除中止：还原失败 " + std::to_wstring(rep.failedDirs.size()) +
                          L" 个，规则标记为待删除");
                MessageBoxW(g_mainWnd, detail.c_str(), L"删除规则未完成", MB_ICONWARNING);
            }
        }

        // 统一设置启用/停用：作用于「勾选的行」，若无勾选则作用于当前选中行。
        // 启用时对每条规则做可用性校验：不可用的规则不能启用（跳过），但不影响其他规则。
        void SetRulesEnabledFlow(bool enable)
        {
            std::vector<int> rows = CheckedRows();
            if (rows.empty())
            {
                if (g_selectedRule >= 0 && g_selectedRule < (int)g_cfg.rules.size())
                    rows.push_back(g_selectedRule);
            }
            if (rows.empty())
            {
                MessageBoxW(g_mainWnd, L"请先勾选或选中要操作的规则。",
                            enable ? L"启用规则" : L"停用规则", MB_ICONINFORMATION);
                return;
            }

            int okCount = 0;
            std::vector<std::wstring> refused;
            for (int idx : rows)
            {
                if (idx < 0 || idx >= (int)g_cfg.rules.size())
                    continue;
                Rule &r = g_cfg.rules[idx];
                if (enable)
                {
                    // 可用性校验：不可用不能启用。
                    // 注意：ValidateOneRule 会因 r.enabled==false 直接返回「该规则未启用」，
                    // 所以必须先临时置为启用再校验，否则被停用过的规则永远无法启用。
                    bool wasEnabled = r.enabled;
                    r.enabled = true;
                    std::wstring verr = g_cfg.ValidateOneRule(r);
                    r.enabled = wasEnabled;
                    if (!verr.empty())
                    {
                        refused.push_back(r.watchDir + L"  ——  " + verr);
                        continue;
                    }
                }
                if (r.enabled != enable)
                {
                    r.enabled = enable;
                    okCount++;
                }
                AppendLog(std::wstring(L"[规则] ") + r.watchDir +
                          (enable ? L" 已启用" : L" 已停用"));
            }

            SaveConfig();
            RefreshRuleList();

            if (!refused.empty())
            {
                std::wstring m = L"以下规则不可用，未启用（不影响其他规则）：\r\n\r\n";
                for (auto &s : refused)
                    m += L"  • " + s + L"\r\n";
                MessageBoxW(g_mainWnd, m.c_str(), L"启用规则", MB_ICONWARNING);
            }
            else if (enable)
            {
                AppendLog(L"[规则] 共启用 " + std::to_wstring(okCount) + L" 条规则");
            }
        }

        // ---- 扫描已有迁移（接管） ----
        // 含义：检查监视目录下是否已存在「链接」，以及目标目录下是否已有对应的数据文件夹，
        //       把这些「此前人工/其他工具迁移过、但本应用没有记录」的目录补进本规则的清单，
        //       使本应用能识别并管理它们；同时提醒那些指向非本规则目标目录的链接。
        void AdoptFlow()
        {
            if (g_selectedRule < 0 || g_selectedRule >= (int)g_cfg.rules.size())
            {
                MessageBoxW(g_mainWnd, L"请先选择一条规则。", L"扫描接管", MB_ICONINFORMATION);
                return;
            }
            Rule &r = g_cfg.rules[g_selectedRule];
            if (r.targetDir.empty())
            {
                MessageBoxW(g_mainWnd, L"该规则尚未设置目标目录。", L"扫描接管", MB_ICONWARNING);
                return;
            }
            MigrateEngine engine(g_cfg);
            AdoptResult ar = engine.AdoptExisting(r);
            SaveConfig();
            RefreshRuleList();

            std::wstring msg = L"扫描已有迁移 完成：\n\n";
            msg += L"新增纳入 " + std::to_wstring(ar.adopted.size()) + L" 个已迁移目录：\n";
            for (auto &n : ar.adopted)
                msg += L"  + " + n + L"\n";
            if (ar.adopted.empty())
                msg += L"  (无)\n";
            msg += L"\n普通目录 " + std::to_wstring(ar.plainDirs.size()) + L" 个\n";

            if (!ar.foreignLinks.empty())
            {
                msg += L"\n[提醒] 以下链接/数据指向的不是本规则的目标目录：\n";
                int shown = 0;
                for (auto &f : ar.foreignLinks)
                {
                    if (shown++ >= 10)
                    {
                        msg += L"  ...\n";
                        break;
                    }
                    msg += L"  ! " + Path::FileName(f.linkPath) + L"  ->  " + f.target + L"\n";
                }
                msg += L"请确认这些目录是否需要处理（可能由其他工具或规则创建）。\n";
            }
            AppendLog(L"[扫描已有迁移] 新增 " + std::to_wstring(ar.adopted.size()) +
                      L" 个，外部链接 " + std::to_wstring(ar.foreignLinks.size()) + L" 个");
            MessageBoxW(g_mainWnd, msg.c_str(), L"扫描已有迁移",
                        ar.foreignLinks.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
        }

        // ---- 新机器上重建链接 ----
        void RebuildFlow()
        {
            if (g_selectedRule < 0 || g_selectedRule >= (int)g_cfg.rules.size())
            {
                MessageBoxW(g_mainWnd, L"请先选择一条规则。", L"重建链接", MB_ICONINFORMATION);
                return;
            }
            Rule &r = g_cfg.rules[g_selectedRule];
            if (r.targetDir.empty())
            {
                MessageBoxW(g_mainWnd, L"该规则尚未设置目标目录（目标目录应为已复制过来的数据目录）。", L"重建链接", MB_ICONWARNING);
                return;
            }
            std::wstring newWatch = PickFolder(g_mainWnd, L"选择新机器上的监视目录（可为不存在的路径）");
            if (newWatch.empty())
            {
                // 允许手输不存在的路径：用输入框
                newWatch = Path::Normalize(GetText(GetDlgItem(g_mainWnd, MainWindow::IDC_RULE_LIST)));
                MessageBoxW(g_mainWnd, L"未选择监视目录，已取消。", L"重建链接", MB_ICONINFORMATION);
                return;
            }
            newWatch = Path::Normalize(newWatch);

            if (MessageBoxW(g_mainWnd,
                            (L"将在新监视目录中依据目标目录的数据重建链接：\r\n\r\n目标目录：" +
                             r.targetDir + L"\r\n新监视目录：" + newWatch +
                             L"\r\n\r\n继续？")
                                .c_str(),
                            L"重建链接", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return;

            g_busy = true;
            MigrateEngine engine(g_cfg);
            Rule ruleCopy = r;
            HWND dlg = ProgressDialog::Show(g_mainWnd, L"重建链接", false);
            g_progressDlg = dlg;
            std::thread th([ruleCopy, newWatch, dlg]()
                           {
                MigrateEngine eng(g_cfg);
                std::wstring err = eng.RebuildLinksFromTarget(ruleCopy, newWatch,
                    [dlg](const ProgressInfo &pi) { ProgressDialog::Update(dlg, pi); }, nullptr);
                auto *info = new std::pair<int, std::wstring>(g_selectedRule, err);
                PostMessageW(g_mainWnd, MainWindow::WM_APP_DONE, 101, (LPARAM)info); });
            th.detach();
        }

        void OnRebuildDone(std::pair<int, std::wstring> *info)
        {
            g_busy = false;
            if (g_progressDlg)
            {
                ProgressDialog::Close(g_progressDlg);
                g_progressDlg = nullptr;
            }
            if (!info->second.empty())
            {
                AppendLog(L"[重建链接] 失败: " + info->second);
                MessageBoxW(g_mainWnd, info->second.c_str(), L"重建链接失败", MB_ICONERROR);
            }
            else
            {
                AppendLog(L"[重建链接] 完成");
                MessageBoxW(g_mainWnd, L"重建链接完成。", L"完成", MB_ICONINFORMATION);
            }
            delete info;
            RefreshRuleList();
        }

        // ---- 迁移 / 恢复 ----
        void StartMigrateCurrentRule()
        {
            if (g_busy.exchange(true))
            {
                MessageBoxW(g_mainWnd, L"当前已有任务在运行", L"dirMover", MB_ICONINFORMATION);
                return;
            }
            if (g_selectedRule < 0 || g_selectedRule >= (int)g_cfg.rules.size())
            {
                MessageBoxW(g_mainWnd, L"请先在列表中选择一条要迁移的规则。", L"开始迁移", MB_ICONINFORMATION);
                g_busy = false;
                return;
            }

            // 只校验并迁移「选中的这一条」规则（不因其他规则无效而受阻）
            ReadPlanIntoConfig();
            SaveConfig();
            Rule rule = g_cfg.rules[g_selectedRule];

            std::wstring err = g_cfg.ValidateOneRule(rule);
            if (!err.empty())
            {
                MessageBoxW(g_mainWnd,
                            (L"选中的规则不可用于迁移：\r\n\r\n" + err).c_str(),
                            L"开始迁移", MB_ICONWARNING);
                g_busy = false;
                return;
            }

            // 服务在运行则委托后台（只迁移选中的这一条规则，带上其监视目录）
            std::wstring svcResp = PipeClient::Send(PipeServer::DefaultPipeName(),
                                                    PipeCommand::Migrate, rule.watchDir);
            if (!svcResp.empty())
            {
                AppendLog(L"[服务] 已通过后台服务触发迁移（仅选中规则）: " + rule.watchDir);
                MessageBoxW(g_mainWnd, L"已通知后台服务开始迁移选中的这条规则。", L"dirMover", MB_ICONINFORMATION);
                g_busy = false;
                return;
            }

            g_cancel = false;
            AppendLog(L"[迁移] 开始迁移选中规则: " + rule.watchDir + L" -> " + rule.targetDir);
            HWND dlg = ProgressDialog::Show(g_mainWnd, L"迁移进度", true);
            g_progressDlg = dlg;
            EnableWindow(GetDlgItem(g_mainWnd, MainWindow::IDC_BTN_MIGRATE_RULE), FALSE);
            g_worker = std::thread([rule, dlg]()
                                   {
                MigrateEngine engine(g_cfg);
                auto result = engine.MigrateRule(rule,
                    [dlg](const ProgressInfo &pi) { ProgressDialog::Update(dlg, pi); },
                    &g_cancel,
                    [](const LockNotice &n)
                    {
                        auto *copy = new LockNotice(n);
                        PostMessageW(g_mainWnd, MainWindow::WM_APP_LOCK, 0, (LPARAM)copy);
                    });
                PostMessageW(g_mainWnd, MainWindow::WM_APP_DONE, 0, (LPARAM)new MigrateResult(result)); });
            g_worker.detach();
        }

        void StartRestoreCurrentRule()
        {
            if (g_busy.exchange(true))
            {
                MessageBoxW(g_mainWnd, L"当前已有任务在运行", L"dirMover", MB_ICONINFORMATION);
                return;
            }
            if (g_selectedRule < 0 || g_selectedRule >= (int)g_cfg.rules.size())
            {
                MessageBoxW(g_mainWnd, L"请先选择一条规则。", L"恢复", MB_ICONINFORMATION);
                g_busy = false;
                return;
            }
            Rule rule = g_cfg.rules[g_selectedRule];
            g_cancel = false;
            HWND dlg = ProgressDialog::Show(g_mainWnd, L"恢复进度", true);
            g_progressDlg = dlg;
            g_worker = std::thread([rule, dlg]()
                                   {
                MigrateEngine engine(g_cfg);
                auto result = engine.RestoreRule(rule,
                    [dlg](const ProgressInfo &pi) { ProgressDialog::Update(dlg, pi); },
                    &g_cancel);
                PostMessageW(g_mainWnd, MainWindow::WM_APP_DONE, 0, (LPARAM)new MigrateResult(result)); });
            g_worker.detach();
        }

        void OnTaskDone(MigrateResult *r)
        {
            g_busy = false;
            if (g_progressDlg)
            {
                ProgressDialog::Close(g_progressDlg);
                g_progressDlg = nullptr;
            }
            EnableWindow(GetDlgItem(g_mainWnd, MainWindow::IDC_BTN_MIGRATE_RULE), TRUE);

            wchar_t buf[512];
            swprintf_s(buf, L"任务完成：处理 %d，成功 %d，跳过 %d，占用 %d，失败 %d，传输 %s",
                       r->total, r->migrated, r->skipped, r->locked, r->failed,
                       Path::FormatBytes(r->bytesMoved).c_str());
            AppendLog(buf);
            if (g_hEta)
                SetWindowTextW(g_hEta, L"就绪");
            if (g_hProgress)
                SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);
            RefreshRuleList();
            MessageBoxW(g_mainWnd, buf, L"完成", MB_ICONINFORMATION);
            delete r;
        }

        void OnChangeTargetDone(std::pair<int, std::wstring> *info)
        {
            g_busy = false;
            if (g_progressDlg)
            {
                ProgressDialog::Close(g_progressDlg);
                g_progressDlg = nullptr;
            }
            int idx = info->first;
            const std::wstring &err = info->second;
            if (!err.empty())
            {
                AppendLog(L"[更换目标] 失败: " + err);
                MessageBoxW(g_mainWnd, err.c_str(), L"更换目标目录失败", MB_ICONERROR);
            }
            else if (idx >= 0 && idx < (int)g_cfg.rules.size())
            {
                // 重新选择新目标（此处已在后台完成），更新配置
                AppendLog(L"[更换目标] 完成");
                MessageBoxW(g_mainWnd, L"已更换目标目录并重建链接。", L"完成", MB_ICONINFORMATION);
            }
            delete info;
            RefreshRuleList();
        }

        void ShowLockNotice(const LockNotice &n)
        {
            std::wstring detail = L"以下文件被占用，相关子任务已跳过（未迁移，源目录保持完整）：\r\n\r\n";
            int shown = 0;
            for (auto &l : n.locks)
            {
                if (shown++ >= 8)
                {
                    detail += L"  ...\r\n";
                    break;
                }
                detail += L"• " + Path::FileName(l.filePath);
                if (!l.processes.empty())
                {
                    detail += L"   ← 占用程序：";
                    for (size_t i = 0; i < l.processes.size(); ++i)
                    {
                        if (i)
                            detail += L", ";
                        detail += l.processes[i];
                    }
                }
                detail += L"\r\n";
            }
            detail += L"\r\n处理方式：请保存并关闭上述程序，稍后重试；已自动创建一次性重试任务。";
            AppendLog(L"[占用] 子任务 " + n.subtask + L" 被占用，已跳过");
            MessageBoxW(g_mainWnd, detail.c_str(), L"文件被占用", MB_ICONWARNING);
            if (g_mainWnd)
                TrayIcon::Notify(g_mainWnd, L"dirMover 文件被占用", n.subtask + L" 迁移被跳过");
        }

        // ==================== 分页与控件布局 ====================
        void ShowPage(int page);
        void UpdateServiceState();

        // 布局常量（DIP，实际绘制统一用 theme::S 缩放）
        const int kSidebarW = 208;
        const int kPageCount = 4;
        constexpr int kNavIds[kPageCount] = {2001, 2002, 2003, 2004};
        const int kNavX = 16;
        const int kNavW = 176;
        const int kNavH = 38;
        const int kNavGap = 6;
        const int kNavTop = 118;

        void SetLabelColor(HWND h, COLORREF c)
        {
            SetPropW(h, L"dirmv_clr", (HANDLE)(INT_PTR)(c + 1));
        }

        HWND g_hTip = nullptr; // 全局 tooltip：每个操作按钮都有一句话说明

        void AddTipById(HWND parent, int id, const wchar_t *text)
        {
            HWND ctl = GetDlgItem(parent, id);
            if (!ctl)
                return;
            if (!g_hTip)
            {
                g_hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                         WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                         CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                         CW_USEDEFAULT, g_mainWnd, nullptr, g_hInst, nullptr);
                if (g_hTip)
                {
                    SendMessageW(g_hTip, TTM_SETMAXTIPWIDTH, 0, theme::S(320));
                    SendMessageW(g_hTip, TTM_SETDELAYTIME, TTDT_INITIAL, 600);
                }
            }
            if (!g_hTip)
                return;
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = g_mainWnd;
            ti.uId = (UINT_PTR)ctl;
            ti.lpszText = (LPWSTR)text;
            SendMessageW(g_hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }

        HWND MakeLabel(HWND parent, const wchar_t *text, int x, int y, int w, int h, HFONT font,
                       COLORREF color)
        {
            HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        theme::S(x), theme::S(y), theme::S(w), theme::S(h), parent,
                                        nullptr, g_hInst, nullptr);
            theme::ApplyFont(hwnd, font);
            SetLabelColor(hwnd, color);
            return hwnd;
        }

        HWND MakeEdit(HWND parent, int id, const wchar_t *text, int x, int y, int w, int h)
        {
            HWND e = CreateWindowExW(0, L"EDIT", text,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     theme::S(x), theme::S(y), theme::S(w), theme::S(h), parent,
                                     (HMENU)(INT_PTR)id, g_hInst, nullptr);
            theme::ApplyFont(e, theme::FontBody());
            SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(theme::S(8), theme::S(8)));
            return e;
        }

        HWND MakeCombo(HWND parent, int id, int x, int y, int w, int h)
        {
            HWND c = CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                         WS_VSCROLL,
                                     theme::S(x), theme::S(y), theme::S(w), theme::S(h), parent,
                                     (HMENU)(INT_PTR)id, g_hInst, nullptr);
            theme::ApplyFont(c, theme::FontBody());
            return c;
        }

        HWND MakeCheck(HWND parent, int id, const wchar_t *text, int x, int y, int w, int h)
        {
            HWND c = CreateWindowExW(0, L"BUTTON", text,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     theme::S(x), theme::S(y), theme::S(w), theme::S(h), parent,
                                     (HMENU)(INT_PTR)id, g_hInst, nullptr);
            theme::ApplyFont(c, theme::FontBody());
            return c;
        }

        // 页面标题区（标题 + 说明），所有页面共用
        void PageHeader(HWND parent, const wchar_t *title, const wchar_t *subtitle)
        {
            MakeLabel(parent, title, 40, 34, 420, 30, theme::FontH2(), theme::Text);
            MakeLabel(parent, subtitle, 40, 68, 640, 20, theme::FontSmall(), theme::TextSub);
        }

        // 规则页动态布局：列表/操作行/任务行/进度/日志随窗口高度调整
        void LayoutRulePage(HWND page)
        {
            if (!page || !g_hRuleList)
                return;
            RECT rc{};
            GetClientRect(page, &rc);
            const int w = rc.right, h = rc.bottom;
            const int left = theme::S(40);
            const int right = w - theme::S(40);
            const int panelW = right - left;

            const int bottom = h - theme::S(32);
            const int logH = theme::S(78);
            const int logY = bottom - logH;
            const int logLabelY = logY - theme::S(22);
            const int progressY = logLabelY - theme::S(26);
            const int tasksY = progressY - theme::S(42);
            const int actionsY = tasksY - theme::S(42);
            const int listTop = theme::S(112);
            int listH = actionsY - theme::S(12) - listTop;
            if (listH < theme::S(96))
                listH = theme::S(96);

            const int listW = panelW;
            SetWindowPos(g_hRuleList, nullptr, left, listTop, listW, listH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            // 列宽：目录两列自适应，其余固定
            int rest = listW - theme::S(36) - theme::S(62) - theme::S(110) - theme::S(88);
            if (rest < theme::S(240))
                rest = theme::S(240);
            ListView_SetColumnWidth(g_hRuleList, 0, theme::S(36));
            ListView_SetColumnWidth(g_hRuleList, 1, rest * 52 / 100);
            ListView_SetColumnWidth(g_hRuleList, 2, rest - rest * 52 / 100);
            ListView_SetColumnWidth(g_hRuleList, 3, theme::S(62));
            ListView_SetColumnWidth(g_hRuleList, 4, theme::S(110));
            ListView_SetColumnWidth(g_hRuleList, 5, theme::S(88));

            if (g_hActLabel)
                SetWindowPos(g_hActLabel, nullptr, left, actionsY + theme::S(6), theme::S(56),
                             theme::S(18), SWP_NOZORDER | SWP_NOACTIVATE);
            struct RowBtn
            {
                int id;
                int w;
            } acts[] = {{MainWindow::IDC_BTN_ENABLE_RULE, 64},
                        {MainWindow::IDC_BTN_DISABLE_RULE, 64},
                        {MainWindow::IDC_BTN_EDIT_RULE, 64},
                        {MainWindow::IDC_BTN_DELETE_RULE, 88}};
            int x = left + theme::S(60);
            for (const auto &a : acts)
            {
                if (HWND b = GetDlgItem(page, a.id))
                    SetWindowPos(b, nullptr, x, actionsY, theme::S(a.w), theme::S(30),
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                x += theme::S(a.w) + theme::S(8);
            }

            if (g_hTaskLabel)
                SetWindowPos(g_hTaskLabel, nullptr, left, tasksY + theme::S(6), theme::S(56),
                             theme::S(18), SWP_NOZORDER | SWP_NOACTIVATE);
            struct TaskBtn
            {
                int id;
                int w;
            } tasks[] = {{MainWindow::IDC_BTN_MIGRATE_RULE, 96},
                         {MainWindow::IDC_BTN_RESTORE_RULE, 64},
                         {MainWindow::IDC_BTN_ADOPT, 116},
                         {MainWindow::IDC_BTN_REBUILD, 108}};
            x = left + theme::S(60);
            for (const auto &a : tasks)
            {
                if (HWND b = GetDlgItem(page, a.id))
                    SetWindowPos(b, nullptr, x, tasksY, theme::S(a.w), theme::S(30),
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                x += theme::S(a.w) + theme::S(8);
            }
            if (HWND stop = GetDlgItem(page, MainWindow::IDC_BTN_STOP))
                SetWindowPos(stop, nullptr, right - theme::S(72), tasksY, theme::S(72), theme::S(30),
                             SWP_NOZORDER | SWP_NOACTIVATE);

            const int etaW = theme::S(176);
            if (g_hProgress)
                SetWindowPos(g_hProgress, nullptr, left, progressY, panelW - etaW - theme::S(12),
                             theme::S(6), SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_hEta)
                SetWindowPos(g_hEta, nullptr, right - etaW, progressY - theme::S(7), etaW,
                             theme::S(20), SWP_NOZORDER | SWP_NOACTIVATE);

            if (g_hLogLabel)
                SetWindowPos(g_hLogLabel, nullptr, left, logLabelY, theme::S(120), theme::S(18),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_hLog)
                SetWindowPos(g_hLog, nullptr, left, logY, panelW, logH,
                             SWP_NOZORDER | SWP_NOACTIVATE);
        }

        void BuildRulePage(HWND parent)
        {
            PageHeader(parent, L"规则", L"监视目录中的非白名单内容会迁移到目标目录，原位置自动建立链接");

            RECT pr{};
            GetClientRect(parent, &pr);
            const int right = pr.right;
            theme::CreateButton(parent, MainWindow::IDC_BTN_IMPORT, L"导入规则",
                                theme::BtnRole::BtnDefault, right - theme::S(40) - theme::S(104) -
                                                           theme::S(8) - theme::S(104),
                                theme::S(34), theme::S(104), theme::S(34));
            theme::CreateButton(parent, MainWindow::IDC_BTN_ADD_RULE, L"新建规则",
                                theme::BtnRole::BtnPrimary, right - theme::S(40) - theme::S(104),
                                theme::S(34), theme::S(104), theme::S(34));

            g_hRuleList = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                                              LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                          theme::S(40), theme::S(112), theme::S(600), theme::S(300),
                                          parent, (HMENU)(INT_PTR)MainWindow::IDC_RULE_LIST,
                                          g_hInst, nullptr);
            ListView_SetExtendedListViewStyle(g_hRuleList,
                                              LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            ListView_SetImageList(g_hRuleList, g_hCheckImgs, LVSIL_SMALL);
            theme::ApplyFont(g_hRuleList, theme::FontBody());
            if (HWND hdr = ListView_GetHeader(g_hRuleList))
                theme::ApplyFont(hdr, theme::FontSmall());

            const wchar_t *heads[6] = {L"", L"监视目录", L"目标目录", L"白名单", L"状态", L"操作"};
            const int kCols[6] = {36, 248, 220, 62, 110, 88};
            for (int i = 0; i < 6; ++i)
            {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                col.pszText = (LPWSTR)heads[i];
                col.cx = theme::S(kCols[i]);
                col.fmt = (i == 0 || i == 3) ? LVCFMT_CENTER : LVCFMT_LEFT;
                ListView_InsertColumn(g_hRuleList, i, &col);
            }

            g_hActLabel = MakeLabel(parent, L"选中规则", 0, 0, 56, 18, theme::FontSmall(),
                                    theme::TextMuted);
            theme::CreateButton(parent, MainWindow::IDC_BTN_ENABLE_RULE, L"启用",
                                theme::BtnRole::BtnDefault, theme::S(0), theme::S(0), theme::S(64),
                                theme::S(30));
            theme::CreateButton(parent, MainWindow::IDC_BTN_DISABLE_RULE, L"停用",
                                theme::BtnRole::BtnDefault, theme::S(0), theme::S(0), theme::S(64),
                                theme::S(30));
            theme::CreateButton(parent, MainWindow::IDC_BTN_EDIT_RULE, L"编辑",
                                theme::BtnRole::BtnDefault, theme::S(0), theme::S(0), theme::S(64),
                                theme::S(30));
            theme::CreateButton(parent, MainWindow::IDC_BTN_DELETE_RULE, L"删除",
                                theme::BtnRole::BtnDanger, theme::S(0), theme::S(0), theme::S(88),
                                theme::S(30));

            g_hTaskLabel = MakeLabel(parent, L"任务", 0, 0, 56, 18, theme::FontSmall(),
                                     theme::TextMuted);
            theme::CreateButton(parent, MainWindow::IDC_BTN_MIGRATE_RULE, L"开始迁移",
                                theme::BtnRole::BtnPrimary, theme::S(0), theme::S(0), theme::S(96),
                                theme::S(30));
            theme::CreateButton(parent, MainWindow::IDC_BTN_RESTORE_RULE, L"恢复",
                                theme::BtnRole::BtnDefault, theme::S(0), theme::S(0), theme::S(64),
                                theme::S(30));
            theme::CreateButton(parent, MainWindow::IDC_BTN_ADOPT, L"扫描接管…",
                                theme::BtnRole::BtnDefault, theme::S(0), theme::S(0), theme::S(116),
                                theme::S(30));
            theme::CreateButton(parent, MainWindow::IDC_BTN_REBUILD, L"重建链接…",
                                theme::BtnRole::BtnDefault, theme::S(0), theme::S(0), theme::S(108),
                                theme::S(30));
            theme::CreateButton(parent, MainWindow::IDC_BTN_STOP, L"停止",
                                theme::BtnRole::BtnDanger, theme::S(0), theme::S(0), theme::S(72),
                                theme::S(30));

            g_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                                          theme::S(0), theme::S(0), theme::S(300), theme::S(6),
                                          parent, (HMENU)(INT_PTR)MainWindow::IDC_PROGRESS,
                                          g_hInst, nullptr);
            SendMessageW(g_hProgress, PBM_SETBKCOLOR, 0, (LPARAM)theme::SegBg);
            SendMessageW(g_hProgress, PBM_SETBARCOLOR, 0, (LPARAM)theme::Accent);

            g_hEta = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                     theme::S(0), theme::S(0), theme::S(176), theme::S(20), parent,
                                     (HMENU)(INT_PTR)MainWindow::IDC_ETA, g_hInst, nullptr);
            theme::ApplyFont(g_hEta, theme::FontSmall());
            SetLabelColor(g_hEta, theme::TextMuted);

            g_hLogLabel = MakeLabel(parent, L"日志", 0, 0, 120, 18, theme::FontSmall(),
                                    theme::TextMuted);
            g_hLog = CreateWindowExW(0, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                         ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
                                     theme::S(0), theme::S(0), theme::S(300), theme::S(78), parent,
                                     (HMENU)(INT_PTR)MainWindow::IDC_LOG, g_hInst, nullptr);
            theme::ApplyFont(g_hLog, theme::FontMono());

            AddTipById(parent, MainWindow::IDC_BTN_ADD_RULE, L"新建一条迁移规则");
            AddTipById(parent, MainWindow::IDC_BTN_IMPORT, L"从其他目录的配置文件导入规则");
            AddTipById(parent, MainWindow::IDC_BTN_ENABLE_RULE,
                       L"对列表中勾选的规则启用自动迁移");
            AddTipById(parent, MainWindow::IDC_BTN_DISABLE_RULE,
                       L"对列表中勾选的规则停用自动迁移");
            AddTipById(parent, MainWindow::IDC_BTN_EDIT_RULE,
                       L"编辑选中规则（监视/目标目录与白名单）");
            AddTipById(parent, MainWindow::IDC_BTN_DELETE_RULE,
                       L"删除选中规则，并恢复它已迁移的目录");
            AddTipById(parent, MainWindow::IDC_BTN_MIGRATE_RULE,
                       L"迁移选中规则下所有尚未迁移的非白名节目录");
            AddTipById(parent, MainWindow::IDC_BTN_RESTORE_RULE,
                       L"把选中规则已迁移的目录恢复回原位置");
            AddTipById(parent, MainWindow::IDC_BTN_ADOPT,
                       L"目标目录中已有数据、原位置缺少链接时，接管并补建链接");
            AddTipById(parent, MainWindow::IDC_BTN_REBUILD,
                       L"目标目录已有数据但原位置链接丢失时，重建链接");
            AddTipById(parent, MainWindow::IDC_BTN_STOP, L"停止当前正在执行的迁移/恢复任务");

            LayoutRulePage(parent);
        }

        void BuildPlanPage(HWND parent)
        {
            PageHeader(parent, L"计划", L"按计划定时扫描已启用的规则并自动迁移新目录");

            RECT pr{};
            GetClientRect(parent, &pr);
            theme::CreateButton(parent, MainWindow::IDC_BTN_SAVE_PLAN, L"保存设置",
                                theme::BtnRole::BtnPrimary, pr.right - theme::S(40) - theme::S(104),
                                theme::S(34), theme::S(104), theme::S(34));

            MakeLabel(parent, L"定时", 40, 112, 200, 22, theme::FontBodyBold(), theme::Text);
            MakeLabel(parent, L"定时间隔", 40, 148, 64, 20, theme::FontBody(), theme::Text);
            g_hSchedPeriod = MakeCombo(parent, MainWindow::IDC_SCHED_PERIOD, 112, 142, 112, 200);
            SendMessageW(g_hSchedPeriod, CB_ADDSTRING, 0, (LPARAM)L"不定时");
            SendMessageW(g_hSchedPeriod, CB_ADDSTRING, 0, (LPARAM)L"每天");
            SendMessageW(g_hSchedPeriod, CB_ADDSTRING, 0, (LPARAM)L"每周");
            SendMessageW(g_hSchedPeriod, CB_ADDSTRING, 0, (LPARAM)L"每月");
            SendMessageW(g_hSchedPeriod, CB_ADDSTRING, 0, (LPARAM)L"每N天");
            SendMessageW(g_hSchedPeriod, CB_SETCURSEL, 0, 0);

            g_hSchedTimeLabel = MakeLabel(parent, L"时刻", 250, 148, 40, 20, theme::FontBody(),
                                          theme::Text);
            g_hSchedHour = MakeEdit(parent, MainWindow::IDC_SCHED_HOUR, L"2", 292, 142, 48, 30);
            g_hSchedColon = MakeLabel(parent, L":", 346, 148, 10, 20, theme::FontBody(), theme::Text);
            g_hSchedMin = MakeEdit(parent, MainWindow::IDC_SCHED_MIN, L"0", 358, 142, 48, 30);

            g_hSchedWeekLabel = MakeLabel(parent, L"周几", 420, 148, 40, 20, theme::FontBody(),
                                          theme::Text);
            g_hSchedWeekCombo = MakeCombo(parent, MainWindow::IDC_SCHED_WEEK, 462, 142, 88, 200);
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)L"周日");
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)L"周一");
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)L"周二");
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)L"周三");
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)L"周四");
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)L"周五");
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)L"周六");
            SendMessageW(g_hSchedWeekCombo, CB_SETCURSEL, 1, 0);

            g_hSchedDomLabel = MakeLabel(parent, L"每月几号", 420, 148, 62, 20, theme::FontBody(),
                                         theme::Text);
            g_hSchedDom = MakeEdit(parent, MainWindow::IDC_SCHED_DOM, L"1", 484, 142, 48, 30);

            g_hSchedNDaysLabel = MakeLabel(parent, L"每几天", 420, 148, 56, 20, theme::FontBody(),
                                           theme::Text);
            g_hSchedNDays = MakeEdit(parent, MainWindow::IDC_SCHED_N, L"3", 480, 142, 48, 30);

            g_hSchedNext = CreateWindowExW(0, L"STATIC", L"下次运行：未启用定时",
                                           WS_CHILD | WS_VISIBLE | SS_LEFT, theme::S(40),
                                           theme::S(186), theme::S(640), theme::S(20), parent,
                                           (HMENU)(INT_PTR)MainWindow::IDC_SCHED_NEXT, g_hInst,
                                           nullptr);
            theme::ApplyFont(g_hSchedNext, theme::FontSmall());
            SetLabelColor(g_hSchedNext, theme::TextMuted);

            MakeLabel(parent, L"运行参数", 40, 238, 200, 22, theme::FontBodyBold(), theme::Text);
            MakeLabel(parent, L"线程数", 40, 278, 52, 20, theme::FontBody(), theme::Text);
            g_hThreads = MakeEdit(parent, MainWindow::IDC_THREADS, L"4", 96, 272, 64, 30);
            MakeLabel(parent, L"链接类型", 196, 278, 64, 20, theme::FontBody(), theme::Text);
            g_hLinkType = MakeCombo(parent, MainWindow::IDC_LINKTYPE, 264, 272, 200, 120);
            SendMessageW(g_hLinkType, CB_ADDSTRING, 0, (LPARAM)L"符号链接(需管理员)");
            SendMessageW(g_hLinkType, CB_ADDSTRING, 0, (LPARAM)L"目录联接(Junction)");
            SendMessageW(g_hLinkType, CB_SETCURSEL, 0, 0);

            g_hMoveMode = MakeCheck(parent, MainWindow::IDC_MOVEMODE, L"移动模式（迁移后删源）", 40,
                                    318, 200, 24);
            SendMessageW(g_hMoveMode, BM_SETCHECK, BST_CHECKED, 0);
            g_hWatcher = MakeCheck(parent, MainWindow::IDC_WATCHER, L"启用实时监视", 260, 318, 160,
                                   24);
            SendMessageW(g_hWatcher, BM_SETCHECK, BST_CHECKED, 0);

            g_hLockDetect = MakeCheck(parent, MainWindow::IDC_LOCKDETECT, L"检测文件占用", 40, 354,
                                      150, 24);
            SendMessageW(g_hLockDetect, BM_SETCHECK, BST_CHECKED, 0);
            MakeLabel(parent, L"重试间隔（分钟）", 200, 360, 110, 20, theme::FontBody(),
                      theme::Text);
            g_hRetryMin = MakeEdit(parent, MainWindow::IDC_RETRYMIN, L"5", 316, 354, 64, 30);

            MakeLabel(parent,
                      L"说明：定时 = 固定周期 + 固定时刻；「不定时」则只依赖实时监视与手动迁移。\r\n"
                      L"只有已启用的规则才会被自动迁移、定时迁移与实时监视使用。",
                      40, 410, 760, 44, theme::FontSmall(), theme::TextSub);

            AddTipById(parent, MainWindow::IDC_BTN_SAVE_PLAN,
                       L"保存定时与运行参数到配置文件");
            AddTipById(parent, MainWindow::IDC_THREADS, L"同时迁移的目录数；0 表示自动（最多 8）");
            AddTipById(parent, MainWindow::IDC_WATCHER, L"目录一出现就自动迁移");
            AddTipById(parent, MainWindow::IDC_LOCKDETECT,
                       L"迁移前检测文件是否被占用，占用则本次跳过");
        }

        void BuildServicePage(HWND parent)
        {
            PageHeader(parent, L"服务", L"后台服务独立运行，退出本界面后仍会继续自动迁移");

            g_hSvcState = CreateWindowExW(0, L"STATIC", L"未安装", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                          theme::S(40), theme::S(110), theme::S(360), theme::S(44),
                                          parent, (HMENU)(INT_PTR)MainWindow::IDC_SVCSTATE, g_hInst,
                                          nullptr);
            theme::ApplyFont(g_hSvcState, theme::FontTitle());
            SetLabelColor(g_hSvcState, g_svcColor);

            MakeLabel(parent, L"服务名 dirMoverSvc · 开机自动启动", 40, 160, 480, 20,
                      theme::FontSmall(), theme::TextMuted);

            const struct
            {
                int id;
                const wchar_t *text;
            } svc[] = {
                {MainWindow::IDC_BTN_SVCINSTALL, L"安装服务"},
                {MainWindow::IDC_BTN_SVCSTART, L"启动服务"},
                {MainWindow::IDC_BTN_SVCSTOP, L"停止服务"},
                {MainWindow::IDC_BTN_SVCREMOVE, L"卸载服务"},
            };
            int x = 40;
            for (const auto &b : svc)
            {
                theme::CreateButton(parent, b.id, b.text, theme::BtnRole::BtnDefault, theme::S(x),
                                    theme::S(196), theme::S(96), theme::S(34));
                x += 104;
            }

            MakeLabel(parent, L"可执行文件", 40, 264, 120, 20, theme::FontSmall(), theme::TextMuted);
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            MakeLabel(parent, exe, 40, 288, 760, 20, theme::FontSmall(), theme::Text);

            MakeLabel(parent, L"服务日志", 40, 324, 120, 20, theme::FontSmall(), theme::TextMuted);
            g_hSvcLogPath = MakeLabel(parent, L"", 40, 348, 760, 20, theme::FontSmall(), theme::Text);
            SetWindowTextW(g_hSvcLogPath,
                           Path::Combine(Path::ExeDir(), L"dirmv_service.log").c_str());

            theme::CreateButton(parent, MainWindow::IDC_BTN_OPENLOG, L"打开日志文件…",
                                theme::BtnRole::BtnDefault, theme::S(40), theme::S(392), theme::S(136),
                                theme::S(34));
            MakeLabel(parent, L"提示：停止服务会等待当前迁移任务安全收尾，不会丢数据。", 40, 444, 700,
                      20, theme::FontSmall(), theme::TextMuted);

            AddTipById(parent, MainWindow::IDC_BTN_SVCINSTALL, L"安装后台服务并设为开机自动启动（需管理员）");
            AddTipById(parent, MainWindow::IDC_BTN_SVCSTART, L"启动后台服务");
            AddTipById(parent, MainWindow::IDC_BTN_SVCSTOP, L"停止后台服务（等待任务收尾）");
            AddTipById(parent, MainWindow::IDC_BTN_SVCREMOVE, L"卸载后台服务（不影响已迁移的数据）");
            AddTipById(parent, MainWindow::IDC_BTN_OPENLOG, L"用默认程序打开服务日志文件");
        }

        void BuildAboutPage(HWND parent)
        {
            PageHeader(parent, L"关于", L"");

            MakeLabel(parent, L"dirMover", 40, 104, 400, 34, theme::FontApp(), theme::Text);
            std::wstring ver = L"版本 " + GetAppVersion();
            MakeLabel(parent, ver.c_str(), 42, 148, 300, 20, theme::FontSmall(), theme::TextMuted);

            MakeLabel(parent,
                      L"· 将监视目录中的非白名单内容迁移到目标目录，原位置保留链接\r\n"
                      L"· 支持符号链接与目录联接（Junction）\r\n"
                      L"· 实时监视新目录，并按计划定时迁移\r\n"
                      L"· 迁移前检测文件占用，避免正在使用的数据被搬走\r\n"
                      L"· 崩溃残留自动恢复，绝不删除唯一副本\r\n"
                      L"· 每规则独立白名单，配置多位置同步",
                      40, 186, 760, 128, theme::FontSmall(), theme::TextSub);

            MakeLabel(parent, L"配置与日志", 40, 340, 200, 22, theme::FontBodyBold(), theme::Text);
            std::wstring cfg = L"配置文件：" + g_iniPath;
            MakeLabel(parent, cfg.c_str(), 40, 372, 780, 20, theme::FontSmall(), theme::TextSub);
        }

        void RelayoutPage(HWND page)
        {
            if (!page)
                return;
            if (page == g_pageHwnd[PAGE_RULES])
                LayoutRulePage(page);
            InvalidateRect(page, nullptr, TRUE);
        }
    } // namespace

    // ==================== 页面切换 ====================
    namespace
    {
        // 页面子窗口过程：把子控件的 WM_COMMAND / WM_NOTIFY 转发给主窗口，
        // 否则按钮点击只会发给页面窗口，主窗口收不到命令。
        LRESULT CALLBACK PageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            switch (msg)
            {
            case WM_COMMAND:
            case WM_NOTIFY:
                if (g_mainWnd)
                    return SendMessageW(g_mainWnd, msg, wParam, lParam);
                break;
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                if (hwnd == g_pageHwnd[PAGE_RULES])
                    LayoutRulePage(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                RECT rc;
                GetClientRect(hwnd, &rc);
                HBRUSH bg = CreateSolidBrush(theme::Bg);
                FillRect(dc, &rc, bg);
                DeleteObject(bg);
                RECT card{theme::S(16), theme::S(16), rc.right - theme::S(16),
                          rc.bottom - theme::S(16)};
                theme::RoundRectEx(dc, card, theme::S(12), theme::Card, theme::Border, 1);
                FramePageFields(hwnd, dc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_CTLCOLORSTATIC:
            {
                HDC dc = (HDC)wParam;
                HWND ctl = (HWND)lParam;
                SetBkMode(dc, TRANSPARENT);
                COLORREF color = theme::Text;
                HANDLE p = GetPropW(ctl, L"dirmv_clr");
                if (p)
                    color = (COLORREF)((INT_PTR)p - 1);
                else if (GetDlgCtrlID(ctl) == MainWindow::IDC_SVCSTATE)
                    color = g_svcColor;
                SetTextColor(dc, color);
                return (LRESULT)theme::CardBrush();
            }
            case WM_CTLCOLOREDIT:
            {
                HDC dc = (HDC)wParam;
                SetBkColor(dc, theme::InputBg);
                SetTextColor(dc, theme::Text);
                return (LRESULT)theme::InputBrush();
            }
            case WM_CTLCOLORLISTBOX:
            {
                HDC dc = (HDC)wParam;
                SetBkColor(dc, theme::InputBg);
                SetTextColor(dc, theme::Text);
                return (LRESULT)theme::InputBrush();
            }
            case WM_CTLCOLORBTN:
                return (LRESULT)theme::CardBrush();
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        void EnsurePageClass()
        {
            static bool done = false;
            if (done)
                return;
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = PageWndProc;
            wc.hInstance = g_hInst;
            wc.lpszClassName = L"dirMoverPage";
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            RegisterClassExW(&wc);
            done = true;
        }

        HWND CreatePage(HWND parent, int x, int y, int w, int h, bool visible)
        {
            EnsurePageClass();
            return CreateWindowExW(0, L"dirMoverPage", L"",
                                   WS_CHILD | (visible ? WS_VISIBLE : 0), x, y, w, h, parent,
                                   nullptr, g_hInst, nullptr);
        }

        void ShowPage(int page)
        {
            g_curTab = page;
            for (int i = 0; i < kPageCount; ++i)
            {
                if (g_pageHwnd[i])
                    ShowWindow(g_pageHwnd[i], (i == page) ? SW_SHOW : SW_HIDE);
                if (g_hNavButtons[i])
                    theme::SetTabActive(g_hNavButtons[i], i == page);
            }
            if (page == PAGE_PLAN)
                UpdateNextRunLabel();
            if (page == PAGE_SERVICE)
                UpdateServiceState();
            if (g_pageHwnd[page])
            {
                RelayoutPage(g_pageHwnd[page]);
                InvalidateRect(g_pageHwnd[page], nullptr, TRUE);
            }
        }
    } // namespace

    // ==================== 命令处理 ====================
    namespace
    {
        void OnCommand(int id)
        {
            switch (id)
            {
            // 左侧导航
            case kNavIds[0]:
                ShowPage(PAGE_RULES);
                break;
            case kNavIds[1]:
                ShowPage(PAGE_PLAN);
                break;
            case kNavIds[2]:
                ShowPage(PAGE_SERVICE);
                break;
            case kNavIds[3]:
                ShowPage(PAGE_ABOUT);
                break;

            // 规则页
            case MainWindow::IDC_BTN_ADD_RULE:
                AddRuleFlow();
                break;
            case MainWindow::IDC_BTN_EDIT_RULE:
                EditRuleFlow();
                break;
            case MainWindow::IDC_BTN_IMPORT:
                ImportRuleFlow();
                break;
            case MainWindow::IDC_BTN_DELETE_RULE:
                DeleteRuleFlow();
                break;
            case MainWindow::IDC_BTN_ENABLE_RULE:
                SetRulesEnabledFlow(true);
                break;
            case MainWindow::IDC_BTN_DISABLE_RULE:
                SetRulesEnabledFlow(false);
                break;
            case MainWindow::IDC_BTN_ADOPT:
                AdoptFlow();
                break;
            case MainWindow::IDC_BTN_REBUILD:
                RebuildFlow();
                break;
            case MainWindow::IDC_BTN_MIGRATE_RULE:
                StartMigrateCurrentRule();
                break;
            case MainWindow::IDC_BTN_RESTORE_RULE:
                StartRestoreCurrentRule();
                break;
            case MainWindow::IDC_BTN_STOP:
                g_cancel = true;
                AppendLog(L"[用户] 请求停止任务");
                break;

            // 计划页
            case MainWindow::IDC_BTN_SAVE_PLAN:
                ReadPlanIntoConfig();
                SaveConfig();
                UpdateNextRunLabel();
                AppendLog(L"[计划] 设置已保存");
                MessageBoxW(g_mainWnd, L"计划设置已保存。", L"dirMover", MB_ICONINFORMATION);
                break;

            // 关于页（服务）
            case MainWindow::IDC_BTN_SVCINSTALL:
            {
                std::wstring err;
                if (ServiceInstaller::Install(L"dirMoverSvc", L"dirMover 目录迁移服务", &err))
                    MessageBoxW(g_mainWnd, L"服务安装成功（已设为开机自动启动）", L"dirMover", MB_ICONINFORMATION);
                else
                    MessageBoxW(g_mainWnd, err.c_str(), L"安装失败", MB_ICONERROR);
                UpdateServiceState();
                break;
            }
            case MainWindow::IDC_BTN_SVCSTART:
            {
                std::wstring err;
                if (!ServiceInstaller::Start(L"dirMoverSvc", &err))
                    MessageBoxW(g_mainWnd, err.c_str(), L"启动失败", MB_ICONERROR);
                UpdateServiceState();
                break;
            }
            case MainWindow::IDC_BTN_SVCSTOP:
            {
                std::wstring err;
                if (!ServiceInstaller::Stop(L"dirMoverSvc", &err))
                    MessageBoxW(g_mainWnd, err.c_str(), L"停止失败", MB_ICONERROR);
                UpdateServiceState();
                break;
            }
            case MainWindow::IDC_BTN_SVCREMOVE:
            {
                std::wstring err;
                if (ServiceInstaller::Uninstall(L"dirMoverSvc", &err))
                    MessageBoxW(g_mainWnd, L"服务已卸载", L"dirMover", MB_ICONINFORMATION);
                else
                    MessageBoxW(g_mainWnd, err.c_str(), L"卸载失败", MB_ICONERROR);
                UpdateServiceState();
                break;
            }

            case MainWindow::IDC_BTN_OPENLOG:
            {
                std::wstring logPath = Path::Combine(Path::ExeDir(), L"dirmv_service.log");
                HINSTANCE r = ShellExecuteW(nullptr, L"open", logPath.c_str(), nullptr, nullptr,
                                            SW_SHOWNORMAL);
                if ((INT_PTR)r <= 32)
                    MessageBoxW(g_mainWnd, (L"无法打开日志文件：\n" + logPath).c_str(), L"dirMover",
                                MB_ICONWARNING);
                break;
            }

            case TrayIcon::IDM_SHOW:
                ShowWindow(g_mainWnd, SW_SHOW);
                SetForegroundWindow(g_mainWnd);
                break;
            case TrayIcon::IDM_MIGRATE:
                StartMigrateCurrentRule();
                break;
            case TrayIcon::IDM_RESTORE:
                StartRestoreCurrentRule();
                break;
            case TrayIcon::IDM_EXIT:
                DestroyWindow(g_mainWnd);
                break;
            }
        }
    } // namespace

    // ==================== 窗口过程 ====================
    namespace
    {
        LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            switch (msg)
            {
            case WM_GETMINMAXINFO:
            {
                auto *mmi = (MINMAXINFO *)lParam;
                RECT r{0, 0, theme::S(900), theme::S(660)};
                AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
                mmi->ptMinTrackSize.x = r.right - r.left;
                mmi->ptMinTrackSize.y = r.bottom - r.top;
                return 0;
            }
            case WM_CREATE:
            {
                g_mainWnd = hwnd;
                g_font = theme::FontBody();

                EnsureCheckImages();
                RECT crc{};
                GetClientRect(hwnd, &crc);
                const int navW = theme::S(kSidebarW);
                for (int i = 0; i < kPageCount; ++i)
                    g_pageHwnd[i] =
                        CreatePage(hwnd, navW, 0, crc.right - navW, crc.bottom, i == 0);

                BuildRulePage(g_pageHwnd[PAGE_RULES]);
                BuildPlanPage(g_pageHwnd[PAGE_PLAN]);
                BuildServicePage(g_pageHwnd[PAGE_SERVICE]);
                BuildAboutPage(g_pageHwnd[PAGE_ABOUT]);

                // 左侧导航项
                static const wchar_t *navLabels[kPageCount] = {L"规则", L"计划", L"服务", L"关于"};
                static const wchar_t *navTips[kPageCount] = {
                    L"管理迁移规则、手动迁移/恢复与日志",
                    L"定时迁移与线程、链接类型等运行参数",
                    L"后台服务的安装、启停与日志",
                    L"版本与配置文件位置"};
                for (int i = 0; i < kPageCount; ++i)
                {
                    g_hNavButtons[i] = theme::CreateButton(
                        hwnd, kNavIds[i], navLabels[i], theme::BtnRole::BtnNav, theme::S(kNavX),
                        theme::S(kNavTop + i * (kNavH + kNavGap)), theme::S(kNavW),
                        theme::S(kNavH));
                    AddTipById(hwnd, kNavIds[i], navTips[i]);
                }

                WritePlanToUI(g_cfg);
                RefreshRuleList();
                UpdateServiceState();
                ShowPage(PAGE_RULES);

                Logger::Instance().AddSink([](LogLevel, const std::wstring &line)
                                           { PostLog(line); });
                TrayIcon::Add(hwnd, g_hInst, L"dirMover 目录迁移工具");
                AppendLog(L"就绪。配置文件（应用目录）: " + g_iniPath);
                // 每 2 秒刷新一次服务状态（服务可能被 SCM/其他程序在界面外
                // 启动、停止或暂停，必须让「服务」页跟上状态变化）
                SetTimer(hwnd, 1, 2000, nullptr);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                RECT rc;
                GetClientRect(hwnd, &rc);
                HBRUSH bg = CreateSolidBrush(theme::Bg);
                FillRect(dc, &rc, bg);
                DeleteObject(bg);

                // 左侧导航栏
                RECT sb{0, 0, theme::S(kSidebarW), rc.bottom};
                HBRUSH wb = CreateSolidBrush(theme::Card);
                FillRect(dc, &sb, wb);
                DeleteObject(wb);
                theme::Line(dc, theme::S(kSidebarW) - 1, 0, theme::S(kSidebarW) - 1, rc.bottom,
                            theme::Border);

                RECT tr{theme::S(24), theme::S(28), theme::S(kSidebarW) - theme::S(12),
                        theme::S(56)};
                theme::TextIn(dc, tr, L"dirMover", theme::FontTitle(), theme::Text,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE, 0);
                RECT sr{theme::S(25), theme::S(58), theme::S(kSidebarW) - theme::S(12),
                        theme::S(80)};
                theme::TextIn(dc, sr, L"目录迁移 · 链接管理", theme::FontSmall(), theme::TextMuted,
                              DT_LEFT | DT_TOP | DT_SINGLELINE, 0);

                // 底部版本号
                std::wstring ver = L"v" + GetAppVersion();
                RECT vr{theme::S(24), rc.bottom - theme::S(40), theme::S(kSidebarW) - theme::S(16),
                        rc.bottom - theme::S(20)};
                theme::TextIn(dc, vr, ver.c_str(), theme::FontSmall(), theme::TextMuted,
                              DT_LEFT | DT_SINGLELINE, 0);

                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_SIZE:
            {
                RECT crc{};
                GetClientRect(hwnd, &crc);
                const int navW = theme::S(kSidebarW);
                for (int i = 0; i < kPageCount; ++i)
                {
                    if (g_pageHwnd[i])
                        SetWindowPos(g_pageHwnd[i], nullptr, navW, 0, crc.right - navW,
                                     crc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
                }
                for (int i = 0; i < kPageCount; ++i)
                {
                    if (g_hNavButtons[i])
                        SetWindowPos(g_hNavButtons[i], HWND_TOP, theme::S(kNavX),
                                     theme::S(kNavTop + i * (kNavH + kNavGap)), theme::S(kNavW),
                                     theme::S(kNavH), SWP_NOACTIVATE);
                }
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            case WM_NOTIFY:
            {
                LPNMHDR nh = (LPNMHDR)lParam;
                if (nh->idFrom == MainWindow::IDC_RULE_LIST && nh->code == NM_CUSTOMDRAW)
                {
                    LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)lParam;
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
                        return CDRF_NOTIFYITEMDRAW;
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                    {
                        bool sel = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;
                        cd->clrTextBk = sel ? theme::AccentSoft : theme::Card;
                        cd->clrText = sel ? theme::AccentPressed : theme::Text;
                        return CDRF_NEWFONT;
                    }
                    return CDRF_DODEFAULT;
                }
                else if (nh->idFrom == MainWindow::IDC_RULE_LIST)
                {
                    if (nh->code == LVN_ITEMCHANGED)
                    {
                        // 仅当「选中状态发生变化」时才更新（避免勾选/图标变化误触发）
                        LPNMLISTVIEW nlv = (LPNMLISTVIEW)lParam;
                        if ((nlv->uChanged & LVIF_STATE) &&
                            (nlv->uNewState & LVIS_SELECTED) != (nlv->uOldState & LVIS_SELECTED))
                        {
                            g_selectedRule = ListView_GetNextItem(g_hRuleList, -1, LVNI_SELECTED);
                            UpdateRuleButtons();
                        }
                    }
                    else if (nh->code == NM_CLICK || nh->code == NM_DBLCLK)
                    {
                        LPNMITEMACTIVATE nia = (LPNMITEMACTIVATE)lParam;
                        int row = nia->iItem;
                        int col = nia->iSubItem;
                        if (nh->code == NM_DBLCLK && row >= 0)
                        {
                            // 双击行 → 编辑该规则
                            g_selectedRule = row;
                            EditRuleFlow();
                        }
                        else if (nh->code == NM_CLICK && row >= 0 && col == 0)
                        {
                            // 点击复选框列 → 切换该行勾选（仅切换勾选，不改变「操作对象」）
                            if (row < (int)g_ruleChecked.size())
                            {
                                g_ruleChecked[row] = !g_ruleChecked[row];
                                RefreshRuleList();
                            }
                        }
                        else if (nh->code == NM_CLICK && row >= 0 && col == 5)
                        {
                            // 点击「操作」列（编辑）→ 编辑本行规则
                            g_selectedRule = row;
                            EditRuleFlow();
                        }
                    }
                    else if (nh->code == LVN_COLUMNCLICK)
                    {
                        LPNMLISTVIEW nv = (LPNMLISTVIEW)lParam;
                        if (nv->iSubItem == 0) // 点击表头复选框列 → 全选/全不选
                            SetAllChecked(!IsAllChecked());
                    }
                }
                return 0;
            }
            case WM_COMMAND:
            {
                int id = LOWORD(wParam);
                if (HIWORD(wParam) == BN_CLICKED || HIWORD(wParam) == 0 || HIWORD(wParam) == CBN_SELCHANGE)
                {
                    if (id == MainWindow::IDC_SCHED_PERIOD)
                    {
                        UpdateScheduleVisibility();
                        UpdateNextRunLabel();
                    }
                    else
                        OnCommand(id);
                }
                return 0;
            }
            case MainWindow::WM_APP_LOG:
            {
                auto *s = reinterpret_cast<std::wstring *>(lParam);
                if (s)
                {
                    AppendLog(*s);
                    delete s;
                }
                return 0;
            }
            case MainWindow::WM_APP_DONE:
            {
                if (wParam == 100)
                {
                    auto *info = reinterpret_cast<std::pair<int, std::wstring> *>(lParam);
                    if (info)
                        OnChangeTargetDone(info);
                }
                else if (wParam == 101)
                {
                    auto *info = reinterpret_cast<std::pair<int, std::wstring> *>(lParam);
                    if (info)
                        OnRebuildDone(info);
                }
                else if (wParam == 102)
                {
                    auto *info = reinterpret_cast<std::pair<int, MigrateEngine::RestoreReport> *>(lParam);
                    if (info)
                        OnDeleteRestoreDone(info);
                }
                else
                {
                    auto *r = reinterpret_cast<MigrateResult *>(lParam);
                    if (r)
                        OnTaskDone(r);
                }
                return 0;
            }
            case MainWindow::WM_APP_LOCK:
            {
                auto *n = reinterpret_cast<LockNotice *>(lParam);
                if (n)
                {
                    ShowLockNotice(*n);
                    delete n;
                }
                return 0;
            }
            case TrayIcon::WM_TRAYICON:
                TrayIcon::HandleMessage(hwnd, wParam, lParam);
                return 0;
            case WM_TIMER:
                if (wParam == 1)
                {
                    HWND svc = g_pageHwnd[PAGE_SERVICE];
                    if (svc && IsWindowVisible(svc))
                        UpdateServiceState();
                }
                return 0;
            case WM_CLOSE:
                ShowWindow(hwnd, SW_HIDE);
                TrayIcon::Notify(hwnd, L"dirMover", L"已最小化到系统托盘，双击图标恢复窗口");
                return 0;
            case WM_DESTROY:
                TrayIcon::Remove(hwnd);
                g_cancel = true;
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    } // namespace

    // ---------------------------------------------------------------------------
    void MainWindow::Register(HINSTANCE hInst)
    {
        g_hInst = hInst;
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES;
        InitCommonControlsEx(&icc);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = MainWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"dirMoverMainWnd";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
        wc.hIconSm = wc.hIcon;
        RegisterClassExW(&wc);
    }

    HWND MainWindow::Create(HINSTANCE hInst, int nCmdShow)
    {
        g_iniPath = ConfigStore::DefaultPath();
        if (!ConfigStore::Load(g_iniPath, g_cfg))
        {
            g_cfg.ApplyDefaults();
        }

        const DWORD style = WS_OVERLAPPEDWINDOW | WS_THICKFRAME | WS_MAXIMIZEBOX;
        RECT rc{0, 0, theme::S(1000), theme::S(740)};
        AdjustWindowRectEx(&rc, style, FALSE, 0);
        HWND hwnd = CreateWindowExW(
            0, L"dirMoverMainWnd", L"dirMover - 目录软链接迁移/恢复工具", style, CW_USEDEFAULT,
            CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInst,
            nullptr);
        if (!hwnd)
            return nullptr;

        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);
        return hwnd;
    }

} // namespace dirmv::gui
