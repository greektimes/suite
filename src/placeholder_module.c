/*
 * placeholder_module.c - branded splash for not-yet-built tabs.
 */

#include "placeholder_module.h"
#include <stdio.h>

#define PH_CLASS "MGTUnicornPlaceholder"
#define PH_LOGO_PX 96

typedef struct PhState {
    wchar_t  tab_name[64];
    HFONT    font_title;   /* 20 pt Segoe UI Semibold */
    HFONT    font_sub;     /* 14 pt Segoe UI Regular */
} PhState;

static PhState *ph_get(HWND h) { return (PhState *)GetWindowLongPtrA(h, GWLP_USERDATA); }

static int ph_dpi(HWND h)
{
    typedef UINT (WINAPI *F)(HWND);
    HMODULE u = GetModuleHandleA("user32.dll");
    F f = u ? (F)GetProcAddress(u, "GetDpiForWindow") : NULL;
    if (f) return (int)f(h);
    {
        HDC dc = GetDC(h);
        int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(h, dc);
        return d ? d : 96;
    }
}

static HFONT ph_make_font(int point, int weight, int dpi)
{
    LOGFONTA lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(point, dpi, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyA(lf.lfFaceName, "Segoe UI");
    return CreateFontIndirectA(&lf);
}

static void ph_paint(HWND hwnd, HDC hdc_target)
{
    PhState *st = ph_get(hwnd);
    RECT rc;
    HDC mem_dc;
    HBITMAP mem_bm, old_bm;
    int w, h, logo_y, logo_x, text_y;
    HICON icon;
    HBRUSH wb;
    HFONT old_f;
    RECT tr;

    if (!st) return;
    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    mem_dc = CreateCompatibleDC(hdc_target);
    mem_bm = CreateCompatibleBitmap(hdc_target, w, h);
    old_bm = (HBITMAP)SelectObject(mem_dc, mem_bm);

    wb = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(mem_dc, &rc, wb);

    icon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));
    if (!icon) icon = LoadIconA(NULL, IDI_APPLICATION);

    logo_y = h * 30 / 100;            /* 30% down from top */
    logo_x = (w - PH_LOGO_PX) / 2;
    if (icon)
        DrawIconEx(mem_dc, logo_x, logo_y, icon, PH_LOGO_PX, PH_LOGO_PX, 0,
                   NULL, DI_NORMAL);

    SetBkMode(mem_dc, TRANSPARENT);

    /* Title: tab name in 20 pt Semibold, centered, 24 px below logo. */
    SetTextColor(mem_dc, RGB(17, 17, 17));
    old_f = (HFONT)SelectObject(mem_dc, st->font_title);
    text_y = logo_y + PH_LOGO_PX + 24;
    tr.left = 0; tr.right = w; tr.top = text_y; tr.bottom = text_y + 60;
    DrawTextW(mem_dc, st->tab_name, -1, &tr,
              DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    {
        /* Measure exact title height to position subtitle. */
        SIZE sz;
        GetTextExtentPoint32W(mem_dc, st->tab_name, lstrlenW(st->tab_name), &sz);
        text_y += sz.cy + 8;
    }

    /* Subtitle: "Coming Soon" in 14 pt regular gray. */
    SelectObject(mem_dc, st->font_sub);
    SetTextColor(mem_dc, RGB(102, 102, 102));
    tr.top = text_y; tr.bottom = text_y + 40;
    DrawTextA(mem_dc, "Coming Soon", -1, &tr,
              DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(mem_dc, old_f);

    BitBlt(hdc_target, 0, 0, w, h, mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bm);
    DeleteObject(mem_bm);
    DeleteDC(mem_dc);
}

static LRESULT CALLBACK PlaceholderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PhState *st;
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        int dpi = ph_dpi(hwnd);
        st = (PhState *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PhState));
        if (!st) return -1;
        if (cs && cs->lpCreateParams) {
            const wchar_t *name = (const wchar_t *)cs->lpCreateParams;
            lstrcpynW(st->tab_name, name, 64);
        } else {
            lstrcpyW(st->tab_name, L"");
        }
        st->font_title = ph_make_font(20, FW_SEMIBOLD, dpi);
        st->font_sub   = ph_make_font(14, FW_NORMAL,   dpi);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        return 0;
    }
    case WM_DESTROY:
        st = ph_get(hwnd);
        if (st) {
            if (st->font_title) DeleteObject(st->font_title);
            if (st->font_sub)   DeleteObject(st->font_sub);
            HeapFree(GetProcessHeap(), 0, st);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        ph_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void ph_register_class_once(void)
{
    static LONG done = 0;
    WNDCLASSEXA wc;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = PlaceholderProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = PH_CLASS;
    RegisterClassExA(&wc);
}

HWND placeholder_module_create(HWND parent, const wchar_t *tab_name)
{
    RECT rc;
    ph_register_class_once();
    if (parent) GetClientRect(parent, &rc); else { rc.left=rc.top=0; rc.right=rc.bottom=100; }
    return CreateWindowExA(0, PH_CLASS, "",
        WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        parent, NULL, GetModuleHandleA(NULL), (LPVOID)tab_name);
}
