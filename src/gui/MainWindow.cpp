#include "gui/MainWindow.h"
#include "gui/I18n.h"
#include "gui/ProgressDialog.h"
#include "gui/TrayIcon.h"
#include "gui/App.h"
#include "gui/Theme.h"
#include "core/PathUtil.h"
#include "core/Log.h"
#include "core/MigrateEngine.h"
#include "core/FileEngine.h"
#include "core/Scheduler.h"
#include "core/PipeServer.h"
#include "service/ServiceMain.h"

#include <windows.h>
#include <winver.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shldisp.h>
#include <shellapi.h>
#include <oleauto.h>
#include <algorithm>
#include <thread>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace dirmv::gui
{
namespace
{
} // namespace

    namespace
    {
        HINSTANCE g_hInst = nullptr;
        Config g_cfg;
        std::wstring g_iniPath;

        HWND g_hNavButtons[5] = {nullptr, nullptr, nullptr, nullptr, nullptr}; // 左侧导航按钮
        HWND g_hRuleList = nullptr;
        HWND g_hLog = nullptr;
        HWND g_hProgress = nullptr;
        HWND g_hEta = nullptr;
        HWND g_hTaskLabel = nullptr;
        HWND g_hLogLabel = nullptr;
        HWND g_hSvcLogPath = nullptr; // 服务页日志路径
        // 分页子窗口（0=规则 1=任务 2=大小 3=计划 4=设置），提前声明供按钮查找使用
        HWND g_pageHwnd[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};

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
        HWND g_hLangCombo = nullptr;
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
        const int PAGE_TASK = 1;
        const int PAGE_SIZE = 2;
        const int PAGE_PLAN = 3;
        const int PAGE_SETTINGS = 4;
        // 设置页同时承载「运行参数 / 后台服务 / 关于」，保留旧名以兼容引用
        const int PAGE_SERVICE = PAGE_SETTINGS;
        const int PAGE_ABOUT = PAGE_SETTINGS;

        void SetFont(HWND h)
        {
            if (g_font)
                SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
        }

        void SetLabelColor(HWND h, COLORREF c); // 定义见下方布局段

        // 消息框统一入口：界面文案跟随当前语言（动态内容原样保留）。
        int MBox(HWND owner, const std::wstring &text, const wchar_t *caption, UINT flags)
        {
            return MessageBoxW(owner, i18n::TranslateText(text).c_str(), i18n::T(caption), flags);
        }
        int MBox(HWND owner, const wchar_t *text, const wchar_t *caption, UINT flags)
        {
            return MessageBoxW(owner, i18n::T(text), i18n::T(caption), flags);
        }

        // 记录所有 tooltip（控件 + 中文原文），切换语言时统一刷新。
        struct TipEntry
        {
            HWND ctl;
            std::wstring text;
        };
        std::vector<TipEntry> g_tips;
        HWND g_hTip = nullptr;
        void AddTipById(HWND parent, int id, const wchar_t *text);
        void UpdateAllTips();

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

        // 从目标目录生成规则：填「已有数据的目标目录」与「链接要建到的新监视目录」。
        // 返回 true=确定；watch/target 就地更新。
        bool GenerateRuleDialog(HWND owner, std::wstring &watch, std::wstring &target)
        {
            static const wchar_t *kCls = L"dirMoverGenRuleDlg";
            static bool registered = false;
            struct Ctx
            {
                bool ok = false;
                std::wstring *watch = nullptr;
                std::wstring *target = nullptr;
                HWND editWatch = nullptr;
                HWND editTarget = nullptr;
            };
            Ctx ctx;
            ctx.watch = &watch;
            ctx.target = &target;

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
                        auto label = [&](const wchar_t *text, int y)
                        {
                            HWND h = CreateWindowExW(0, L"STATIC", text,
                                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                                     theme::S(18), theme::S(y), theme::S(520),
                                                     theme::S(20), hwnd, nullptr, nullptr, nullptr);
                            theme::ApplyFont(h, theme::FontBody());
                            SetPropW(h, L"dirmv_clr", (HANDLE)(INT_PTR)(theme::Text + 1));
                        };
                        auto edit = [&](int id, int y)
                        {
                            HWND h = CreateWindowExW(0, L"EDIT", L"",
                                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                                     theme::S(18), theme::S(y), theme::S(446),
                                                     theme::S(30), hwnd, (HMENU)(INT_PTR)id,
                                                     nullptr, nullptr);
                            theme::ApplyFont(h, theme::FontBody());
                            SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                                         MAKELPARAM(theme::S(6), theme::S(6)));
                            return h;
                        };
                        label(L"目标目录（已经存在的数据目录）", 16);
                        c->editTarget = edit(5001, 40);
                        theme::CreateButton(hwnd, 5003, L"浏览…", theme::BtnRole::BtnGhost,
                                            theme::S(470), theme::S(40), theme::S(72), theme::S(30));
                        label(L"新监视目录（链接将建在这里，之后由后台服务管理）", 82);
                        c->editWatch = edit(5002, 106);
                        theme::CreateButton(hwnd, 5004, L"浏览…", theme::BtnRole::BtnGhost,
                                            theme::S(470), theme::S(106), theme::S(72), theme::S(30));
                        HWND tip = CreateWindowExW(0, L"STATIC",
                                                   L"将扫描目标目录下的一级子文件夹，在新监视目录中逐个建立指向目标目录的链接，"
                                                   L"并把它们纳入规则清单。目标目录里已有的数据不会被移动或删除。",
                                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                                   theme::S(18), theme::S(150), theme::S(524),
                                                   theme::S(60), hwnd, nullptr, nullptr, nullptr);
                        theme::ApplyFont(tip, theme::FontSmall());
                        SetPropW(tip, L"dirmv_clr", (HANDLE)(INT_PTR)(theme::TextMuted + 1));
                        theme::CreateButton(hwnd, 1, L"生成规则", theme::BtnRole::BtnPrimary,
                                            theme::S(304), theme::S(224), theme::S(110), theme::S(34));
                        theme::CreateButton(hwnd, 2, L"取消", theme::BtnRole::BtnDefault,
                                            theme::S(422), theme::S(224), theme::S(110), theme::S(34));
                        SetWindowTextW(c->editTarget, c->target->c_str());
                        SetWindowTextW(c->editWatch, c->watch->c_str());
                        SetFocus(c->editTarget);
                        return 0;
                    }
                    case WM_COMMAND:
                    {
                        int id = LOWORD(wp);
                        if (id == 5003 || id == 5004)
                        {
                            HWND edit = (id == 5003) ? c->editTarget : c->editWatch;
                            std::wstring cur = GetText(edit);
                            std::wstring picked = PickFolder(hwnd, id == 5003 ? L"选择已有数据的目标目录"
                                                                              : L"选择链接要建到的新监视目录");
                            if (!picked.empty())
                                SetWindowTextW(edit, picked.c_str());
                            else if (!cur.empty())
                                SetWindowTextW(edit, cur.c_str());
                            return 0;
                        }
                        if (id == 1)
                        {
                            std::wstring t = Path::Normalize(GetText(c->editTarget));
                            std::wstring w = Path::Normalize(GetText(c->editWatch));
                            if (t.empty() || w.empty())
                            {
                                MBox(hwnd, L"请同时填写目标目录与新监视目录。", L"生成规则",
                                            MB_ICONINFORMATION);
                                return 0;
                            }
                            *c->target = t;
                            *c->watch = w;
                            c->ok = true;
                            DestroyWindow(hwnd);
                            return 0;
                        }
                        if (id == 2)
                        {
                            DestroyWindow(hwnd);
                            return 0;
                        }
                        return 0;
                    }
                    case WM_CLOSE:
                        DestroyWindow(hwnd);
                        return 0;
                    case WM_ERASEBKGND:
                    {
                        RECT rc;
                        GetClientRect(hwnd, &rc);
                        FillRect((HDC)wp, &rc, theme::CardBrush());
                        return 1;
                    }
                    case WM_CTLCOLORSTATIC:
                    {
                        HDC dc = (HDC)wp;
                        SetBkMode(dc, TRANSPARENT);
                        SetTextColor(dc, theme::TextSub);
                        return (LRESULT)theme::CardBrush();
                    }
                    case WM_CTLCOLOREDIT:
                    {
                        HDC dc = (HDC)wp;
                        SetBkColor(dc, theme::InputBg);
                        SetTextColor(dc, theme::Text);
                        return (LRESULT)theme::InputBrush();
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
            HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kCls, L"从目标目录生成规则",
                                       WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                       rc.left + theme::S(60), rc.top + theme::S(40),
                                       theme::S(560), theme::S(300), owner, nullptr, g_hInst, &ctx);
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

        // ---- 规则列表 ----
        // 复选框图像：0=未选 1=已选
        
        // 规则状态：不仅显示启用/停用，还反映后台服务是否真的在监视
        std::wstring RuleStatusText(const Rule &r)
        {
            if (r.pendingDelete)
                return i18n::T(L"待删除(还原失败)");
            if (!r.enabled)
                return i18n::T(L"已停用");
            if (!g_cfg.ValidateOneRule(r).empty())
                return i18n::T(L"不可用");
            int svc = ServiceInstaller::QueryState(L"dirMoverSvc");
            if (svc == 1)
                return i18n::T(L"已启用 · 运行中");
            if (svc == 2 || svc == 3)
                return i18n::T(L"已启用 · 服务切换中");
            if (svc == -2)
                return i18n::T(L"已启用 · 服务未安装");
            return i18n::T(L"已启用 · 后台已停止");
        }

        // 只刷新「状态」列（服务状态变化时由 WM_TIMER 调用）
        void UpdateRuleStatuses()
        {
            if (!g_hRuleList)
                return;
            int n = (int)g_cfg.rules.size();
            if (ListView_GetItemCount(g_hRuleList) != n)
                return;
            for (int i = 0; i < n; ++i)
            {
                std::wstring st = RuleStatusText(g_cfg.rules[i]);
                ListView_SetItemText(g_hRuleList, i, 4, const_cast<LPWSTR>(st.c_str()));
            }
        }

        void RefreshRuleList()
        {
            if (!g_hRuleList)
                return;
            std::wstring selWatch;
            if (g_selectedRule >= 0 && g_selectedRule < (int)g_cfg.rules.size())
                selWatch = g_cfg.rules[g_selectedRule].watchDir;

            SendMessageW(g_hRuleList, LVM_DELETEALLITEMS, 0, 0);
            for (size_t i = 0; i < g_cfg.rules.size(); ++i)
            {
                const Rule &r = g_cfg.rules[i];
                LVITEMW it{};
                it.mask = LVIF_TEXT;
                it.iItem = (int)i;
                it.iSubItem = 0;
                it.pszText = const_cast<LPWSTR>(r.watchDir.c_str());
                SendMessageW(g_hRuleList, LVM_INSERTITEMW, 0, (LPARAM)&it);

                std::wstring tgt = r.targetDir.empty() ? L"(未设置)" : r.targetDir;
                ListView_SetItemText(g_hRuleList, (int)i, 1, const_cast<LPWSTR>(tgt.c_str()));

                std::wstring ex = std::to_wstring(r.exclude.size());
                ListView_SetItemText(g_hRuleList, (int)i, 2, const_cast<LPWSTR>(ex.c_str()));

                std::wstring mv = std::to_wstring(r.migrated.size());
                ListView_SetItemText(g_hRuleList, (int)i, 3, const_cast<LPWSTR>(mv.c_str()));

                std::wstring st = RuleStatusText(r);
                ListView_SetItemText(g_hRuleList, (int)i, 4, const_cast<LPWSTR>(st.c_str()));

                ListView_SetItemText(g_hRuleList, (int)i, 5, const_cast<LPWSTR>(i18n::T(L"详情")));
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

        }

        // 当前勾选的行索引
        
        
        

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
            const wchar_t *txt = i18n::T(L"未知");
            COLORREF color = theme::TextSub;
            switch (st)
            {
            case -2:
                txt = i18n::T(L"未安装");
                color = theme::TextSub;
                break;
            case -1:
                txt = i18n::T(L"无法查询（权限不足）");
                color = theme::Warning;
                break;
            case 0:
                txt = i18n::T(L"已安装（已停止）");
                color = theme::TextSub;
                break;
            case 1:
                txt = i18n::T(L"运行中");
                color = theme::Success;
                break;
            case 3:
                txt = i18n::T(L"已暂停");
                color = theme::Warning;
                break;
            case 2:
                txt = i18n::T(L"状态切换中");
                color = theme::TextSub;
                break;
            default:
                txt = i18n::T(L"未知");
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
        // 返回：0=取消，1=已保存，2=请求删除该规则（调用方负责执行删除流程）
        static Rule ReadRuleFromDialog(HWND dlg, const Rule &base, bool isNew)
        {
            Rule out = base;
            if (isNew)
                out.watchDir = Path::Normalize(Path::Trim(GetText(GetDlgItem(dlg, 1))));
            out.targetDir = Path::Normalize(Path::Trim(GetText(GetDlgItem(dlg, 2))));
            if (HWND cb = GetDlgItem(dlg, 104))
                out.enabled = (SendMessageW(cb, BM_GETCHECK, 0, 0) == BST_CHECKED);

            out.exclude.clear();
            HWND lstExcl = GetDlgItem(dlg, 4);
            int nEx = (int)SendMessageW(lstExcl, LB_GETCOUNT, 0, 0);
            for (int i = 0; i < nEx; ++i)
            {
                int len = (int)SendMessageW(lstExcl, LB_GETTEXTLEN, i, 0);
                std::wstring s(len, L'\0');
                SendMessageW(lstExcl, LB_GETTEXT, i, (LPARAM)&s[0]);
                if (!s.empty())
                    out.exclude.push_back(s);
            }
            return out;
        }

        // 关闭/取消时比较「界面当前值」与「打开时的数据」，而不是一有输入就视为脏
        static bool RuleDialogModified(HWND dlg, const Rule &base, bool isNew)
        {
            Rule cur = ReadRuleFromDialog(dlg, base, isNew);
            if (cur.enabled != base.enabled)
                return true;
            if (Path::ToLower(Path::Normalize(cur.targetDir)) !=
                Path::ToLower(Path::Normalize(base.targetDir)))
                return true;
            if (isNew && Path::ToLower(Path::Normalize(cur.watchDir)) !=
                             Path::ToLower(Path::Normalize(base.watchDir)))
                return true;
            if (cur.exclude.size() != base.exclude.size())
                return true;
            std::vector<std::wstring> a, b;
            a.reserve(cur.exclude.size());
            b.reserve(base.exclude.size());
            for (auto &s : cur.exclude)
                a.push_back(Path::ToLower(s));
            for (auto &s : base.exclude)
                b.push_back(Path::ToLower(s));
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            return a != b;
        }

        int RuleEditDialog(HWND owner, Rule &rule, bool isNew)
        {
            struct DlgCtx
            {
                Rule *rule;
                bool ok;
                bool isNew;
                bool del;   // 是否请求删除
            };
            DlgCtx ctx{&rule, false, isNew, false};


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
                        if (!c->isNew)
                            B(103, L"删除此规则", 12, 316, 116, 30, theme::BtnRole::BtnDanger);
                        {
                            HWND cb = CreateWindowExW(0, L"BUTTON", L"启用此规则",
                                                   WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                                   theme::S(186), theme::S(322), theme::S(140),
                                                   theme::S(22), hwnd, (HMENU)104, g_hInst, nullptr);
                            theme::ApplyFont(cb, theme::FontBody());
                            SendMessageW(cb, BM_SETCHECK, c->rule->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
                        }
                        return 0;
                    }
                    case WM_COMMAND:
                    {
                        int id = LOWORD(wp);
                        int code = HIWORD(wp);
                        auto *cc = (DlgCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

                        // 「删除此规则」：只负责确认并把请求交回调用方（还原+删除由 DeleteRuleFlow 执行）
                        if (id == 103 && code == BN_CLICKED)
                        {
                            std::wstring name = (cc && cc->rule) ? cc->rule->watchDir : L"";
                            std::wstring text =
                                L"删除规则前会先把已迁移的目录还原回原位置。\r\n\r\n" + name +
                                L"\r\n\r\n确定删除该规则？";
                            if (MBox(hwnd, text.c_str(), L"删除规则",
                                            MB_YESNO | MB_ICONWARNING) == IDYES)
                            {
                                cc->del = true;
                                cc->ok = false;
                                DestroyWindow(hwnd);
                            }
                            return 0;
                        }

                        // 先处理「保存 / 取消」——注意 监视/目标 Edit 的 id 分别是 1/2，
                        // 与 IDOK(1)/IDCANCEL(2) 数值相同；用通知码(BN_CLICKED)区分按钮。
                        if (id == IDOK && code == BN_CLICKED)
                        {
                            Rule tmp = ReadRuleFromDialog(hwnd, *cc->rule, cc->isNew);

                            if (tmp.watchDir.empty())
                            {
                                MBox(hwnd, L"请设置监视目录。", L"规则", MB_ICONWARNING);
                                return 0;
                            }
                            if (tmp.targetDir.empty())
                            {
                                if (MBox(hwnd, L"目标目录为空，是否仍保存？\n（保存后该规则不会执行迁移）",
                                                L"规则", MB_YESNO | MB_ICONQUESTION) != IDYES)
                                    return 0;
                            }
                            *cc->rule = tmp;
                            cc->ok = true;
                            DestroyWindow(hwnd);
                            return 0;
                        }
                        if (id == IDCANCEL && code == BN_CLICKED)
                        {
                            // 只有界面值与打开时的数据确实不同才提示保存
                            if (cc && RuleDialogModified(hwnd, *cc->rule, cc->isNew))
                            {
                                int r = MBox(hwnd, L"有未保存的修改，是否保存？",
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
                            }
                        }
                        else if (id == 1 || id == 2)
                        {
                            // 文本框编辑：关闭时统一比较，无需逐键标记
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
                            }
                        }
                        return 0;
                    }
                    case WM_CLOSE:
                    {
                        // 关闭按钮 / 标题栏 X：等同取消（有修改则提示）
                        auto *cc2 = (DlgCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
                        if (cc2 && RuleDialogModified(hwnd, *cc2->rule, cc2->isNew))
                        {
                            int r = MBox(hwnd,
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
                                       isNew ? L"规则详情 · 新建" : L"规则详情",
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
            return ctx.del ? 2 : (ctx.ok ? 1 : 0);
        }

        // ---- 规则操作流程 ----
        void AddRuleFlow()
        {
            Rule r;
            r.watchDir.clear();
            r.targetDir.clear();
            r.exclude.clear(); // 白名单默认为空；若监视目录是用户主目录则在保存后自动补默认项
            r.enabled = true;

            if (RuleEditDialog(g_mainWnd, r, true) != 1)
                return;

            // 监视目录是当前用户主目录时，自动补上系统/特殊文件夹白名单，
            // 避免把桌面、文档、下载等目录直接迁移走；其他目录默认白名单为空。
            wchar_t prof[MAX_PATH]{};
            DWORD pn = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
            if (pn > 0 && pn < MAX_PATH && Path::Equals(r.watchDir, prof) && r.exclude.empty())
                r.exclude = Config::DefaultExclude();

            g_cfg.rules.push_back(r);

            std::wstring err = g_cfg.ValidateRules();
            if (!err.empty())
            {
                g_cfg.rules.pop_back();
                MBox(g_mainWnd, err.c_str(), L"规则无效", MB_ICONWARNING);
                return;
            }

            SaveConfig();
            RefreshRuleList();
            AppendLog(L"[规则] 已添加: " + r.watchDir + L" -> " + r.targetDir +
                      L"（白名单 " + std::to_wstring(r.exclude.size()) + L" 项）");
            MBox(g_mainWnd, L"规则已添加并启用。点击「开始迁移」执行迁移。",
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
                MBox(g_mainWnd, err.c_str(), L"导入规则", MB_ICONWARNING);
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
                    MBox(g_mainWnd,
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
            if (MBox(g_mainWnd, preview.c_str(), L"导入规则",
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
            MBox(g_mainWnd, done.c_str(), L"导入规则", MB_ICONINFORMATION);
        }

        // 修改选中规则（弹出窗口；监视目录只读，可改目标目录/白名单）
        void DeleteRuleFlow(bool alreadyConfirmed);

        void EditRuleFlow()
        {
            if (g_selectedRule < 0 || g_selectedRule >= (int)g_cfg.rules.size())
            {
                MBox(g_mainWnd, L"请先在列表中选择一条规则。", L"修改规则", MB_ICONINFORMATION);
                return;
            }
            Rule r = g_cfg.rules[g_selectedRule];
            Rule before = r;

            int dlgResult = RuleEditDialog(g_mainWnd, r, false);
            if (dlgResult == 2)
            {
                DeleteRuleFlow(true);
                return;
            }
            if (dlgResult != 1)
                return;

            bool targetChanged = !Path::Equals(before.targetDir, r.targetDir);

            if (targetChanged && !before.targetDir.empty() && !r.targetDir.empty())
            {
                if (MBox(g_mainWnd,
                                (L"目标目录已更改：\r\n\r\n旧：" + before.targetDir +
                                 L"\r\n新：" + r.targetDir +
                                 L"\r\n\r\n是否把旧目标的数据整体移动到新目标并重建链接？")
                                    .c_str(),
                                L"修改规则", MB_YESNO | MB_ICONQUESTION) == IDYES)
                {
                    MigrateEngine engine(g_cfg);
                    std::wstring err = engine.ChangeTargetDir(before, r.targetDir, nullptr, nullptr);
                    if (!err.empty())
                        MBox(g_mainWnd, err.c_str(), L"更换目标目录失败", MB_ICONERROR);
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
                    MBox(g_mainWnd, (L"白名单还原异常：\r\n" + log).c_str(),
                                L"修改规则", MB_ICONWARNING);
            }

            g_cfg.rules[g_selectedRule] = r;
            SaveConfig();
            RefreshRuleList();
            AppendLog(L"[规则] 已更新: " + r.watchDir + L" -> " + r.targetDir);
        }

        void DeleteRuleFlow(bool alreadyConfirmed)
        {
            if (g_selectedRule < 0 || g_selectedRule >= (int)g_cfg.rules.size())
            {
                MBox(g_mainWnd, L"请先在列表中选择一条规则。", L"删除规则", MB_ICONINFORMATION);
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

            if (!alreadyConfirmed &&
                MBox(g_mainWnd, prompt.c_str(), L"删除规则",
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
                MBox(g_mainWnd,
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
                MBox(g_mainWnd, detail.c_str(), L"删除规则未完成", MB_ICONWARNING);
            }
        }

        // 统一设置启用/停用：作用于「勾选的行」，若无勾选则作用于当前选中行。
        // 启用时对每条规则做可用性校验：不可用的规则不能启用（跳过），但不影响其他规则。
        void SetRulesEnabledFlow(bool enable)
        {
            std::vector<int> rows;
            if (rows.empty())
            {
                if (g_selectedRule >= 0 && g_selectedRule < (int)g_cfg.rules.size())
                    rows.push_back(g_selectedRule);
            }
            if (rows.empty())
            {
                MBox(g_mainWnd, L"请先勾选或选中要操作的规则。",
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
                MBox(g_mainWnd, m.c_str(), L"启用规则", MB_ICONWARNING);
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
            std::vector<int> idxs;
            for (size_t i = 0; i < g_cfg.rules.size(); ++i)
            {
                const Rule &r = g_cfg.rules[i];
                if (r.enabled && !r.pendingDelete && !r.targetDir.empty())
                    idxs.push_back((int)i);
            }
            if (idxs.empty())
            {
                MBox(g_mainWnd, L"没有已启用且设置了目标目录的规则。", L"扫描接管",
                            MB_ICONINFORMATION);
                return;
            }
            MigrateEngine engine(g_cfg);
            int adopted = 0, foreign = 0, plain = 0;
            std::wstring detail;
            for (int i : idxs)
            {
                AdoptResult ar = engine.AdoptExisting(g_cfg.rules[i]);
                adopted += (int)ar.adopted.size();
                foreign += (int)ar.foreignLinks.size();
                plain += (int)ar.plainDirs.size();
                for (auto &n : ar.adopted)
                    detail += L"  + " + n + L"\n";
            }
            SaveConfig();
            RefreshRuleList();

            std::wstring msg = L"扫描已有迁移 完成：\n\n";
            msg += L"新增纳入 " + std::to_wstring(adopted) + L" 个已迁移目录\n";
            if (!detail.empty())
                msg += detail;
            msg += L"\n普通目录 " + std::to_wstring(plain) + L" 个\n";
            if (foreign > 0)
                msg += L"\n[提醒] 发现 " + std::to_wstring(foreign) +
                       L" 个指向其他目标目录的链接，请确认是否需要处理。\n";
            AppendLog(L"[扫描已有迁移] 新增 " + std::to_wstring(adopted) + L" 个，外部链接 " +
                      std::to_wstring(foreign) + L" 个");
            MBox(g_mainWnd, msg.c_str(), L"扫描已有迁移",
                        foreign == 0 ? MB_ICONINFORMATION : MB_ICONWARNING);
        }

        // ---- 从目标目录生成规则 ----
        // 数据已经在目标目录（例如拷贝过来的旧数据），在新监视目录里建链接，
        // 同时把该目录纳入规则，之后由后台服务继续管理。
        void GenerateRuleFlow()
        {
            std::wstring watch, target;
            if (g_selectedRule >= 0 && g_selectedRule < (int)g_cfg.rules.size())
            {
                target = g_cfg.rules[g_selectedRule].targetDir;
                watch = g_cfg.rules[g_selectedRule].watchDir;
            }
            if (!GenerateRuleDialog(g_mainWnd, watch, target))
                return;
            if (!Path::DirectoryExists(target))
            {
                MBox(g_mainWnd, (L"目标目录不存在：\r\n" + target).c_str(), L"生成规则",
                            MB_ICONWARNING);
                return;
            }

            // 优先复用「目标目录+监视目录」都相同的规则；否则复用目标目录相同的规则
            int idx = -1;
            for (int i = 0; i < (int)g_cfg.rules.size(); ++i)
            {
                if (Path::Equals(g_cfg.rules[i].targetDir, target) &&
                    Path::Equals(g_cfg.rules[i].watchDir, watch))
                {
                    idx = i;
                    break;
                }
            }
            if (idx < 0)
            {
                for (int i = 0; i < (int)g_cfg.rules.size(); ++i)
                {
                    if (Path::Equals(g_cfg.rules[i].targetDir, target))
                    {
                        idx = i;
                        break;
                    }
                }
            }

            Rule rule = (idx >= 0) ? g_cfg.rules[idx] : Rule();
            rule.watchDir = watch;
            rule.targetDir = target;
            rule.enabled = true;
            rule.pendingDelete = false;
            rule.pendingDeleteFailed.clear();
            rule.migrated.clear();
            for (auto &s : Path::ListSubDirectories(target))
                rule.migrated.push_back(s);
            if (rule.migrated.empty())
            {
                MBox(g_mainWnd, L"目标目录下没有子文件夹，无法生成规则。", L"生成规则",
                            MB_ICONINFORMATION);
                return;
            }

            if (idx < 0)
            {
                g_cfg.rules.push_back(rule);
                idx = (int)g_cfg.rules.size() - 1;
            }
            else
            {
                g_cfg.rules[idx] = rule;
            }
            g_selectedRule = idx;
            SaveConfig();
            RefreshRuleList();
            AppendLog(L"[生成规则] " + watch + L" -> " + target + L"，共 " +
                      std::to_wstring(rule.migrated.size()) + L" 个目录");

            if (MBox(g_mainWnd,
                            (L"将为目标目录中的 " + std::to_wstring(rule.migrated.size()) +
                             L" 个子文件夹在新位置建立链接：\r\n\r\n目标目录：" + target +
                             L"\r\n新监视目录：" + watch +
                             L"\r\n\r\n继续？（目标数据不会被移动或删除）")
                                .c_str(),
                            L"生成规则", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return;

            g_busy = true;
            Rule ruleCopy = rule;
            HWND dlg = ProgressDialog::Show(g_mainWnd, L"生成规则并重建链接", false);
            g_progressDlg = dlg;
            std::thread th([ruleCopy, watch, dlg]()
                           {
                MigrateEngine eng(g_cfg);
                std::wstring err = eng.RebuildLinksFromTarget(ruleCopy, watch,
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
                AppendLog(L"[生成规则] 失败: " + info->second);
                MBox(g_mainWnd, info->second.c_str(), L"生成规则失败", MB_ICONERROR);
            }
            else
            {
                AppendLog(L"[生成规则] 完成，规则已生成并交给后台服务管理");
                MBox(g_mainWnd,
                            L"规则已生成，链接已重建。\r\n之后该目录会由后台服务按计划继续管理。",
                            L"生成规则完成", MB_ICONINFORMATION);
            }
            delete info;
            RefreshRuleList();
        }
        // ---- 迁移 / 恢复 ----
        // 开始迁移：自动对所有已启用且可用的规则执行（无需先选中某条规则）
        void StartMigrateEnabledRules()
        {
            if (g_busy.exchange(true))
            {
                MBox(g_mainWnd, L"当前已有任务在运行", L"dirMover", MB_ICONINFORMATION);
                return;
            }
            ReadPlanIntoConfig();
            SaveConfig();

            std::vector<Rule> rules;
            for (const auto &r : g_cfg.rules)
            {
                if (!r.enabled || r.pendingDelete)
                    continue;
                if (!g_cfg.ValidateOneRule(r).empty())
                    continue;
                rules.push_back(r);
            }
            if (rules.empty())
            {
                MBox(g_mainWnd, L"没有可迁移的已启用规则（请先启用规则并设置监视/目标目录）。",
                            L"开始迁移", MB_ICONINFORMATION);
                g_busy = false;
                return;
            }

            // 服务在运行则交给后台服务统一迁移所有已启用规则
            std::wstring svcResp = PipeClient::Send(PipeServer::DefaultPipeName(),
                                                    PipeCommand::Migrate, L"");
            if (!svcResp.empty())
            {
                AppendLog(L"[服务] 已通过后台服务触发迁移（全部已启用规则）");
                MBox(g_mainWnd, L"已通知后台服务开始迁移全部已启用的规则。", L"dirMover",
                            MB_ICONINFORMATION);
                g_busy = false;
                return;
            }

            g_cancel = false;
            AppendLog(L"[迁移] 开始迁移全部已启用规则，共 " + std::to_wstring(rules.size()) + L" 条");
            HWND dlg = ProgressDialog::Show(g_mainWnd, L"迁移进度", true);
            g_progressDlg = dlg;
            if (HWND b = GetDlgItem(g_pageHwnd[PAGE_TASK], MainWindow::IDC_BTN_MIGRATE_RULE))
                EnableWindow(b, FALSE);
            g_worker = std::thread([rules, dlg]()
                                   {
                MigrateEngine engine(g_cfg);
                MigrateResult total;
                for (const auto &rule : rules)
                {
                    if (g_cancel.load())
                        break;
                    auto result = engine.MigrateRule(rule,
                        [dlg](const ProgressInfo &pi) { ProgressDialog::Update(dlg, pi); },
                        &g_cancel,
                        [](const LockNotice &n)
                        {
                            auto *copy = new LockNotice(n);
                            PostMessageW(g_mainWnd, MainWindow::WM_APP_LOCK, 0, (LPARAM)copy);
                        });
                    total.total += result.total;
                    total.migrated += result.migrated;
                    total.skipped += result.skipped;
                    total.locked += result.locked;
                    total.failed += result.failed;
                    total.bytesMoved += result.bytesMoved;
                    total.subtasks.insert(total.subtasks.end(), result.subtasks.begin(),
                                          result.subtasks.end());
                }
                PostMessageW(g_mainWnd, MainWindow::WM_APP_DONE, 0, (LPARAM)new MigrateResult(total)); });
            g_worker.detach();
        }

        // 恢复：对所有已启用规则把已迁移目录搬回原位置
        void StartRestoreEnabledRules()
        {
            if (g_busy.exchange(true))
            {
                MBox(g_mainWnd, L"当前已有任务在运行", L"dirMover", MB_ICONINFORMATION);
                return;
            }
            std::vector<Rule> rules;
            for (const auto &r : g_cfg.rules)
            {
                if (!r.enabled || r.pendingDelete)
                    continue;
                rules.push_back(r);
            }
            if (rules.empty())
            {
                MBox(g_mainWnd, L"没有已启用的规则。", L"恢复", MB_ICONINFORMATION);
                g_busy = false;
                return;
            }
            g_cancel = false;
            AppendLog(L"[恢复] 开始恢复全部已启用规则，共 " + std::to_wstring(rules.size()) + L" 条");
            HWND dlg = ProgressDialog::Show(g_mainWnd, L"恢复进度", true);
            g_progressDlg = dlg;
            g_worker = std::thread([rules, dlg]()
                                   {
                MigrateEngine engine(g_cfg);
                MigrateResult total;
                for (const auto &rule : rules)
                {
                    if (g_cancel.load())
                        break;
                    auto result = engine.RestoreRule(rule,
                        [dlg](const ProgressInfo &pi) { ProgressDialog::Update(dlg, pi); },
                        &g_cancel);
                    total.total += result.total;
                    total.migrated += result.migrated;
                    total.skipped += result.skipped;
                    total.locked += result.locked;
                    total.failed += result.failed;
                    total.bytesMoved += result.bytesMoved;
                }
                PostMessageW(g_mainWnd, MainWindow::WM_APP_DONE, 0, (LPARAM)new MigrateResult(total)); });
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
            if (HWND b = GetDlgItem(g_pageHwnd[PAGE_TASK], MainWindow::IDC_BTN_MIGRATE_RULE))
                EnableWindow(b, TRUE);

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
            MBox(g_mainWnd, buf, L"完成", MB_ICONINFORMATION);
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
                MBox(g_mainWnd, err.c_str(), L"更换目标目录失败", MB_ICONERROR);
            }
            else if (idx >= 0 && idx < (int)g_cfg.rules.size())
            {
                // 重新选择新目标（此处已在后台完成），更新配置
                AppendLog(L"[更换目标] 完成");
                MBox(g_mainWnd, L"已更换目标目录并重建链接。", L"完成", MB_ICONINFORMATION);
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
            MBox(g_mainWnd, detail.c_str(), L"文件被占用", MB_ICONWARNING);
            if (g_mainWnd)
                TrayIcon::Notify(g_mainWnd, L"dirMover 文件被占用", n.subtask + L" 迁移被跳过");
        }

        // ==================== 分页与控件布局 ====================
        void ShowPage(int page);
        void UpdateServiceState();

        // 布局常量（DIP，实际绘制统一用 theme::S 缩放）
        const int kSidebarW = 208;
        const int kPageCount = 5;
        constexpr int kNavIds[kPageCount] = {2001, 2002, 2003, 2004, 2005};
        const int kNavX = 16;
        const int kNavW = 176;
        const int kNavH = 38;
        const int kNavGap = 6;
        const int kNavTop = 118;

        void SetLabelColor(HWND h, COLORREF c)
        {
            SetPropW(h, L"dirmv_clr", (HANDLE)(INT_PTR)(c + 1));
        }

        // 全局 tooltip：每个操作按钮都有一句话说明

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
            ti.lpszText = (LPWSTR)i18n::T(text);
            SendMessageW(g_hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            g_tips.push_back(TipEntry{ctl, text});
        }

        void UpdateAllTips()
        {
            if (!g_hTip)
                return;
            for (const auto &e : g_tips)
            {
                TOOLINFOW ti{};
                ti.cbSize = sizeof(ti);
                ti.uFlags = TTF_IDISHWND;
                ti.hwnd = g_mainWnd;
                ti.uId = (UINT_PTR)e.ctl;
                ti.lpszText = (LPWSTR)i18n::T(e.text.c_str());
                SendMessageW(g_hTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
            }
        }

        HWND MakeLabel(HWND parent, const wchar_t *text, int x, int y, int w, int h, HFONT font,
                       COLORREF color)
        {
            HWND hwnd = CreateWindowExW(0, L"STATIC", i18n::T(text), WS_CHILD | WS_VISIBLE | SS_LEFT,
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
            const int listTop = theme::S(112);
            int listH = bottom - listTop;
            if (listH < theme::S(96))
                listH = theme::S(96);

            const int listW = panelW;
            SetWindowPos(g_hRuleList, nullptr, left, listTop, listW, listH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            // 列宽：目录两列自适应，其余固定
            int rest = listW - theme::S(62) - theme::S(62) - theme::S(150) - theme::S(88);
            if (rest < theme::S(240))
                rest = theme::S(240);
            ListView_SetColumnWidth(g_hRuleList, 0, rest * 52 / 100);
            ListView_SetColumnWidth(g_hRuleList, 1, rest - rest * 52 / 100);
            ListView_SetColumnWidth(g_hRuleList, 2, theme::S(62));
            ListView_SetColumnWidth(g_hRuleList, 3, theme::S(62));
            ListView_SetColumnWidth(g_hRuleList, 4, theme::S(150));
            ListView_SetColumnWidth(g_hRuleList, 5, theme::S(88));
        }

        // 任务页：迁移/恢复等操作、进度与日志集中在这里
        void LayoutTaskPage(HWND page)
        {
            if (!page)
                return;
            RECT rc{};
            GetClientRect(page, &rc);
            const int w = rc.right, h = rc.bottom;
            const int left = theme::S(40);
            const int right = w - theme::S(40);
            const int panelW = right - left;
            const int bottom = h - theme::S(32);

            const int opsY = theme::S(136);
            struct TaskBtn
            {
                int id;
                int w;
            } tasks[] = {{MainWindow::IDC_BTN_MIGRATE_RULE, 120},
                         {MainWindow::IDC_BTN_RESTORE_RULE, 80},
                         {MainWindow::IDC_BTN_ADOPT, 130},
                         {MainWindow::IDC_BTN_REBUILD, 200}};
            int x = left;
            for (const auto &a : tasks)
            {
                if (HWND b = GetDlgItem(page, a.id))
                    SetWindowPos(b, nullptr, x, opsY, theme::S(a.w), theme::S(30),
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                x += theme::S(a.w) + theme::S(8);
            }
            if (HWND stop = GetDlgItem(page, MainWindow::IDC_BTN_STOP))
                SetWindowPos(stop, nullptr, right - theme::S(72), opsY, theme::S(72), theme::S(30),
                             SWP_NOZORDER | SWP_NOACTIVATE);

            const int progressY = opsY + theme::S(54);
            const int etaW = theme::S(176);
            if (g_hProgress)
                SetWindowPos(g_hProgress, nullptr, left, progressY, panelW - etaW - theme::S(12),
                             theme::S(6), SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_hEta)
                SetWindowPos(g_hEta, nullptr, right - etaW, progressY - theme::S(7), etaW,
                             theme::S(20), SWP_NOZORDER | SWP_NOACTIVATE);

            const int logLabelY = progressY + theme::S(28);
            if (g_hLogLabel)
                SetWindowPos(g_hLogLabel, nullptr, left, logLabelY, theme::S(120), theme::S(18),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            const int logY = logLabelY + theme::S(24);
            const int logH = bottom - logY;
            if (g_hLog && logH > theme::S(60))
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
            theme::ApplyFont(g_hRuleList, theme::FontBody());
            if (HWND hdr = ListView_GetHeader(g_hRuleList))
                theme::ApplyFont(hdr, theme::FontSmall());

            const wchar_t *heads[6] = {L"监视目录", L"目标目录", L"白名单", L"已迁移", L"状态", L"详情"};
            const int kCols[6] = {248, 220, 62, 62, 150, 88};
            for (int i = 0; i < 6; ++i)
            {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                col.pszText = (LPWSTR)i18n::T(heads[i]);
                col.cx = theme::S(kCols[i]);
                col.fmt = (i == 2 || i == 3) ? LVCFMT_CENTER : LVCFMT_LEFT;
                ListView_InsertColumn(g_hRuleList, i, &col);
            }


            AddTipById(parent, MainWindow::IDC_BTN_ADD_RULE, L"新建一条迁移规则");
            AddTipById(parent, MainWindow::IDC_BTN_IMPORT, L"从其他目录的配置文件导入规则");

            LayoutRulePage(parent);
        }

        // 任务页：迁移/恢复/接管/生成规则、进度与日志
        void BuildTaskPage(HWND parent)
        {
            PageHeader(parent, L"任务", L"执行迁移、恢复与接管，并查看进度和日志");

            theme::CreateButton(parent, MainWindow::IDC_BTN_MIGRATE_RULE, L"开始迁移",
                                theme::BtnRole::BtnPrimary, theme::S(0), theme::S(0), theme::S(120),
                                theme::S(30));
            theme::CreateButton(parent, MainWindow::IDC_BTN_RESTORE_RULE, L"恢复",
                                theme::BtnRole::BtnDefault, theme::S(0), theme::S(0), theme::S(80),
                                theme::S(30));
            theme::CreateButton(parent, MainWindow::IDC_BTN_ADOPT, L"扫描接管…",
                                theme::BtnRole::BtnDefault, theme::S(0), theme::S(0), theme::S(130),
                                theme::S(30));
            theme::CreateButton(parent, MainWindow::IDC_BTN_REBUILD, L"从目标目录生成规则…",
                                theme::BtnRole::BtnDefault, theme::S(0), theme::S(0), theme::S(200),
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
                                         ES_READONLY | ES_AUTOVSCROLL,
                                     theme::S(0), theme::S(0), theme::S(300), theme::S(78), parent,
                                     (HMENU)(INT_PTR)MainWindow::IDC_LOG, g_hInst, nullptr);
            theme::ApplyFont(g_hLog, theme::FontMono());


            AddTipById(parent, MainWindow::IDC_BTN_MIGRATE_RULE,
                       L"迁移全部已启用规则下尚未迁移的非白名节目录（无需选中规则）");
            AddTipById(parent, MainWindow::IDC_BTN_RESTORE_RULE,
                       L"把全部已启用规则已迁移的目录恢复回原位置");
            AddTipById(parent, MainWindow::IDC_BTN_ADOPT,
                       L"扫描全部已启用规则的目标目录，接管已有数据并补建链接");
            AddTipById(parent, MainWindow::IDC_BTN_REBUILD,
                       L"选择已有数据的目标目录与新监视目录，建立链接并把该目录纳入规则");
            AddTipById(parent, MainWindow::IDC_BTN_STOP, L"停止当前正在执行的迁移/恢复任务");

            LayoutTaskPage(parent);
        }

        // ==================== 大小页 ====================
        struct SizeEntry
        {
            std::wstring name;
            unsigned long long bytes = 0;
        };
        struct SizeResult
        {
            std::vector<SizeEntry> watch;
            std::vector<SizeEntry> target;
            unsigned long long watchTotal = 0;
            unsigned long long targetTotal = 0;
        };

        HWND g_hSizeRule = nullptr;
        HWND g_hSizeWatchList = nullptr;
        HWND g_hSizeTargetList = nullptr;
        HWND g_hSizeSummary = nullptr;
        HWND g_hSizeStatus = nullptr;
        HWND g_hSizeRuleLabel = nullptr;
        HWND g_hSizeWatchTitle = nullptr;
        HWND g_hSizeTargetTitle = nullptr;
        std::atomic<bool> g_sizeBusy{false};
        std::atomic<bool> g_sizeCancel{false};
        std::vector<SizeEntry> g_sizeWatchData;
        std::vector<SizeEntry> g_sizeTargetData;
        unsigned long long g_sizeWatchTotal = 0;
        unsigned long long g_sizeTargetTotal = 0;

        void UpdateSizeSummaryText()
        {
            if (!g_hSizeSummary)
                return;
            const std::wstring wn = std::to_wstring(g_sizeWatchData.size());
            const std::wstring tn = std::to_wstring(g_sizeTargetData.size());
            std::wstring s;
            if (i18n::IsEnglish())
                s = L"Watch total " + Path::FormatBytes(g_sizeWatchTotal) + L" (" + wn +
                    L" items) · Target total " + Path::FormatBytes(g_sizeTargetTotal) + L" (" + tn +
                    L" items)";
            else
                s = L"监视目录合计 " + Path::FormatBytes(g_sizeWatchTotal) + L"（" + wn +
                    L" 项） · 目标目录合计 " + Path::FormatBytes(g_sizeTargetTotal) + L"（" + tn +
                    L" 项）";
            SetWindowTextW(g_hSizeSummary, s.c_str());
        }

        void ScanChildDirs(const std::wstring &dir, std::vector<SizeEntry> &out,
                           unsigned long long &total, std::atomic<bool> &cancel)
        {
            auto names = Path::ListSubDirectories(dir);
            for (const auto &n : names)
            {
                if (cancel.load())
                    return;
                std::wstring child = Path::Combine(dir, n);
                if (Path::IsReparsePoint(child))
                    continue;
                DirStat st{};
                if (!FileEngine::Scan(child, st))
                    continue;
                out.push_back({n, st.totalBytes});
                total += st.totalBytes;
            }
            std::sort(out.begin(), out.end(),
                      [](const SizeEntry &a, const SizeEntry &b) { return a.bytes > b.bytes; });
        }

        void FillSizeList(HWND lv, const std::vector<SizeEntry> &items, unsigned long long total)
        {
            if (!lv)
                return;
            ListView_DeleteAllItems(lv);
            for (size_t i = 0; i < items.size(); ++i)
            {
                LVITEMW it{};
                it.mask = LVIF_TEXT;
                it.iItem = (int)i;
                it.pszText = const_cast<LPWSTR>(items[i].name.c_str());
                int row = ListView_InsertItem(lv, &it);
                std::wstring sz = Path::FormatBytes(items[i].bytes);
                ListView_SetItemText(lv, row, 1, const_cast<LPWSTR>(sz.c_str()));
                wchar_t pct[32];
                swprintf_s(pct, L"%.1f%%", total ? (double)items[i].bytes * 100.0 / (double)total : 0.0);
                ListView_SetItemText(lv, row, 2, pct);
            }
        }

        void OnSizeDone(SizeResult *r)
        {
            g_sizeBusy = false;
            g_sizeWatchData = std::move(r->watch);
            g_sizeTargetData = std::move(r->target);
            FillSizeList(g_hSizeWatchList, g_sizeWatchData, r->watchTotal);
            FillSizeList(g_hSizeTargetList, g_sizeTargetData, r->targetTotal);
            g_sizeWatchTotal = r->watchTotal;
            g_sizeTargetTotal = r->targetTotal;
            UpdateSizeSummaryText();
            SetWindowTextW(g_hSizeStatus, L"");
            delete r;
        }

        void StartSizeScan()
        {
            if (g_sizeBusy.exchange(true))
                return;
            int idx = (int)SendMessageW(g_hSizeRule, CB_GETCURSEL, 0, 0);
            if (idx < 0 || idx >= (int)g_cfg.rules.size())
            {
                SetWindowTextW(g_hSizeStatus, L"没有可用的规则。");
                SetWindowTextW(g_hSizeSummary, L"");
                ListView_DeleteAllItems(g_hSizeWatchList);
                ListView_DeleteAllItems(g_hSizeTargetList);
                g_sizeBusy = false;
                return;
            }
            std::wstring watch = g_cfg.rules[idx].watchDir;
            std::wstring target = g_cfg.rules[idx].targetDir;
            SetWindowTextW(g_hSizeStatus, L"正在统计两个目录的大小…");
            g_sizeCancel = false;
            HWND main = g_mainWnd;
            std::thread th([watch, target, main]() {
                auto *r = new SizeResult();
                if (Path::DirectoryExists(watch))
                    ScanChildDirs(watch, r->watch, r->watchTotal, g_sizeCancel);
                if (Path::DirectoryExists(target))
                    ScanChildDirs(target, r->target, r->targetTotal, g_sizeCancel);
                PostMessageW(main, MainWindow::WM_APP_SIZE, 0, (LPARAM)r);
            });
            th.detach();
        }

        void RefreshSizeRuleCombo()
        {
            if (!g_hSizeRule)
                return;
            SendMessageW(g_hSizeRule, CB_RESETCONTENT, 0, 0);
            for (const auto &r : g_cfg.rules)
            {
                std::wstring s = r.watchDir + L"  →  " + r.targetDir;
                SendMessageW(g_hSizeRule, CB_ADDSTRING, 0, (LPARAM)s.c_str());
            }
            if (!g_cfg.rules.empty())
                SendMessageW(g_hSizeRule, CB_SETCURSEL, 0, 0);
            StartSizeScan();
        }

        void LayoutSizePage(HWND page)
        {
            RECT r;
            GetClientRect(page, &r);
            const int left = theme::S(40);
            const int right = r.right - theme::S(40);
            const int top = theme::S(104);
            if (g_hSizeRuleLabel)
                SetWindowPos(g_hSizeRuleLabel, nullptr, left, top + theme::S(6), theme::S(44),
                             theme::S(20), SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_hSizeRule)
                SetWindowPos(g_hSizeRule, nullptr, left + theme::S(48), top, theme::S(380),
                             theme::S(30), SWP_NOZORDER | SWP_NOACTIVATE);
            if (HWND b = GetDlgItem(page, MainWindow::IDC_SIZE_REFRESH))
                SetWindowPos(b, nullptr, right - theme::S(96), top, theme::S(96), theme::S(30),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_hSizeSummary)
                SetWindowPos(g_hSizeSummary, nullptr, left, top + theme::S(44), right - left,
                             theme::S(20), SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_hSizeStatus)
                SetWindowPos(g_hSizeStatus, nullptr, right - theme::S(300), top + theme::S(74),
                             theme::S(300), theme::S(20), SWP_NOZORDER | SWP_NOACTIVATE);

            const int listTop = top + theme::S(100);
            const int listH = r.bottom - listTop - theme::S(20);
            const int gap = theme::S(24);
            const int w = (right - left - gap) / 2;
            const int pctW = theme::S(58);
            const int sizeW = theme::S(88);
            const int nameW = w - pctW - sizeW - theme::S(26);
            auto fitCols = [&](HWND lv) {
                if (!lv)
                    return;
                ListView_SetColumnWidth(lv, 0, nameW);
                ListView_SetColumnWidth(lv, 1, sizeW);
                ListView_SetColumnWidth(lv, 2, pctW);
            };
            fitCols(g_hSizeWatchList);
            fitCols(g_hSizeTargetList);
            if (g_hSizeWatchTitle)
                SetWindowPos(g_hSizeWatchTitle, nullptr, left, listTop - theme::S(24), w,
                             theme::S(20), SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_hSizeTargetTitle)
                SetWindowPos(g_hSizeTargetTitle, nullptr, left + w + gap, listTop - theme::S(24), w,
                             theme::S(20), SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_hSizeWatchList)
                SetWindowPos(g_hSizeWatchList, nullptr, left, listTop, w, listH,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_hSizeTargetList)
                SetWindowPos(g_hSizeTargetList, nullptr, left + w + gap, listTop, w, listH,
                             SWP_NOZORDER | SWP_NOACTIVATE);
        }

        void BuildSizePage(HWND parent)
        {
            PageHeader(parent, L"大小", L"扫描监视目录与目标目录的直接子目录，按占用大小排序");

            g_hSizeRuleLabel = MakeLabel(parent, L"规则", 0, 0, 44, 20, theme::FontBody(), theme::Text);
            g_hSizeRule = MakeCombo(parent, MainWindow::IDC_SIZE_RULE, 0, 0, 380, 4096);
            theme::CreateButton(parent, MainWindow::IDC_SIZE_REFRESH, L"刷新", theme::BtnRole::BtnDefault,
                                0, 0, 96, 30);
            g_hSizeSummary = MakeLabel(parent, L"", 0, 0, 600, 20, theme::FontBody(), theme::Text);
            g_hSizeStatus = MakeLabel(parent, L"", 0, 0, 320, 20, theme::FontSmall(), theme::TextMuted);
            g_hSizeWatchTitle = MakeLabel(parent, L"监视目录", 0, 0, 260, 20, theme::FontBodyBold(),
                                          theme::Text);
            g_hSizeTargetTitle = MakeLabel(parent, L"目标目录", 0, 0, 260, 20, theme::FontBodyBold(),
                                           theme::Text);

            const wchar_t *heads[3] = {L"直接子目录", L"大小", L"占比"};
            const int kCols[3] = {220, 90, 70};
            auto makeLv = [&](int id) {
                HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                          WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                                              LVS_SHOWSELALWAYS,
                                          0, 0, 300, 200, parent, (HMENU)(INT_PTR)id, g_hInst,
                                          nullptr);
                ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
                theme::ApplyFont(lv, theme::FontBody());
                if (HWND hdr = ListView_GetHeader(lv))
                    theme::ApplyFont(hdr, theme::FontSmall());
                for (int i = 0; i < 3; ++i)
                {
                    LVCOLUMNW col{};
                    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                    col.pszText = (LPWSTR)i18n::T(heads[i]);
                    col.cx = theme::S(kCols[i]);
                    col.fmt = (i == 1 || i == 2) ? LVCFMT_RIGHT : LVCFMT_LEFT;
                    ListView_InsertColumn(lv, i, &col);
                }
                return lv;
            };
            g_hSizeWatchList = makeLv(MainWindow::IDC_SIZE_LIST_WATCH);
            g_hSizeTargetList = makeLv(MainWindow::IDC_SIZE_LIST_TARGET);

            AddTipById(parent, MainWindow::IDC_SIZE_REFRESH, L"重新统计监视目录与目标目录的直接子目录大小");
            LayoutSizePage(parent);
        }
        void BuildPlanPage(HWND parent)
        {
            PageHeader(parent, L"计划", L"按计划定时扫描已启用的规则并自动迁移新目录");

            MakeLabel(parent, L"定时", 40, 112, 200, 22, theme::FontBodyBold(), theme::Text);
            MakeLabel(parent, L"定时间隔", 40, 148, 64, 20, theme::FontBody(), theme::Text);
            g_hSchedPeriod = MakeCombo(parent, MainWindow::IDC_SCHED_PERIOD, 112, 142, 112, 200);
            SendMessageW(g_hSchedPeriod, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"不定时"));
            SendMessageW(g_hSchedPeriod, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"每天"));
            SendMessageW(g_hSchedPeriod, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"每周"));
            SendMessageW(g_hSchedPeriod, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"每月"));
            SendMessageW(g_hSchedPeriod, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"每N天"));
            SendMessageW(g_hSchedPeriod, CB_SETCURSEL, 0, 0);

            g_hSchedTimeLabel = MakeLabel(parent, L"时刻", 250, 148, 40, 20, theme::FontBody(),
                                          theme::Text);
            g_hSchedHour = MakeEdit(parent, MainWindow::IDC_SCHED_HOUR, L"2", 292, 142, 48, 30);
            g_hSchedColon = MakeLabel(parent, L":", 346, 148, 10, 20, theme::FontBody(), theme::Text);
            g_hSchedMin = MakeEdit(parent, MainWindow::IDC_SCHED_MIN, L"0", 358, 142, 48, 30);

            g_hSchedWeekLabel = MakeLabel(parent, L"周几", 420, 148, 40, 20, theme::FontBody(),
                                          theme::Text);
            g_hSchedWeekCombo = MakeCombo(parent, MainWindow::IDC_SCHED_WEEK, 462, 142, 88, 200);
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"周日"));
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"周一"));
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"周二"));
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"周三"));
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"周四"));
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"周五"));
            SendMessageW(g_hSchedWeekCombo, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"周六"));
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

            MakeLabel(parent, L"说明：定时 = 固定周期 + 固定时刻；「不定时」则只依赖实时监视与手动迁移。",
                      40, 232, 760, 20, theme::FontSmall(), theme::TextSub);

        }

        // 设置页：运行参数 + 后台服务 + 关于
        void BuildSettingsPage(HWND parent)
        {
            PageHeader(parent, L"设置", L"运行参数、后台服务与版本信息");

            // ── 运行参数 ──
            MakeLabel(parent, L"运行参数", 40, 96, 200, 22, theme::FontBodyBold(), theme::Text);
            MakeLabel(parent, L"线程数", 40, 134, 52, 20, theme::FontBody(), theme::Text);
            g_hThreads = MakeEdit(parent, MainWindow::IDC_THREADS, L"4", 96, 128, 64, 30);
            MakeLabel(parent, L"链接类型", 196, 134, 64, 20, theme::FontBody(), theme::Text);
            g_hLinkType = MakeCombo(parent, MainWindow::IDC_LINKTYPE, 264, 128, 190, 120);
            SendMessageW(g_hLinkType, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"符号链接(需管理员)"));
            SendMessageW(g_hLinkType, CB_ADDSTRING, 0, (LPARAM)i18n::T(L"目录联接(Junction)"));
            SendMessageW(g_hLinkType, CB_SETCURSEL, 0, 0);

            // ── 界面语言 ──
            MakeLabel(parent, L"界面语言", 470, 134, 84, 20, theme::FontBody(), theme::Text);
            g_hLangCombo = MakeCombo(parent, MainWindow::IDC_LANG, 558, 128, 130, 120);
            SendMessageW(g_hLangCombo, CB_ADDSTRING, 0, (LPARAM)L"中文");
            SendMessageW(g_hLangCombo, CB_ADDSTRING, 0, (LPARAM)L"English");
            SendMessageW(g_hLangCombo, CB_SETCURSEL, i18n::IsEnglish() ? 1 : 0, 0);

            g_hMoveMode = MakeCheck(parent, MainWindow::IDC_MOVEMODE, L"移动模式（迁移后删源）", 40,
                                    174, 280, 24);
            SendMessageW(g_hMoveMode, BM_SETCHECK, BST_CHECKED, 0);
            g_hWatcher = MakeCheck(parent, MainWindow::IDC_WATCHER, L"启用实时监视", 330, 174, 230,
                                   24);
            SendMessageW(g_hWatcher, BM_SETCHECK, BST_CHECKED, 0);

            g_hLockDetect = MakeCheck(parent, MainWindow::IDC_LOCKDETECT, L"检测文件占用", 40, 212,
                                      160, 24);
            SendMessageW(g_hLockDetect, BM_SETCHECK, BST_CHECKED, 0);
            MakeLabel(parent, L"重试间隔（分钟）", 210, 218, 160, 20, theme::FontBody(),
                      theme::Text);
            g_hRetryMin = MakeEdit(parent, MainWindow::IDC_RETRYMIN, L"5", 380, 212, 64, 30);

            theme::CreateButton(parent, MainWindow::IDC_BTN_SAVE_PLAN, L"保存设置",
                                theme::BtnRole::BtnPrimary, theme::S(40), theme::S(262), theme::S(104),
                                theme::S(34));

            // ── 后台服务 ──
            MakeLabel(parent, L"后台服务", 40, 318, 200, 22, theme::FontBodyBold(), theme::Text);
            g_hSvcState = CreateWindowExW(0, L"STATIC", L"未安装", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                          theme::S(40), theme::S(348), theme::S(360), theme::S(36),
                                          parent, (HMENU)(INT_PTR)MainWindow::IDC_SVCSTATE, g_hInst,
                                          nullptr);
            theme::ApplyFont(g_hSvcState, theme::FontH2());
            SetLabelColor(g_hSvcState, g_svcColor);

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
                                    theme::S(396), theme::S(96), theme::S(34));
                x += 104;
            }

            MakeLabel(parent, L"服务日志", 40, 446, 96, 20, theme::FontSmall(), theme::TextMuted);
            g_hSvcLogPath = MakeLabel(parent, L"", 144, 446, 660, 20, theme::FontSmall(), theme::Text);
            SetWindowTextW(g_hSvcLogPath,
                           Path::Combine(Path::ExeDir(), L"dirmv_service.log").c_str());
            theme::CreateButton(parent, MainWindow::IDC_BTN_OPENLOG, L"打开日志",
                                theme::BtnRole::BtnDefault, theme::S(40), theme::S(470), theme::S(96),
                                theme::S(30));

            // ── 关于 ──
            MakeLabel(parent, L"关于", 40, 524, 200, 22, theme::FontBodyBold(), theme::Text);
            std::wstring ver = L"dirMover 版本 " + GetAppVersion();
            MakeLabel(parent, ver.c_str(), 40, 556, 420, 20, theme::FontSmall(), theme::Text);
            MakeLabel(parent, L"联系方式：yhdhz@0916@outlook.com（衍荒）", 40, 580, 520, 20,
                      theme::FontSmall(), theme::TextSub);
            std::wstring cfg = L"配置文件：" + g_iniPath;
            MakeLabel(parent, cfg.c_str(), 40, 612, 780, 20, theme::FontSmall(), theme::TextSub);

            AddTipById(parent, MainWindow::IDC_THREADS, L"同时迁移的目录数；0 表示自动（最多 8）");
            AddTipById(parent, MainWindow::IDC_WATCHER, L"目录一出现就自动迁移");
            AddTipById(parent, MainWindow::IDC_LOCKDETECT,
                       L"迁移前检测文件是否被占用，占用则本次跳过并创建一次性重试任务");
            AddTipById(parent, MainWindow::IDC_RETRYMIN,
                       L"占用/失败后创建一次性重试任务的间隔；0 表示使用默认 5 分钟");
            AddTipById(parent, MainWindow::IDC_BTN_SVCINSTALL, L"安装后台服务并设为开机自动启动（需管理员）");
            AddTipById(parent, MainWindow::IDC_BTN_SVCSTART, L"启动后台服务");
            AddTipById(parent, MainWindow::IDC_BTN_SVCSTOP, L"停止后台服务（等待任务收尾）");
            AddTipById(parent, MainWindow::IDC_BTN_SVCREMOVE, L"卸载后台服务（不影响已迁移的数据）");
            AddTipById(parent, MainWindow::IDC_BTN_OPENLOG, L"用默认程序打开服务日志文件");
        }


        void RelayoutPage(HWND page)
        {
            if (!page)
                return;
            if (page == g_pageHwnd[PAGE_RULES])
                LayoutRulePage(page);
            else if (page == g_pageHwnd[PAGE_TASK])
                LayoutTaskPage(page);
            else if (page == g_pageHwnd[PAGE_SIZE])
                LayoutSizePage(page);
        }
    } // namespace

    // ==================== 界面语言切换 ====================
    namespace
    {
        // 把父窗口下所有子控件的标题按当前语言重译（无法匹配的文本原样保留）
        void TranslateChildWindows(HWND parent)
        {
            EnumChildWindows(
                parent,
                [](HWND c, LPARAM) -> BOOL
                {
                    wchar_t buf[2048];
                    int n = GetWindowTextW(c, buf, 2048);
                    if (n > 0)
                    {
                        std::wstring t(buf);
                        std::wstring tr = i18n::TranslateText(t);
                        if (tr == t)
                        {
                            // 拼接标签：把已知中文前缀替换成当前语言
                            static const wchar_t *prefixes[] = {L"dirMover 版本 ", L"配置文件：", L"服务名 "};
                            for (const wchar_t *pre : prefixes)
                            {
                                const size_t plen = wcslen(pre);
                                if (t.size() >= plen && t.compare(0, plen, pre) == 0)
                                {
                                    tr = i18n::T(pre) + t.substr(plen);
                                    break;
                                }
                            }
                        }
                        if (tr != t)
                        {
                            SetWindowTextW(c, tr.c_str());
                            InvalidateRect(c, nullptr, TRUE);
                        }
                    }
                    return TRUE;
                },
                0);
        }

        void RebuildComboItems(HWND cb, const wchar_t *const *items, int count)
        {
            if (!cb)
                return;
            int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
            if (sel < 0)
                sel = 0;
            SendMessageW(cb, CB_RESETCONTENT, 0, 0);
            for (int i = 0; i < count; ++i)
                SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)i18n::T(items[i]));
            SendMessageW(cb, CB_SETCURSEL, (WPARAM)sel, 0);
        }

        void SetRuleListHeaders()
        {
            static const wchar_t *heads[6] = {L"监视目录", L"目标目录", L"白名单", L"已迁移", L"状态", L"详情"};
            for (int i = 0; i < 6; ++i)
            {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT;
                col.pszText = (LPWSTR)i18n::T(heads[i]);
                ListView_SetColumn(g_hRuleList, i, &col);
            }
        }

        void SetSizeListHeaders(HWND lv)
        {
            static const wchar_t *heads[3] = {L"直接子目录", L"大小", L"占比"};
            for (int i = 0; i < 3; ++i)
            {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT;
                col.pszText = (LPWSTR)i18n::T(heads[i]);
                ListView_SetColumn(lv, i, &col);
            }
        }

        void ApplyLanguageToUi()
        {
            // 1) 所有子控件标题
            TranslateChildWindows(g_mainWnd);

            // 2) 列表列头
            SetRuleListHeaders();
            SetSizeListHeaders(g_hSizeWatchList);
            SetSizeListHeaders(g_hSizeTargetList);

            // 3) 下拉框条目（值不变，只重译显示文本）
            static const wchar_t *periods[5] = {L"不定时", L"每天", L"每周", L"每月", L"每N天"};
            static const wchar_t *weeks[7] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
            static const wchar_t *links[2] = {L"符号链接(需管理员)", L"目录联接(Junction)"};
            RebuildComboItems(g_hSchedPeriod, periods, 5);
            RebuildComboItems(g_hSchedWeekCombo, weeks, 7);
            RebuildComboItems(g_hLinkType, links, 2);
            if (g_hLangCombo)
                SendMessageW(g_hLangCombo, CB_SETCURSEL, i18n::IsEnglish() ? 1 : 0, 0);

            // 4) 动态文本
            SetWindowTextW(g_mainWnd, i18n::T(L"dirMover - 目录软链接迁移/恢复工具"));
            UpdateAllTips();
            RefreshRuleList();
            UpdateRuleStatuses();
            UpdateServiceState();
            UpdateSizeSummaryText();
            InvalidateRect(g_mainWnd, nullptr, TRUE);
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
                else if (hwnd == g_pageHwnd[PAGE_TASK])
                    LayoutTaskPage(hwnd);
                else if (hwnd == g_pageHwnd[PAGE_SIZE])
                    LayoutSizePage(hwnd);
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
                if (hwnd == g_pageHwnd[PAGE_SETTINGS])
                {
                    const int px = theme::S(24);
                    const int pw = rc.right - theme::S(24);
                    const RECT g1{px, theme::S(84), pw, theme::S(306)};
                    const RECT g2{px, theme::S(310), pw, theme::S(510)};
                    const RECT g3{px, theme::S(514), pw, rc.bottom - theme::S(24)};
                    theme::RoundRectEx(dc, g1, theme::S(10), theme::Card, theme::Border, 1);
                    theme::RoundRectEx(dc, g2, theme::S(10), theme::Card, theme::Border, 1);
                    theme::RoundRectEx(dc, g3, theme::S(10), theme::Card, theme::Border, 1);
                }
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
                // 复选框由 comctl32 v6 通过 WM_CTLCOLORSTATIC 取背景色，需要白刷；
                // 普通静态文字返回空刷子，让面板/卡片底色透出
                wchar_t cls[16]{};
                GetClassNameW(ctl, cls, 16);
                if (lstrcmpiW(cls, L"Button") == 0)
                    return (LRESULT)theme::CardBrush();
                return (LRESULT)GetStockObject(HOLLOW_BRUSH);
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
                ShowPage(PAGE_TASK);
                break;
            case kNavIds[2]:
                ShowPage(PAGE_SIZE);
                RefreshSizeRuleCombo();
                break;
            case kNavIds[3]:
                ShowPage(PAGE_PLAN);
                break;
            case kNavIds[4]:
                ShowPage(PAGE_SETTINGS);
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
                GenerateRuleFlow();
                break;
            case MainWindow::IDC_BTN_MIGRATE_RULE:
                StartMigrateEnabledRules();
                break;
            case MainWindow::IDC_BTN_RESTORE_RULE:
                StartRestoreEnabledRules();
                break;
            case MainWindow::IDC_BTN_STOP:
                g_cancel = true;
                AppendLog(L"[用户] 请求停止任务");
                break;

            // 计划页
            case MainWindow::IDC_LANG:
            {
                HWND cb = GetDlgItem(g_pageHwnd[PAGE_SETTINGS], MainWindow::IDC_LANG);
                int sel = cb ? (int)SendMessageW(cb, CB_GETCURSEL, 0, 0) : 0;
                if (sel < 0)
                    sel = 0;
                g_cfg.uiLanguage = (sel == 1) ? L"en" : L"zh";
                i18n::SetLanguage(g_cfg.uiLanguage);
                SaveConfig();
                ApplyLanguageToUi();
                AppendLog(sel == 1 ? L"[设置] 界面语言已切换为 English"
                                   : L"[设置] 界面语言已切换为中文");
                break;
            }

            case MainWindow::IDC_BTN_SAVE_PLAN:
                ReadPlanIntoConfig();
                SaveConfig();
                UpdateNextRunLabel();
                AppendLog(L"[计划] 设置已保存");
                MBox(g_mainWnd, L"计划设置已保存。", L"dirMover", MB_ICONINFORMATION);
                break;

            // 设置页（服务）
            case MainWindow::IDC_BTN_SVCINSTALL:
            {
                std::wstring err;
                if (ServiceInstaller::Install(L"dirMoverSvc", L"dirMover 目录迁移服务", &err))
                    MBox(g_mainWnd, L"服务安装成功（已设为开机自动启动）", L"dirMover", MB_ICONINFORMATION);
                else
                    MBox(g_mainWnd, err.c_str(), L"安装失败", MB_ICONERROR);
                UpdateServiceState();
                break;
            }
            case MainWindow::IDC_BTN_SVCSTART:
            {
                std::wstring err;
                if (!ServiceInstaller::Start(L"dirMoverSvc", &err))
                    MBox(g_mainWnd, err.c_str(), L"启动失败", MB_ICONERROR);
                UpdateServiceState();
                break;
            }
            case MainWindow::IDC_BTN_SVCSTOP:
            {
                std::wstring err;
                if (!ServiceInstaller::Stop(L"dirMoverSvc", &err))
                    MBox(g_mainWnd, err.c_str(), L"停止失败", MB_ICONERROR);
                UpdateServiceState();
                break;
            }
            case MainWindow::IDC_BTN_SVCREMOVE:
            {
                std::wstring err;
                if (ServiceInstaller::Uninstall(L"dirMoverSvc", &err))
                    MBox(g_mainWnd, L"服务已卸载", L"dirMover", MB_ICONINFORMATION);
                else
                    MBox(g_mainWnd, err.c_str(), L"卸载失败", MB_ICONERROR);
                UpdateServiceState();
                break;
            }

            case MainWindow::IDC_BTN_OPENLOG:
            {
                std::wstring logPath = Path::Combine(Path::ExeDir(), L"dirmv_service.log");
                HINSTANCE r = ShellExecuteW(nullptr, L"open", logPath.c_str(), nullptr, nullptr,
                                            SW_SHOWNORMAL);
                if ((INT_PTR)r <= 32)
                    MBox(g_mainWnd, (L"无法打开日志文件：\n" + logPath).c_str(), L"dirMover",
                                MB_ICONWARNING);
                break;
            }

            case MainWindow::IDC_SIZE_REFRESH:
                StartSizeScan();
                break;
            case MainWindow::IDC_SIZE_RULE:
                StartSizeScan();
                break;

            case TrayIcon::IDM_SHOW:
                ShowWindow(g_mainWnd, SW_SHOW);
                SetForegroundWindow(g_mainWnd);
                break;
            case TrayIcon::IDM_MIGRATE:
                StartMigrateEnabledRules();
                break;
            case TrayIcon::IDM_RESTORE:
                StartRestoreEnabledRules();
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
                RECT crc{};
                GetClientRect(hwnd, &crc);
                const int navW = theme::S(kSidebarW);
                for (int i = 0; i < kPageCount; ++i)
                    g_pageHwnd[i] =
                        CreatePage(hwnd, navW, 0, crc.right - navW, crc.bottom, i == 0);

                BuildRulePage(g_pageHwnd[PAGE_RULES]);
                BuildTaskPage(g_pageHwnd[PAGE_TASK]);
                BuildSizePage(g_pageHwnd[PAGE_SIZE]);
                BuildPlanPage(g_pageHwnd[PAGE_PLAN]);
                BuildSettingsPage(g_pageHwnd[PAGE_SETTINGS]);

                // 左侧导航项
                static const wchar_t *navLabels[kPageCount] = {L"规则", L"任务", L"大小", L"计划",
                                                               L"设置"};
                static const wchar_t *navTips[kPageCount] = {
                    L"管理迁移规则",
                    L"执行迁移/恢复并查看进度与日志",
                    L"对比监视目录与目标目录的直接子目录大小",
                    L"定时间隔与下次运行时间",
                    L"运行参数、后台服务与版本信息"};
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
                theme::TextIn(dc, sr, i18n::T(L"目录迁移 · 链接管理"), theme::FontSmall(), theme::TextMuted,
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
                        else if (nh->code == NM_CLICK && row >= 0 && col == 5)
                        {
                            // 点击「详情」列 → 打开规则详情
                            g_selectedRule = row;
                            EditRuleFlow();
                        }
                    }
                }
                if ((nh->idFrom == MainWindow::IDC_SIZE_LIST_WATCH ||
                     nh->idFrom == MainWindow::IDC_SIZE_LIST_TARGET) &&
                    nh->code == NM_CUSTOMDRAW)
                {
                    LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)lParam;
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
                    {
                        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYSUBITEMDRAW;
                    }
                    if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM) &&
                        cd->iSubItem == 1)
                    {
                        const std::vector<SizeEntry> &data =
                            (nh->idFrom == MainWindow::IDC_SIZE_LIST_WATCH) ? g_sizeWatchData
                                                                             : g_sizeTargetData;
                        size_t idx = (size_t)cd->nmcd.dwItemSpec;
                        unsigned long long maxB = 0;
                        for (const auto &e : data)
                        {
                            if (e.bytes > maxB)
                                maxB = e.bytes;
                        }
                        if (idx < data.size() && maxB > 0)
                        {
                            RECT r = cd->nmcd.rc;
                            int w = (int)((double)data[idx].bytes / (double)maxB *
                                          (r.right - r.left));
                            RECT bar{r.left, r.bottom - theme::S(6), r.left + w,
                                     r.bottom - theme::S(2)};
                            HBRUSH br = CreateSolidBrush(theme::AccentSoft);
                            FillRect(cd->nmcd.hdc, &bar, br);
                            DeleteObject(br);
                        }
                    }
                    return CDRF_DODEFAULT;
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
            case MainWindow::WM_APP_SIZE:
            {
                auto *r = reinterpret_cast<SizeResult *>(lParam);
                if (r)
                    OnSizeDone(r);
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
                    HWND rules = g_pageHwnd[PAGE_RULES];
                    if (rules && IsWindowVisible(rules))
                        UpdateRuleStatuses();
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
        i18n::InitFromConfig(g_cfg.uiLanguage);

        const DWORD style = WS_OVERLAPPEDWINDOW | WS_THICKFRAME | WS_MAXIMIZEBOX;
        RECT rc{0, 0, theme::S(1000), theme::S(740)};
        AdjustWindowRectEx(&rc, style, FALSE, 0);
        HWND hwnd = CreateWindowExW(
            0, L"dirMoverMainWnd", i18n::T(L"dirMover - 目录软链接迁移/恢复工具"), style, CW_USEDEFAULT,
            CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInst,
            nullptr);
        if (!hwnd)
            return nullptr;

        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);
        return hwnd;
    }

} // namespace dirmv::gui
