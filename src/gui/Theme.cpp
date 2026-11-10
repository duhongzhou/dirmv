#include "gui/Theme.h"
#include "gui/I18n.h"

#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include <cmath>
#include <cstring>
#include <string>

#pragma comment(lib, "gdiplus.lib")

namespace dirmv::gui::theme
{
    namespace
    {
        int g_dpi = 96;
        double g_scale = 1.0;

        HFONT g_body = nullptr;
        HFONT g_small = nullptr;
        HFONT g_bold = nullptr;
        HFONT g_bodyBold = nullptr;
        HFONT g_h2 = nullptr;
        HFONT g_title = nullptr;
        HFONT g_app = nullptr;
        HFONT g_mono = nullptr;

        HBRUSH g_cardBrush = nullptr;
        HBRUSH g_inputBrush = nullptr;

        const wchar_t *kUiFace = L"Microsoft YaHei UI";

        int Scaled(int v) { return (int)std::lround(v * g_scale); }

        HFONT MakeFont(int pt, int weight)
        {
            LOGFONTW lf{};
            lf.lfHeight = -MulDiv(pt, g_dpi, 72);
            lf.lfWeight = weight;
            lf.lfQuality = CLEARTYPE_QUALITY;
            lf.lfCharSet = DEFAULT_CHARSET;
            wcsncpy_s(lf.lfFaceName, kUiFace, _TRUNCATE);
            return CreateFontIndirectW(&lf);
        }

        HFONT MakeMono(int pt)
        {
            LOGFONTW lf{};
            lf.lfHeight = -MulDiv(pt, g_dpi, 72);
            lf.lfWeight = FW_NORMAL;
            lf.lfQuality = CLEARTYPE_QUALITY;
            wcsncpy_s(lf.lfFaceName, L"Consolas", _TRUNCATE);
            return CreateFontIndirectW(&lf);
        }

        void AddRoundPath(Gdiplus::GraphicsPath &path, const Gdiplus::RectF &r, float rad)
        {
            float d = rad * 2.0f;
            if (d > r.Width)
                d = r.Width;
            if (d > r.Height)
                d = r.Height;
            if (d <= 0.5f)
            {
                path.AddRectangle(r);
                return;
            }
            const float PI = 3.14159265f;
            (void)PI;
            path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
            path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
            path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
            path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
            path.CloseFigure();
        }

        void FillRoundRect(HDC dc, const RECT &rc, int radius, COLORREF fill)
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::RectF r((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                             (Gdiplus::REAL)(rc.right - rc.left), (Gdiplus::REAL)(rc.bottom - rc.top));
            Gdiplus::GraphicsPath path;
            AddRoundPath(path, r, (Gdiplus::REAL)radius);
            Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(fill), GetGValue(fill),
                                                     GetBValue(fill)));
            g.FillPath(&brush, &path);
        }

        void StrokeRoundRect(HDC dc, const RECT &rc, int radius, COLORREF color, int width)
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::RectF r((Gdiplus::REAL)rc.left + width / 2.0f, (Gdiplus::REAL)rc.top + width / 2.0f,
                             (Gdiplus::REAL)(rc.right - rc.left - width),
                             (Gdiplus::REAL)(rc.bottom - rc.top - width));
            Gdiplus::GraphicsPath path;
            AddRoundPath(path, r, (Gdiplus::REAL)radius);
            Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)),
                             (Gdiplus::REAL)width);
            g.DrawPath(&pen, &path);
        }

        // ---- 按钮窗口过程 ----
        struct BtnState
        {
            BtnRole role = BtnDefault;
            bool hover = false;
            bool pressed = false;
            bool tracking = false;
            bool active = false; // BtnTab 用
        };

        BtnState *StateOf(HWND h)
        {
            return reinterpret_cast<BtnState *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }

        void NotifyClick(HWND h)
        {
            int id = GetDlgCtrlID(h);
            SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)h);
        }

        void PaintButton(HWND h, HDC target)
        {
            BtnState *st = StateOf(h);
            if (!st)
                return;
            RECT rc;
            GetClientRect(h, &rc);
            int w = rc.right - rc.left;
            int ht = rc.bottom - rc.top;
            if (w <= 0 || ht <= 0)
                return;

            HDC mem = CreateCompatibleDC(target);
            HBITMAP bmp = CreateCompatibleBitmap(target, w, ht);
            HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);
            SetBkMode(mem, TRANSPARENT);

            bool enabled = IsWindowEnabled(h) != FALSE;
            bool focused = GetFocus() == h;
            int radius = Scaled(7);

            COLORREF fill = Card, border = Border, text = Text, textBg = Card;
            int borderWidth = 1;

            switch (st->role)
            {
            case BtnPrimary:
                fill = Accent;
                border = Accent;
                text = RGB(0xFF, 0xFF, 0xFF);
                textBg = Accent;
                if (!enabled)
                {
                    fill = RGB(0xED, 0xEF, 0xF3);
                    border = RGB(0xE3, 0xE6, 0xEB);
                    text = RGB(0xA9, 0xAF, 0xBA);
                    textBg = fill;
                }
                else if (st->pressed)
                    fill = AccentPressed;
                else if (st->hover)
                    fill = AccentHover;
                border = fill;
                textBg = fill;
                break;
            case BtnDanger:
                fill = Card;
                border = RGB(0xE8, 0xC6, 0xC8);
                text = Danger;
                textBg = fill;
                if (!enabled)
                {
                    fill = RGB(0xF3, 0xF4, 0xF6);
                    border = RGB(0xE3, 0xE6, 0xEB);
                    text = RGB(0xA9, 0xAF, 0xBA);
                }
                else if (st->pressed)
                {
                    fill = RGB(0xF8, 0xDF, 0xE0);
                    border = RGB(0xDD, 0xA9, 0xAC);
                }
                else if (st->hover)
                {
                    fill = DangerSoft;
                    border = RGB(0xDD, 0xAE, 0xB1);
                }
                textBg = fill;
                break;
            case BtnGhost:
                fill = SegBg;
                border = SegBg;
                text = TextSub;
                if (st->hover && enabled)
                    fill = SegHover;
                if (st->pressed && enabled)
                    fill = RGB(0xD8, 0xDC, 0xE3);
                border = fill;
                textBg = fill;
                break;
            case BtnTab:
                if (st->active)
                {
                    fill = Card;
                    border = RGB(0xDF, 0xE3, 0xEA);
                    text = Text;
                }
                else
                {
                    fill = SegBg;
                    border = SegBg;
                    text = TextSub;
                    if (st->hover && enabled)
                        fill = SegHover;
                    if (st->pressed && enabled)
                        fill = RGB(0xD9, 0xDD, 0xE4);
                    border = fill;
                }
                textBg = fill;
                break;
            case BtnNav:
                if (st->active)
                {
                    fill = AccentSoft;
                    border = AccentSoft;
                    text = Accent;
                }
                else
                {
                    fill = Card;
                    border = Card;
                    text = TextSub;
                    if (!enabled)
                        text = TextMuted;
                    else if (st->pressed)
                        fill = SegHover;
                    else if (st->hover)
                        fill = HoverFill;
                    border = fill;
                }
                textBg = fill;
                break;
            case BtnDefault:
            default:
                fill = Card;
                border = BorderStrong;
                text = Text;
                if (!enabled)
                {
                    fill = RGB(0xF3, 0xF4, 0xF6);
                    border = RGB(0xE5, 0xE7, 0xEC);
                    text = RGB(0xA9, 0xAF, 0xBA);
                }
                else if (st->pressed)
                {
                    fill = RGB(0xE9, 0xEC, 0xF1);
                    border = RGB(0xC3, 0xC9, 0xD3);
                }
                else if (st->hover)
                {
                    fill = HoverFill;
                    border = RGB(0xBF, 0xC6, 0xD1);
                }
                textBg = fill;
                break;
            }

            // 背景：按钮所在容器颜色（卡片白 / 头部底色 / 分段底色）
            COLORREF back = Card;
            if (st->role == BtnTab)
                back = st->active ? Card : SegBg;
            RECT full = rc;
            HBRUSH backBrush = CreateSolidBrush(back);
            FillRect(mem, &full, backBrush);
            DeleteObject(backBrush);

            RECT r = rc;
            if (st->role != BtnTab)
            {
                r.left += 0;
                r.top += st->pressed ? Scaled(1) : 0;
            }
            FillRoundRect(mem, r, radius, fill);
            if (border != fill)
                StrokeRoundRect(mem, r, radius, border, borderWidth);

            if (focused && enabled)
            {
                RECT fr = r;
                InflateRect(&fr, -Scaled(3), -Scaled(3));
                StrokeRoundRect(mem, fr, Scaled(5), RGB(0x8F, 0xB0, 0xF2), 1);
            }

            // 文字
            HFONT f = (st->role == BtnPrimary || st->role == BtnTab) ? FontBodyBold() : FontBody();
            if (st->role == BtnTab && !st->active)
                f = FontBody();
            if (st->role == BtnNav && st->active)
                f = FontBodyBold();
            SelectObject(mem, f);
            SetTextColor(mem, text);
            wchar_t buf[256]{};
            GetWindowTextW(h, buf, 256);
            RECT tr = r;
            if (st->role == BtnNav)
            {
                tr.left += Scaled(14);
                tr.right -= Scaled(10);
                DrawTextW(mem, buf, -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            else
            {
                tr.left += Scaled(6);
                tr.right -= Scaled(6);
                DrawTextW(mem, buf, -1, &tr,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            BitBlt(target, 0, 0, w, ht, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
        }

        LRESULT CALLBACK BtnProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
        {
            BtnState *st = StateOf(h);
            switch (msg)
            {
            case WM_CREATE:
            {
                auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
                st = new BtnState();
                st->role = (BtnRole)(INT_PTR)cs->lpCreateParams;
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
                return 0;
            }
            case WM_DESTROY:
                delete st;
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                PaintButton(h, dc);
                EndPaint(h, &ps);
                return 0;
            }
            case WM_MOUSEMOVE:
                if (st && !st->tracking)
                {
                    TRACKMOUSEEVENT tme{};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = h;
                    TrackMouseEvent(&tme);
                    st->tracking = true;
                }
                if (st && !st->hover)
                {
                    st->hover = true;
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            case WM_MOUSELEAVE:
                if (st)
                {
                    st->hover = false;
                    st->tracking = false;
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            case WM_LBUTTONDOWN:
                if (st)
                {
                    st->pressed = true;
                    SetCapture(h);
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            case WM_LBUTTONUP:
                if (st)
                {
                    bool inside = false;
                    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                    RECT rc;
                    GetClientRect(h, &rc);
                    inside = PtInRect(&rc, pt) != FALSE;
                    bool wasPressed = st->pressed;
                    st->pressed = false;
                    if (GetCapture() == h)
                        ReleaseCapture();
                    InvalidateRect(h, nullptr, FALSE);
                    if (wasPressed && inside && IsWindowEnabled(h))
                        NotifyClick(h);
                }
                return 0;
            case WM_CAPTURECHANGED:
                if (st && st->pressed)
                {
                    st->pressed = false;
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            case WM_KEYDOWN:
                if (wp == VK_SPACE && st && !st->pressed)
                {
                    st->pressed = true;
                    InvalidateRect(h, nullptr, FALSE);
                }
                else if (wp == VK_RETURN)
                {
                    NotifyClick(h);
                }
                return 0;
            case WM_KEYUP:
                if (wp == VK_SPACE && st && st->pressed)
                {
                    st->pressed = false;
                    InvalidateRect(h, nullptr, FALSE);
                    NotifyClick(h);
                }
                return 0;
            case WM_GETDLGCODE:
                return DLGC_BUTTON | DLGC_WANTCHARS;
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            case WM_ENABLE:
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            case WM_SETTEXT:
            {
                LRESULT r = DefWindowProcW(h, msg, wp, lp);
                InvalidateRect(h, nullptr, FALSE);
                return r;
            }
            }
            return DefWindowProcW(h, msg, wp, lp);
        }
    } // namespace

    void Init(HINSTANCE hInst)
    {
        (void)hInst;
        HDC screen = GetDC(nullptr);
        g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(nullptr, screen);
        if (g_dpi <= 0)
            g_dpi = 96;
        g_scale = g_dpi / 96.0;

        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartup(&token, &gdiplusStartupInput, nullptr);

        g_body = MakeFont(10, FW_NORMAL);
        g_small = MakeFont(9, FW_NORMAL);
        g_bold = MakeFont(10, FW_SEMIBOLD);
        g_bodyBold = MakeFont(10, FW_SEMIBOLD);
        g_h2 = MakeFont(12, FW_SEMIBOLD);
        g_title = MakeFont(15, FW_SEMIBOLD);
        g_app = MakeFont(22, FW_SEMIBOLD);
        g_mono = MakeMono(9);

        g_cardBrush = CreateSolidBrush(Card);
        g_inputBrush = CreateSolidBrush(InputBg);
    }

    int Dpi() { return g_dpi; }
    int S(int v) { return Scaled(v); }

    HFONT FontBody() { return g_body; }
    HFONT FontSmall() { return g_small; }
    HFONT FontBold() { return g_bold; }
    HFONT FontBodyBold() { return g_bodyBold; }
    HFONT FontH2() { return g_h2; }
    HFONT FontTitle() { return g_title; }
    HFONT FontApp() { return g_app; }
    HFONT FontMono() { return g_mono; }

    void ApplyFont(HWND h, HFONT f)
    {
        if (h && f)
            SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    }

    HBRUSH CardBrush()
    {
        if (!g_cardBrush)
            g_cardBrush = CreateSolidBrush(Card);
        return g_cardBrush;
    }

    HBRUSH InputBrush()
    {
        if (!g_inputBrush)
            g_inputBrush = CreateSolidBrush(InputBg);
        return g_inputBrush;
    }

    void FillRound(HDC dc, const RECT &rc, int radius, COLORREF fill)
    {
        FillRoundRect(dc, rc, radius, fill);
    }

    void RoundRectEx(HDC dc, const RECT &rc, int radius, COLORREF fill, COLORREF border,
                     int borderWidth)
    {
        FillRoundRect(dc, rc, radius, fill);
        if (border != fill)
            StrokeRoundRect(dc, rc, radius, border, borderWidth);
    }

    void Line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color)
    {
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HPEN old = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, x1, y1, nullptr);
        LineTo(dc, x2, y2);
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    void TextIn(HDC dc, const RECT &rc, const std::wstring &text, HFONT f, COLORREF color,
                UINT flags, int padding)
    {
        RECT r = rc;
        r.left += padding;
        r.right -= padding;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        HFONT old = (HFONT)SelectObject(dc, f);
        DrawTextW(dc, text.c_str(), (int)text.size(), &r, flags);
        SelectObject(dc, old);
    }

    void RegisterControls(HINSTANCE hInst)
    {
        static bool registered = false;
        if (registered)
            return;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = BtnProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = L"DirMvButton";
        wc.hbrBackground = nullptr;
        RegisterClassExW(&wc);
        registered = true;
    }

    HWND CreateButton(HWND parent, int id, const std::wstring &text, BtnRole role,
                      int x, int y, int w, int h)
    {
        HWND btn = CreateWindowExW(0, L"DirMvButton", i18n::T(text.c_str()),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                   x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr,
                                   (LPVOID)(INT_PTR)role);
        return btn;
    }

    void SetTabActive(HWND tab, bool active)
    {
        if (!tab)
            return;
        BtnState *st = StateOf(tab);
        if (st && st->active != active)
        {
            st->active = active;
            InvalidateRect(tab, nullptr, FALSE);
        }
    }

    void SetButtonRole(HWND button, BtnRole role)
    {
        if (!button)
            return;
        BtnState *st = StateOf(button);
        if (st)
        {
            st->role = role;
            InvalidateRect(button, nullptr, FALSE);
        }
    }

} // namespace dirmv::gui::theme
