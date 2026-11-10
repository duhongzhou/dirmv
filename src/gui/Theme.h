#pragma once
// ---------------------------------------------------------------------------
// Theme.h - 界面视觉系统
//   统一配色、字号、圆角、自绘按钮（Flat Button）。
//   所有坐标使用 S() 做 DPI 缩放（系统 DPI 感知）。
// ---------------------------------------------------------------------------
#include <windows.h>
#include <string>

namespace dirmv::gui::theme
{

    // ---- 调色板（浅色主题） ----
    constexpr COLORREF Bg = RGB(0xF4, 0xF5, 0xF7);           // 窗口底色
    constexpr COLORREF Card = RGB(0xFF, 0xFF, 0xFF);         // 卡片
    constexpr COLORREF Border = RGB(0xE5, 0xE7, 0xEC);       // 常规描边
    constexpr COLORREF BorderStrong = RGB(0xD2, 0xD7, 0xDF); // 输入框/次要按钮描边
    constexpr COLORREF Text = RGB(0x1D, 0x23, 0x2E);         // 主文字
    constexpr COLORREF TextSub = RGB(0x63, 0x6B, 0x7A);      // 次要文字
    constexpr COLORREF TextMuted = RGB(0x9A, 0xA1, 0xAD);    // 弱化文字
    constexpr COLORREF Accent = RGB(0x2E, 0x6B, 0xE6);       // 主色
    constexpr COLORREF AccentHover = RGB(0x24, 0x5E, 0xD3);
    constexpr COLORREF AccentPressed = RGB(0x1C, 0x4D, 0xB4);
    constexpr COLORREF AccentSoft = RGB(0xE9, 0xF0, 0xFE);
    constexpr COLORREF Danger = RGB(0xC8, 0x36, 0x3F);
    constexpr COLORREF DangerSoft = RGB(0xFB, 0xEC, 0xED);
    constexpr COLORREF Success = RGB(0x1B, 0x8A, 0x52);
    constexpr COLORREF Warning = RGB(0xA0, 0x5E, 0x00);
    constexpr COLORREF SegBg = RGB(0xEB, 0xED, 0xF1);        // 分段控件底
    constexpr COLORREF SegHover = RGB(0xE1, 0xE4, 0xEA);
    constexpr COLORREF InputBg = RGB(0xEE, 0xF1, 0xF4);      // 输入框/列表框底
    constexpr COLORREF HoverFill = RGB(0xF5, 0xF7, 0xFA);

    void Init(HINSTANCE hInst); // 初始化 GDI+、DPI、字体
    int Dpi();
    int S(int v); // DIP → 像素

    HFONT FontBody();
    HFONT FontSmall();
    HFONT FontBold();
    HFONT FontBodyBold();
    HFONT FontH2();
    HFONT FontTitle();
    HFONT FontApp();
    HFONT FontMono();

    void ApplyFont(HWND h, HFONT f);
    HBRUSH CardBrush();
    HBRUSH InputBrush();

    // ---- 绘制 ----
    void FillRound(HDC dc, const RECT &rc, int radius, COLORREF fill);
    void RoundRectEx(HDC dc, const RECT &rc, int radius, COLORREF fill, COLORREF border,
                     int borderWidth = 1);
    void Line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color);
    void TextIn(HDC dc, const RECT &rc, const std::wstring &text, HFONT f, COLORREF color,
                UINT flags, int padding = 0);

    // ---- 自绘按钮 ----
    enum BtnRole
    {
        BtnDefault = 0, // 白底描边
        BtnPrimary = 1, // 主色实心
        BtnDanger = 2,  // 危险操作用描边
        BtnGhost = 3,   // 无边框文字按钮
        BtnTab = 4,     // 分段标签
    };

    void RegisterControls(HINSTANCE hInst);
    HWND CreateButton(HWND parent, int id, const std::wstring &text, BtnRole role,
                      int x, int y, int w, int h);
    void SetTabActive(HWND tab, bool active);
    void SetButtonRole(HWND button, BtnRole role);

} // namespace dirmv::gui::theme
