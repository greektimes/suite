/*
 * modern_mode.c - top tab strip + per-tab content area for Modern Mode.
 *
 * Tab strip: 36 px tall, full client width, four equal-width tabs.
 *   Active:   bg #FFEBEB (pale red), label #111, red #DC0000 bottom bar
 *             (MM_BORDER_PX thick; recolored from the original blue #0078D4).
 *   Hover:    bg #ECECEC.
 *   Inactive: bg #F3F3F3, label #666.
 *
 * Each tab has a 16 x 16 GDI-drawn icon (TV, speaker, folded paper, globe)
 * and a 12 pt bold Segoe UI label. The tab strip's WM_LBUTTONDOWN switches the
 * active tab; WM_MOUSEMOVE tracks hover (with TrackMouseEvent for the
 * MOUSELEAVE notification).
 */

#include "modern_mode.h"
#include "placeholder_module.h"
#include "suite_shell.h"     /* WM_APP_RESIZE_ENDED */
#include <stdio.h>

#define MM_CLASS      "MGTUnicornModernMode"
#define MM_STRIP_H    36
#define MM_LABEL_PT   12   /* was 9; bumped a few pt + bold for a clearer strip */
#define MM_ICON_PX    16
#define MM_BORDER_PX  3    /* active-tab red outline thickness (all four sides) */

typedef struct MMState {
    HWND content[MM_TAB_COUNT];
    int  active;
    int  hover;            /* -1 if none */
    HFONT font_label;
    RECT  pane_rect;       /* current outer rect inside the parent */
} MMState;

static MMState *mm_get(HWND h) { return (MMState *)GetWindowLongPtrA(h, GWLP_USERDATA); }

static const char *g_mm_tab_names[MM_TAB_COUNT] = {
    "Website", "Newspaper", "Live Radio", "Live TV"
};

static int mm_dpi(HWND h)
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

static HFONT mm_make_label_font(int dpi)
{
    LOGFONTA lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(MM_LABEL_PT, dpi, 72);
    lf.lfWeight = FW_BOLD;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyA(lf.lfFaceName, "Segoe UI");
    return CreateFontIndirectA(&lf);
}

/* ------------------------------------------------------------------ */
/* Icon primitives (16x16 base, scaled by caller via x/y/dim params).  */
/* ------------------------------------------------------------------ */

static void mm_icon_livetv(HDC hdc, int x, int y, int dim, COLORREF c)
{
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HPEN old_p = (HPEN)SelectObject(hdc, pen);
    HBRUSH old_b = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    /* TV body: rectangle in the lower 75% of the 16x16 box. */
    Rectangle(hdc, x, y + dim / 4, x + dim, y + dim);
    /* Antennae: two diagonal lines from top center. */
    MoveToEx(hdc, x + dim / 2, y + dim / 4, NULL);
    LineTo(hdc, x + dim / 4, y);
    MoveToEx(hdc, x + dim / 2, y + dim / 4, NULL);
    LineTo(hdc, x + 3 * dim / 4, y);
    SelectObject(hdc, old_p);
    SelectObject(hdc, old_b);
    DeleteObject(pen);
}

static void mm_icon_liveradio(HDC hdc, int x, int y, int dim, COLORREF c)
{
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HPEN old_p = (HPEN)SelectObject(hdc, pen);
    HBRUSH old_b = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    /* Speaker body: trapezoid (narrow inside, wide outside cone). */
    POINT pts[4] = {
        { x + dim / 8,     y + 3 * dim / 8 },
        { x + 3 * dim / 8, y + dim / 4 },
        { x + 3 * dim / 8, y + 3 * dim / 4 },
        { x + dim / 8,     y + 5 * dim / 8 }
    };
    Polygon(hdc, pts, 4);
    /* Two sound-wave arcs on the right. */
    Arc(hdc, x + dim / 2,     y + dim / 4,  x + dim,       y + 3 * dim / 4,
             x + 3 * dim / 4, y + dim / 4,  x + 3 * dim / 4, y + 3 * dim / 4);
    Arc(hdc, x + 5 * dim / 8, y + 3 * dim / 8, x + 7 * dim / 8, y + 5 * dim / 8,
             x + 3 * dim / 4, y + 3 * dim / 8, x + 3 * dim / 4, y + 5 * dim / 8);
    SelectObject(hdc, old_p);
    SelectObject(hdc, old_b);
    DeleteObject(pen);
}

static void mm_icon_newspaper(HDC hdc, int x, int y, int dim, COLORREF c)
{
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HPEN old_p = (HPEN)SelectObject(hdc, pen);
    HBRUSH old_b = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    /* Folded doc: rectangle with a corner fold indicator. */
    Rectangle(hdc, x + dim / 8, y + dim / 8, x + 7 * dim / 8, y + 7 * dim / 8);
    /* Two horizontal text lines. */
    MoveToEx(hdc, x + dim / 4, y + dim / 2 - 1, NULL);
    LineTo(hdc, x + 3 * dim / 4, y + dim / 2 - 1);
    MoveToEx(hdc, x + dim / 4, y + dim / 2 + 2, NULL);
    LineTo(hdc, x + 3 * dim / 4, y + dim / 2 + 2);
    SelectObject(hdc, old_p);
    SelectObject(hdc, old_b);
    DeleteObject(pen);
}

static void mm_icon_website(HDC hdc, int x, int y, int dim, COLORREF c)
{
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HPEN old_p = (HPEN)SelectObject(hdc, pen);
    HBRUSH old_b = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    /* Globe outline + two latitude lines + one longitude line. */
    Ellipse(hdc, x + dim / 8, y + dim / 8, x + 7 * dim / 8, y + 7 * dim / 8);
    MoveToEx(hdc, x + dim / 8, y + dim / 2, NULL);
    LineTo(hdc, x + 7 * dim / 8, y + dim / 2);
    Arc(hdc, x + dim / 4, y + dim / 8, x + 3 * dim / 4, y + 7 * dim / 8,
             x + dim / 2, y + dim / 8, x + dim / 2, y + 7 * dim / 8);
    SelectObject(hdc, old_p);
    SelectObject(hdc, old_b);
    DeleteObject(pen);
}

typedef void (*mm_icon_fn)(HDC, int, int, int, COLORREF);
static const mm_icon_fn g_mm_icons[MM_TAB_COUNT] = {
    mm_icon_website, mm_icon_newspaper, mm_icon_liveradio, mm_icon_livetv
};

/* ------------------------------------------------------------------ */
/* Layout + paint.                                                     */
/* ------------------------------------------------------------------ */

static void mm_layout_children(HWND hwnd)
{
    MMState *st = mm_get(hwnd);
    RECT rc;
    int strip_h, i, w, h;
    if (!st) return;
    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    strip_h = MulDiv(MM_STRIP_H, mm_dpi(hwnd), 96);
    for (i = 0; i < MM_TAB_COUNT; i++) {
        if (st->content[i]) {
            MoveWindow(st->content[i], 0, strip_h, w, h - strip_h, TRUE);
        }
    }
}

static int mm_strip_h_px(HWND hwnd)
{
    return MulDiv(MM_STRIP_H, mm_dpi(hwnd), 96);
}

static int mm_tab_at_x(HWND hwnd, int x, int y, int client_w)
{
    int tab_w;
    int strip_h = mm_strip_h_px(hwnd);
    if (y < 0 || y >= strip_h) return -1;
    tab_w = client_w / MM_TAB_COUNT;
    if (tab_w <= 0) return -1;
    {
        int t = x / tab_w;
        if (t < 0 || t >= MM_TAB_COUNT) return -1;
        return t;
    }
}

static void mm_paint_strip(HWND hwnd, HDC hdc_target)
{
    MMState *st = mm_get(hwnd);
    RECT rc;
    HDC mem_dc;
    HBITMAP mem_bm, old_bm;
    int w, strip_h, tab_w, i, dpi;
    HBRUSH br_active, br_hover, br_inactive, br_border;
    HFONT old_f;
    if (!st) return;
    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    if (w <= 0) return;
    dpi = mm_dpi(hwnd);
    strip_h = mm_strip_h_px(hwnd);
    tab_w = w / MM_TAB_COUNT;
    if (tab_w <= 0) return;

    mem_dc = CreateCompatibleDC(hdc_target);
    mem_bm = CreateCompatibleBitmap(hdc_target, w, strip_h);
    old_bm = (HBITMAP)SelectObject(mem_dc, mem_bm);

    br_active   = CreateSolidBrush(RGB(255, 235, 235));   /* pale red fill */
    br_hover    = CreateSolidBrush(RGB(236, 236, 236));
    br_inactive = CreateSolidBrush(RGB(243, 243, 243));
    br_border   = CreateSolidBrush(RGB(220, 0, 0));        /* bright red outline */

    SetBkMode(mem_dc, TRANSPARENT);
    old_f = (HFONT)SelectObject(mem_dc, st->font_label);

    for (i = 0; i < MM_TAB_COUNT; i++) {
        RECT tr;
        BOOL active  = (i == st->active);
        BOOL hover   = (i == st->hover && !active);
        HBRUSH bg    = active ? br_active : (hover ? br_hover : br_inactive);
        COLORREF lc  = active ? RGB(17, 17, 17) : RGB(102, 102, 102);
        SIZE sz;
        int icon_dim = MulDiv(MM_ICON_PX, dpi, 96);
        int gap      = MulDiv(6, dpi, 96);
        int label_len = (int)strlen(g_mm_tab_names[i]);
        int icon_x, label_x, label_y;
        int border_px = MulDiv(MM_BORDER_PX, dpi, 96);

        tr.left   = i * tab_w;
        tr.top    = 0;
        tr.right  = (i == MM_TAB_COUNT - 1) ? w : (i + 1) * tab_w;
        tr.bottom = strip_h;
        FillRect(mem_dc, &tr, bg);

        SetTextColor(mem_dc, lc);
        GetTextExtentPoint32A(mem_dc, g_mm_tab_names[i], label_len, &sz);

        /* Center icon + gap + label horizontally inside the tab. */
        {
            int content_w = icon_dim + gap + sz.cx;
            icon_x  = tr.left + (tab_w - content_w) / 2;
            label_x = icon_x + icon_dim + gap;
        }
        label_y = (strip_h - sz.cy) / 2;
        {
            int icon_y = (strip_h - icon_dim) / 2;
            g_mm_icons[i](mem_dc, icon_x, icon_y, icon_dim, lc);
        }
        TextOutA(mem_dc, label_x, label_y, g_mm_tab_names[i], label_len);

        if (active) {
            /* Bottom-bar highlight only: a single border_px-thick red
             * FillRect along the bottom edge of the active tab (like the
             * original blue underline, but red RGB(220,0,0) to match the
             * pale-red fill br_active laid down before the icon+label).
             * No top/left/right sides. */
            RECT e;
            e.left = tr.left; e.right = tr.right;
            e.top = tr.bottom - border_px; e.bottom = tr.bottom;
            FillRect(mem_dc, &e, br_border);
        }
    }

    SelectObject(mem_dc, old_f);

    DeleteObject(br_active);
    DeleteObject(br_hover);
    DeleteObject(br_inactive);
    DeleteObject(br_border);

    BitBlt(hdc_target, 0, 0, w, strip_h, mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bm);
    DeleteObject(mem_bm);
    DeleteDC(mem_dc);
}

static void mm_set_active(HWND hwnd, int tab)
{
    MMState *st = mm_get(hwnd);
    int i;
    if (!st) return;
    if (tab < 0 || tab >= MM_TAB_COUNT) return;
    if (tab == st->active) return;
    st->active = tab;
    for (i = 0; i < MM_TAB_COUNT; i++) {
        if (st->content[i])
            ShowWindow(st->content[i], i == tab ? SW_SHOW : SW_HIDE);
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static LRESULT CALLBACK ModernModeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MMState *st;
    switch (msg) {
    case WM_CREATE: {
        int dpi = mm_dpi(hwnd);
        int i;
        st = (MMState *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MMState));
        if (!st) return -1;
        st->active = MM_TAB_WEBSITE;
        st->hover  = -1;
        st->font_label = mm_make_label_font(dpi);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        /* Create placeholders for ALL four tabs at construct time; the
         * shell swaps each for its real module via modern_mode_set_tab_child
         * (keyed by the MM_TAB_* constants, not by position). Default
         * active tab at launch is Website (the leftmost tab); switch_to_mode
         * restores the per-mode remembered tab on subsequent toggles. */
        for (i = 0; i < MM_TAB_COUNT; i++) {
            wchar_t wname[64];
            MultiByteToWideChar(CP_UTF8, 0, g_mm_tab_names[i], -1, wname, 64);
            st->content[i] = placeholder_module_create(hwnd, wname);
            if (st->content[i]) {
                ShowWindow(st->content[i], i == st->active ? SW_SHOW : SW_HIDE);
            }
        }
        mm_layout_children(hwnd);
        return 0;
    }

    case WM_DESTROY:
        st = mm_get(hwnd);
        if (st) {
            int i;
            for (i = 0; i < MM_TAB_COUNT; i++)
                if (st->content[i]) DestroyWindow(st->content[i]);
            if (st->font_label) DeleteObject(st->font_label);
            HeapFree(GetProcessHeap(), 0, st);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        return 0;

    case WM_SIZE:
        mm_layout_children(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_ERASEBKGND: {
        /* Fill below the strip with white in case the active tab child
         * has not painted yet (avoids flash on first show). */
        HDC hdc = (HDC)wp;
        RECT rc;
        HBRUSH wb = (HBRUSH)GetStockObject(WHITE_BRUSH);
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, wb);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        mm_paint_strip(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = (int)(short)LOWORD(lp);
        int y = (int)(short)HIWORD(lp);
        RECT rc;
        int t;
        GetClientRect(hwnd, &rc);
        t = mm_tab_at_x(hwnd, x, y, rc.right - rc.left);
        if (t >= 0) mm_set_active(hwnd, t);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(lp);
        int y = (int)(short)HIWORD(lp);
        RECT rc;
        int t;
        TRACKMOUSEEVENT tme;
        GetClientRect(hwnd, &rc);
        t = mm_tab_at_x(hwnd, x, y, rc.right - rc.left);
        st = mm_get(hwnd);
        if (st && st->hover != t) {
            st->hover = t;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        st = mm_get(hwnd);
        if (st && st->hover != -1) {
            st->hover = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_APP_RESIZE_ENDED:
        /* Forward to the active tab's content HWND so its MF consumer
         * (livetv_module on tab 0) can rebuild its swap chain at the
         * stabilized size now that the drag has ended. */
        st = mm_get(hwnd);
        if (st && st->active >= 0 && st->active < MM_TAB_COUNT &&
            st->content[st->active]) {
            SendMessageA(st->content[st->active], WM_APP_RESIZE_ENDED,
                         wp, lp);
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void mm_register_class_once(void)
{
    static LONG done = 0;
    WNDCLASSEXA wc;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ModernModeProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = MM_CLASS;
    RegisterClassExA(&wc);
}

HWND modern_mode_create(HWND parent, RECT content_rect)
{
    HWND hwnd;
    mm_register_class_once();
    hwnd = CreateWindowExA(0, MM_CLASS, "",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        content_rect.left, content_rect.top,
        content_rect.right - content_rect.left,
        content_rect.bottom - content_rect.top,
        parent, NULL, GetModuleHandleA(NULL), NULL);
    if (hwnd) {
        MMState *st = mm_get(hwnd);
        if (st) st->pane_rect = content_rect;
    }
    return hwnd;
}

void modern_mode_resize(HWND hwnd, RECT new_content_rect)
{
    MMState *st;
    if (!hwnd) return;
    st = mm_get(hwnd);
    if (st) st->pane_rect = new_content_rect;
    MoveWindow(hwnd, new_content_rect.left, new_content_rect.top,
        new_content_rect.right - new_content_rect.left,
        new_content_rect.bottom - new_content_rect.top, TRUE);
}

void modern_mode_set_active_tab(HWND hwnd, int tab_index)
{
    mm_set_active(hwnd, tab_index);
}

int modern_mode_get_active_tab(HWND hwnd)
{
    MMState *st = mm_get(hwnd);
    return st ? st->active : 0;
}

void modern_mode_set_tab_child(HWND hwnd, int tab_index, HWND new_child)
{
    MMState *st = mm_get(hwnd);
    RECT rc;
    int strip_h, w, h;
    if (!st || tab_index < 0 || tab_index >= MM_TAB_COUNT) return;
    if (st->content[tab_index] == new_child) return;
    if (st->content[tab_index]) DestroyWindow(st->content[tab_index]);
    st->content[tab_index] = new_child;
    if (new_child) {
        SetParent(new_child, hwnd);
        GetClientRect(hwnd, &rc);
        strip_h = mm_strip_h_px(hwnd);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top - strip_h;
        if (h < 0) h = 0;
        MoveWindow(new_child, 0, strip_h, w, h, TRUE);
        ShowWindow(new_child, tab_index == st->active ? SW_SHOW : SW_HIDE);
    }
}
