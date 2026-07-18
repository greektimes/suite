/*
 * website_module.c - Website tab content pane (Modern Mode).
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * A WebView2-backed reader for https://greektimes.ca/ with a four-button
 * flat toolbar (Home / Back / Forward / Refresh) in the Newspaper-toolbar
 * style, plus a domain-lock navigation policy that keeps the user inside
 * greektimes.ca and opens outbound links in the system browser.
 *
 * Sections:
 *   1. Diagnostic log layer (DEBUG_WEBSITE_MODULE, default 0).
 *   2. WsState + constants.
 *   3. Domain-lock helper + nav-start / nav-completed callbacks.
 *   4. Toolbar paint + layout.
 *   5. WndProc + create.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <wininet.h>     /* InternetCrackUrlW */
#include <shellapi.h>    /* ShellExecuteW */
#include <stdio.h>
#include <stdarg.h>

#include "website_module.h"
#include "webview2_host_service.h"

/* ------------------------------------------------------------------ */
/* GDI+ flat-C decls (MinGW gdiplus.h is C++ only). Mirrors the subset  */
/* mode_toggle.c declares, plus polygon / rectangle fills for the       */
/* toolbar pictograms. The Suite already links -lgdiplus.               */
/* ------------------------------------------------------------------ */

typedef struct GpGraphics GpGraphics;
typedef struct GpBrush    GpBrush;
typedef struct GpPen      GpPen;
typedef struct GpPath     GpPath;
typedef int   GpStatus;
typedef DWORD ARGB;

typedef struct { INT X, Y; } MgtGpPointI;

typedef struct MgtGdiplusStartupInput_ {
    UINT32 GdiplusVersion;
    void  *DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} MgtGdiplusStartupInput;

extern int      WINAPI GdiplusStartup(ULONG_PTR *token,
                                      const MgtGdiplusStartupInput *input,
                                      void *output);
extern GpStatus WINAPI GdipCreateFromHDC(HDC hdc, GpGraphics **g);
extern GpStatus WINAPI GdipDeleteGraphics(GpGraphics *g);
extern GpStatus WINAPI GdipSetSmoothingMode(GpGraphics *g, int mode);
extern GpStatus WINAPI GdipCreateSolidFill(ARGB color, GpBrush **brush);
extern GpStatus WINAPI GdipDeleteBrush(GpBrush *brush);
extern GpStatus WINAPI GdipCreatePen1(ARGB color, float width, int unit, GpPen **pen);
extern GpStatus WINAPI GdipDeletePen(GpPen *pen);
extern GpStatus WINAPI GdipDrawPath(GpGraphics *g, GpPen *pen, GpPath *path);
extern GpStatus WINAPI GdipCreatePath(int fill_mode, GpPath **path);
extern GpStatus WINAPI GdipDeletePath(GpPath *path);
extern GpStatus WINAPI GdipAddPathArcI(GpPath *path, INT x, INT y, INT w, INT h,
                                       float start_angle, float sweep_angle);
extern GpStatus WINAPI GdipFillPolygonI(GpGraphics *g, GpBrush *brush,
                                        const MgtGpPointI *points, INT count,
                                        int fill_mode);
extern GpStatus WINAPI GdipFillRectangleI(GpGraphics *g, GpBrush *brush,
                                          INT x, INT y, INT w, INT h);

#define SMOOTHING_MODE_ANTIALIAS 4
#define WS_UNIT_PIXEL            2
#define WS_FILLMODE_ALTERNATE    0

static ULONG_PTR g_ws_gdiplus_token = 0;

static void ws_ensure_gdiplus(void)
{
    MgtGdiplusStartupInput in;
    if (g_ws_gdiplus_token) return;
    in.GdiplusVersion = 1;
    in.DebugEventCallback = NULL;
    in.SuppressBackgroundThread = FALSE;
    in.SuppressExternalCodecs = FALSE;
    GdiplusStartup(&g_ws_gdiplus_token, &in, NULL);
}

static ARGB ws_argb(int r, int g, int b)
{
    return 0xFF000000u | ((ARGB)r << 16) | ((ARGB)g << 8) | (ARGB)b;
}

/* ================================================================== */
/* 1. Diagnostic log layer (mirrors radio_engine's gating).            */
/* ================================================================== */

#ifndef DEBUG_WEBSITE_MODULE
#define DEBUG_WEBSITE_MODULE 0
#endif

#if DEBUG_WEBSITE_MODULE

static FILE             *g_ws_log_fp       = NULL;
static ULONGLONG         g_ws_log_t0       = 0;
static CRITICAL_SECTION  g_ws_log_cs;
static BOOL              g_ws_log_cs_ready = FALSE;
static LONG              g_ws_log_refs     = 0;

static void wslog_init_cs_once(void)
{
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        InitializeCriticalSection(&g_ws_log_cs);
        g_ws_log_cs_ready = TRUE;
    } else {
        while (!g_ws_log_cs_ready) Sleep(0);
    }
}

static void wslog_open(void)
{
    char path[MAX_PATH];
    DWORD n;
    wslog_init_cs_once();
    EnterCriticalSection(&g_ws_log_cs);
    if (InterlockedIncrement(&g_ws_log_refs) == 1) {
        n = GetEnvironmentVariableA("TEMP", path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) lstrcpyA(path, "C:\\Windows\\Temp");
        lstrcatA(path, "\\mgt_website_module.log");
        g_ws_log_fp = fopen(path, "a");
        g_ws_log_t0 = GetTickCount64();
        if (g_ws_log_fp) {
            fprintf(g_ws_log_fp,
                "\n==== mgt_website_module.log session opened t0=%llu ====\n",
                (unsigned long long)g_ws_log_t0);
            fflush(g_ws_log_fp);
        }
    }
    LeaveCriticalSection(&g_ws_log_cs);
}

static void wslog_close(void)
{
    if (!g_ws_log_cs_ready) return;
    EnterCriticalSection(&g_ws_log_cs);
    if (InterlockedDecrement(&g_ws_log_refs) == 0) {
        if (g_ws_log_fp) { fclose(g_ws_log_fp); g_ws_log_fp = NULL; }
    }
    LeaveCriticalSection(&g_ws_log_cs);
}

static void wslog(const char *fmt, ...)
{
    char    line[512];
    int     n;
    va_list ap;
    if (!g_ws_log_cs_ready) return;
    EnterCriticalSection(&g_ws_log_cs);
    if (!g_ws_log_fp) { LeaveCriticalSection(&g_ws_log_cs); return; }
    n = _snprintf(line, sizeof(line), "%6lu  ",
                  (unsigned long)(GetTickCount64() - g_ws_log_t0));
    if (n < 0 || n >= (int)sizeof(line)) n = 0;
    va_start(ap, fmt);
    _vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 2] = '\0';
    lstrcatA(line, "\n");
    fputs(line, g_ws_log_fp);
    fflush(g_ws_log_fp);
    LeaveCriticalSection(&g_ws_log_cs);
}

#else  /* DEBUG_WEBSITE_MODULE */

#define wslog(...)      ((void)0)
static void wslog_open(void)  {}
static void wslog_close(void) {}

#endif /* DEBUG_WEBSITE_MODULE */

/* ================================================================== */
/* 2. WsState + constants.                                             */
/* ================================================================== */

#define WS_CLASS          "MGTUnicornWebsite"
#define WS_HOME_URL       L"https://greektimes.ca/"

#define WS_BAR_H          36
#define WS_BTN_H          28
#define WS_BTN_GAP         8
#define WS_BTN_MARGIN_X   12

/* Square pictogram buttons (2026-06-16 polish): width == height. */
#define WS_BTN_W          WS_BTN_H   /* 28x28 */
#define WS_ICON_DIM       16         /* inner pictogram square */

#define IDC_WS_HOME       8201
#define IDC_WS_BACK       8202
#define IDC_WS_FORWARD    8203
#define IDC_WS_REFRESH    8204

/* Posted by the nav-completed callback (WV2 thread) so the toolbar
 * enable-state update runs on the UI thread. */
#define WM_APP_WS_REFRESH_NAV_STATE  (WM_APP + 11)

/* Flat-button theme tokens. Local to this TU; identical values to the
 * Newspaper toolbar (kept separate per dispatch -- no shared symbols). */
#define WS_THEME_BAR_BG     RGB(252, 252, 253)
#define WS_THEME_BAR_SEP    RGB(220, 220, 226)
#define WS_BTN_BG_NORMAL    RGB(245, 245, 247)
#define WS_BTN_BG_HOVER     RGB(235, 235, 240)
#define WS_BTN_BG_PRESSED   RGB(220, 220, 228)
#define WS_BTN_BG_DISABLED  RGB(248, 248, 250)
#define WS_BTN_TEXT         RGB( 28,  28,  32)
#define WS_BTN_TEXT_DIS     RGB(160, 160, 168)
#define WS_BTN_ACCENT       RGB(  0, 120, 212)

typedef struct WsState {
    HWND     hwnd;
    WV2Host *wv2;

    HWND     btn_home;
    HWND     btn_back;
    HWND     btn_forward;
    HWND     btn_refresh;
    HFONT    font_btn;       /* 9 pt Segoe UI */

    HWND     hover_btn;
    BOOL     tracking_mouse;
} WsState;

static WsState *ws_get(HWND h) { return (WsState *)GetWindowLongPtrA(h, GWLP_USERDATA); }

/* ================================================================== */
/* 3. Domain-lock helper + navigation callbacks.                       */
/* ================================================================== */

/* TRUE iff `uri` should be allowed to load inside the embedded WebView2:
 * always allow non-http(s) schemes (about:, blob:, data:, edge:, ...);
 * for http/https, allow only greektimes.ca and its subdomains. Fail open
 * on any parse failure so legitimate navigation never silently breaks. */
static BOOL ws_uri_is_greektimes(const wchar_t *uri)
{
    URL_COMPONENTSW uc;
    wchar_t host[256];
    int     i;
    size_t  hl, sl;
    const wchar_t *suffix = L".greektimes.ca";

    if (!uri) return TRUE;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize   = sizeof(uc);
    uc.lpszHostName   = host;
    uc.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
    if (!InternetCrackUrlW(uri, 0, 0, &uc)) return TRUE;   /* fail open */

    if (uc.nScheme != INTERNET_SCHEME_HTTP &&
        uc.nScheme != INTERNET_SCHEME_HTTPS)
        return TRUE;                                       /* allow non-web */

    /* ASCII-lowercase the host (host names are ASCII / punycode). */
    for (i = 0; host[i]; i++)
        if (host[i] >= L'A' && host[i] <= L'Z') host[i] = (wchar_t)(host[i] + 32);

    if (lstrcmpW(host, L"greektimes.ca") == 0) return TRUE;
    hl = (size_t)lstrlenW(host);
    sl = (size_t)lstrlenW(suffix);
    if (hl >= sl && lstrcmpW(host + (hl - sl), suffix) == 0) return TRUE;
    return FALSE;
}

/* NavigationStarting hook: allow greektimes.ca; otherwise cancel the
 * in-WebView2 navigation and open the URL in the user's default browser. */
static BOOL ws_nav_start_cb(const wchar_t *uri, void *user)
{
    (void)user;
    if (ws_uri_is_greektimes(uri)) {
        wslog("nav allow: in-place");
        return TRUE;
    }
    wslog("nav block: opening externally");
    ShellExecuteW(NULL, L"open", uri, NULL, NULL, SW_SHOWNORMAL);
    return FALSE;
}

/* NavigationCompleted / HistoryChanged hook: marshal a toolbar
 * enable-state refresh to the UI thread. */
static void ws_nav_completed_cb(BOOL success, void *user)
{
    WsState *st = (WsState *)user;
    (void)success;
    if (st && st->hwnd)
        PostMessageA(st->hwnd, WM_APP_WS_REFRESH_NAV_STATE, 0, 0);
}

static void ws_refresh_nav_buttons(WsState *st)
{
    if (!st) return;
    if (st->btn_back)
        EnableWindow(st->btn_back, wv2_can_go_back(st->wv2));
    if (st->btn_forward)
        EnableWindow(st->btn_forward, wv2_can_go_forward(st->wv2));
    if (st->btn_back)    InvalidateRect(st->btn_back, NULL, TRUE);
    if (st->btn_forward) InvalidateRect(st->btn_forward, NULL, TRUE);
}

static void ws_wv2_ready_cb(WV2Host *h, void *u)
{
    WsState *st = (WsState *)u;
    (void)h;
    if (!st) return;
    wslog("wv2 ready -> navigate home");
    wv2_navigate(st->wv2, WS_HOME_URL);
}

/* ================================================================== */
/* 4. Toolbar paint + layout.                                          */
/* ================================================================== */

static HFONT ws_make_font(int pt, int weight)
{
    LOGFONTA lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight  = -MulDiv(pt, 96, 72);
    lf.lfWeight  = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyA(lf.lfFaceName, "Segoe UI");
    return CreateFontIndirectA(&lf);
}

/* ---- GDI+ pictogram helpers ----------------------------------------
 * Each draws into the 16x16 inner square whose top-left is
 * (r->left, r->top), in colour c, anti-aliased. Geometry follows the
 * dispatch's 0..15 relative sketches, offset by the inner origin. */

static void ws_draw_home(GpGraphics *g, const RECT *r, ARGB c)
{
    int ox = r->left, oy = r->top;
    GpBrush *br = NULL;
    if (GdipCreateSolidFill(c, &br) != 0) return;
    {
        MgtGpPointI roof[3] = { {ox+8,oy+2}, {ox+1,oy+8}, {ox+15,oy+8} };
        GdipFillPolygonI(g, br, roof, 3, WS_FILLMODE_ALTERNATE);
    }
    GdipFillRectangleI(g, br, ox+3, oy+8, 10, 7);   /* body (3,8)..(13,15) */
    GdipDeleteBrush(br);
}

static void ws_draw_back(GpGraphics *g, const RECT *r, ARGB c)
{
    int ox = r->left, oy = r->top;
    GpBrush *br = NULL;
    if (GdipCreateSolidFill(c, &br) != 0) return;
    {
        MgtGpPointI tri[3] = { {ox+3,oy+8}, {ox+10,oy+3}, {ox+10,oy+13} };
        GdipFillPolygonI(g, br, tri, 3, WS_FILLMODE_ALTERNATE);
    }
    GdipFillRectangleI(g, br, ox+10, oy+7, 3, 2);   /* shaft tail (10,7)..(13,9) */
    GdipDeleteBrush(br);
}

static void ws_draw_forward(GpGraphics *g, const RECT *r, ARGB c)
{
    int ox = r->left, oy = r->top;
    GpBrush *br = NULL;
    if (GdipCreateSolidFill(c, &br) != 0) return;
    {
        MgtGpPointI tri[3] = { {ox+12,oy+8}, {ox+5,oy+3}, {ox+5,oy+13} };
        GdipFillPolygonI(g, br, tri, 3, WS_FILLMODE_ALTERNATE);
    }
    GdipFillRectangleI(g, br, ox+2, oy+7, 3, 2);    /* shaft tail (2,7)..(5,9) */
    GdipDeleteBrush(br);
}

static void ws_draw_refresh(GpGraphics *g, const RECT *r, ARGB c)
{
    int ox = r->left, oy = r->top;
    GpPath  *path = NULL;
    GpPen   *pen  = NULL;
    GpBrush *br   = NULL;
    /* ~270 deg arc on a circle inscribed (1px inset) in the 16x16 box,
     * centred (8,8), radius 6; start 45 deg, sweep 270 -> open gap on the
     * top-right where the arrowhead sits. */
    if (GdipCreatePath(WS_FILLMODE_ALTERNATE, &path) == 0) {
        GdipAddPathArcI(path, ox+2, oy+2, 12, 12, 45.0f, 270.0f);
        if (GdipCreatePen1(c, 2.0f, WS_UNIT_PIXEL, &pen) == 0) {
            GdipDrawPath(g, pen, path);
            GdipDeletePen(pen);
        }
        GdipDeletePath(path);
    }
    /* Filled arrowhead near the arc's open end (~315 deg, top-right),
     * pointing roughly tangent (rightward). */
    if (GdipCreateSolidFill(c, &br) == 0) {
        MgtGpPointI head[3] = { {ox+15,oy+4}, {ox+10,oy+1}, {ox+10,oy+8} };
        GdipFillPolygonI(g, br, head, 3, WS_FILLMODE_ALTERNATE);
        GdipDeleteBrush(br);
    }
}

static void ws_paint_button(WsState *st, LPDRAWITEMSTRUCT dis)
{
    RECT     rc = dis->rcItem;
    BOOL     enabled = (dis->itemState & ODS_DISABLED) == 0;
    BOOL     pressed = (dis->itemState & ODS_SELECTED) != 0;
    BOOL     focused = (dis->itemState & ODS_FOCUS) != 0;
    BOOL     hover   = (st->hover_btn == dis->hwndItem);
    HBRUSH   br;
    COLORREF bg;
    RECT     ir;
    ARGB     col;
    GpGraphics *g = NULL;
    int      icon = WS_ICON_DIM;

    if (!enabled)     bg = WS_BTN_BG_DISABLED;
    else if (pressed) bg = WS_BTN_BG_PRESSED;
    else if (hover)   bg = WS_BTN_BG_HOVER;
    else              bg = WS_BTN_BG_NORMAL;

    /* Flat-button background (same palette as the Newspaper toolbar). */
    br = CreateSolidBrush(bg);
    FillRect(dis->hDC, &rc, br);
    DeleteObject(br);

    /* Pictogram: 16x16 inner square centered in the button. Accent colour
     * when enabled (per button), gray when disabled. The window text
     * (Home / Back / Forward / Refresh) is kept only as the accessible
     * name -- it is deliberately NOT rendered. */
    ir.left = rc.left + ((rc.right - rc.left) - icon) / 2;
    ir.top  = rc.top  + ((rc.bottom - rc.top) - icon) / 2;
    ir.right = ir.left + icon;
    ir.bottom = ir.top + icon;

    if (!enabled) col = ws_argb(160, 160, 168);          /* gray */
    else switch (dis->CtlID) {
        case IDC_WS_HOME:    col = ws_argb( 46, 160,  67); break;  /* green */
        case IDC_WS_BACK:
        case IDC_WS_FORWARD: col = ws_argb( 31, 111, 235); break;  /* blue */
        case IDC_WS_REFRESH: col = ws_argb(255, 138,   0); break;  /* orange */
        default:             col = ws_argb( 31, 111, 235); break;
    }

    ws_ensure_gdiplus();
    if (GdipCreateFromHDC(dis->hDC, &g) == 0) {
        GdipSetSmoothingMode(g, SMOOTHING_MODE_ANTIALIAS);
        switch (dis->CtlID) {
        case IDC_WS_HOME:    ws_draw_home   (g, &ir, col); break;
        case IDC_WS_BACK:    ws_draw_back   (g, &ir, col); break;
        case IDC_WS_FORWARD: ws_draw_forward(g, &ir, col); break;
        case IDC_WS_REFRESH: ws_draw_refresh(g, &ir, col); break;
        }
        GdipDeleteGraphics(g);
    }

    if (focused && enabled) {
        HPEN pen = CreatePen(PS_SOLID, 1, WS_BTN_ACCENT);
        HPEN old = (HPEN)SelectObject(dis->hDC, pen);
        int  y = rc.bottom - 4;
        MoveToEx(dis->hDC, rc.left + 6, y, NULL);
        LineTo  (dis->hDC, rc.right - 6, y);
        SelectObject(dis->hDC, old);
        DeleteObject(pen);
    }
}

static void ws_layout(HWND hwnd)
{
    WsState *st = ws_get(hwnd);
    RECT     rc;
    int      x, y;
    if (!st) return;
    GetClientRect(hwnd, &rc);

    x = WS_BTN_MARGIN_X;
    y = (WS_BAR_H - WS_BTN_H) / 2;

    if (st->btn_home)
        MoveWindow(st->btn_home, x, y, WS_BTN_W, WS_BTN_H, TRUE);
    x += WS_BTN_W + WS_BTN_GAP;
    if (st->btn_back)
        MoveWindow(st->btn_back, x, y, WS_BTN_W, WS_BTN_H, TRUE);
    x += WS_BTN_W + WS_BTN_GAP;
    if (st->btn_forward)
        MoveWindow(st->btn_forward, x, y, WS_BTN_W, WS_BTN_H, TRUE);
    x += WS_BTN_W + WS_BTN_GAP;
    if (st->btn_refresh)
        MoveWindow(st->btn_refresh, x, y, WS_BTN_W, WS_BTN_H, TRUE);

    if (st->wv2) {
        HWND wv2_child = wv2_hwnd(st->wv2);
        if (wv2_child) {
            int top = WS_BAR_H;
            int h   = rc.bottom - top;
            if (h < 0) h = 0;
            MoveWindow(wv2_child, 0, top, rc.right, h, TRUE);
            {
                RECT bounds = { 0, 0, rc.right, h };
                wv2_set_bounds(st->wv2, &bounds);
            }
        }
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

/* ================================================================== */
/* 5. WndProc + create.                                                */
/* ================================================================== */

static LRESULT CALLBACK WsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WsState *st;
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = ((LPCREATESTRUCTA)lp)->hInstance;
        wslog_open();
        st = (WsState *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*st));
        if (!st) return -1;
        st->hwnd = hwnd;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)st);

        st->font_btn = ws_make_font(9, FW_NORMAL);

        /* Window text ("Home"/"Back"/"Forward"/"Refresh") is retained as
         * the accessible name (screen readers / Magnifier announce it);
         * the owner-draw paint renders a pictogram and does NOT draw it. */
        st->btn_home = CreateWindowA("BUTTON", "Home",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, WS_BTN_W, WS_BTN_H, hwnd,
            (HMENU)(INT_PTR)IDC_WS_HOME, hInst, NULL);
        st->btn_back = CreateWindowA("BUTTON", "Back",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, WS_BTN_W, WS_BTN_H, hwnd,
            (HMENU)(INT_PTR)IDC_WS_BACK, hInst, NULL);
        st->btn_forward = CreateWindowA("BUTTON", "Forward",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, WS_BTN_W, WS_BTN_H, hwnd,
            (HMENU)(INT_PTR)IDC_WS_FORWARD, hInst, NULL);
        st->btn_refresh = CreateWindowA("BUTTON", "Refresh",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, WS_BTN_W, WS_BTN_H, hwnd,
            (HMENU)(INT_PTR)IDC_WS_REFRESH, hInst, NULL);

        /* Home + Refresh always enabled; Back / Forward gated by history. */
        EnableWindow(st->btn_back,    FALSE);
        EnableWindow(st->btn_forward, FALSE);

        /* WebView2 host. NULL user-data folder => the default
         * %LOCALAPPDATA%\MGT Unicorn Suite\WebView2 (shared with the
         * Newspaper module; the two live on separate domains). */
        st->wv2 = wv2_create(hwnd, NULL);
        if (st->wv2) {
            wv2_set_nav_start_cb    (st->wv2, ws_nav_start_cb,     st);
            wv2_set_nav_completed_cb(st->wv2, ws_nav_completed_cb, st);
            wv2_set_ready_cb        (st->wv2, ws_wv2_ready_cb,     st);
        }
        wslog("WM_CREATE wv2=%p", (void *)st->wv2);
        ws_layout(hwnd);
        return 0;
    }

    case WM_SIZE:
        ws_layout(hwnd);
        return 0;

    case WM_ERASEBKGND: {
        HDC  hdc = (HDC)wp;
        RECT rc, bar;
        HBRUSH br;
        GetClientRect(hwnd, &rc);
        bar = rc; bar.bottom = WS_BAR_H;
        br = CreateSolidBrush(WS_THEME_BAR_BG);
        FillRect(hdc, &bar, br);
        DeleteObject(br);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC  hdc;
        RECT rc, bar;
        HBRUSH bg, sep;
        st = ws_get(hwnd);
        if (!st) break;
        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        bar = rc; bar.bottom = WS_BAR_H;
        bg = CreateSolidBrush(WS_THEME_BAR_BG);
        FillRect(hdc, &bar, bg);
        DeleteObject(bg);
        sep = CreateSolidBrush(WS_THEME_BAR_SEP);
        {
            RECT line = { rc.left, WS_BAR_H - 1, rc.right, WS_BAR_H };
            FillRect(hdc, &line, sep);
        }
        DeleteObject(sep);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        st = ws_get(hwnd);
        if (st && dis && dis->CtlType == ODT_BUTTON) {
            ws_paint_button(st, dis);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, WS_THEME_BAR_BG);
        SetBkMode ((HDC)wp, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH);

    case WM_COMMAND: {
        int id   = LOWORD(wp);
        int code = HIWORD(wp);
        st = ws_get(hwnd);
        if (st && code == BN_CLICKED) {
            switch (id) {
            case IDC_WS_HOME:    wv2_navigate(st->wv2, WS_HOME_URL); break;
            case IDC_WS_BACK:    wv2_go_back(st->wv2);    break;
            case IDC_WS_FORWARD: wv2_go_forward(st->wv2); break;
            case IDC_WS_REFRESH: wv2_refresh(st->wv2);    break;
            }
        }
        return 0;
    }

    case WM_APP_WS_REFRESH_NAV_STATE:
        st = ws_get(hwnd);
        if (st) ws_refresh_nav_buttons(st);
        return 0;

    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        HWND  child = ChildWindowFromPointEx(hwnd, pt, CWP_SKIPINVISIBLE);
        st = ws_get(hwnd);
        if (!st) return 0;
        if (!st->tracking_mouse) {
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize    = sizeof(tme);
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            st->tracking_mouse = TRUE;
        }
        if (child != hwnd && (child == st->btn_home || child == st->btn_back ||
                              child == st->btn_forward || child == st->btn_refresh)) {
            if (st->hover_btn != child) {
                HWND prev = st->hover_btn;
                st->hover_btn = child;
                if (prev) InvalidateRect(prev, NULL, TRUE);
                InvalidateRect(child, NULL, TRUE);
            }
        } else if (st->hover_btn) {
            HWND prev = st->hover_btn;
            st->hover_btn = NULL;
            InvalidateRect(prev, NULL, TRUE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        st = ws_get(hwnd);
        if (st) {
            st->tracking_mouse = FALSE;
            if (st->hover_btn) {
                HWND prev = st->hover_btn;
                st->hover_btn = NULL;
                InvalidateRect(prev, NULL, TRUE);
            }
        }
        return 0;

    case WM_DESTROY:
        st = ws_get(hwnd);
        if (st) {
            if (st->wv2) { wv2_destroy(st->wv2); st->wv2 = NULL; }
            if (st->font_btn) DeleteObject(st->font_btn);
            HeapFree(GetProcessHeap(), 0, st);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        wslog_close();
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void ws_register_class_once(void)
{
    static LONG done = 0;
    WNDCLASSEXA wc;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WsProc;
    wc.hInstance     = (HINSTANCE)GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = NULL;          /* painted in WM_ERASEBKGND/WM_PAINT */
    wc.lpszClassName = WS_CLASS;
    RegisterClassExA(&wc);
}

HWND website_module_create(HWND parent)
{
    RECT rc = {0, 0, 100, 100};
    ws_register_class_once();
    if (parent) GetClientRect(parent, &rc);
    return CreateWindowExA(0, WS_CLASS, "",
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        parent, NULL, GetModuleHandleA(NULL), NULL);
}
