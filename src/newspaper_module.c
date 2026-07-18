/*
 * newspaper_module.c - Newspaper tab: modernized flat toolbar +
 *                       WebView2 host pointed at BookReader.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * Layout (top to bottom):
 *   Toolbar strip (NM_BAR_H px): three owner-drawn flat buttons
 *     (Back Issues / Save PDF / Print) left-aligned + a right-aligned
 *     "Page N of M" indicator painted in WM_PAINT.
 *   WebView2 host child: fills the remaining client area.
 *
 * The Prev / Next / Zoom- / Zoom+ / Fit W buttons that lived here in
 * Phase 1 were retired by the 2026-06-15 follow-up dispatch -- they
 * duplicated BookReader's bottom UI. The remaining toolbar exposes
 * the Suite-side actions: back-issue picker, PDF download, and the
 * Suite-driven print pipeline.
 *
 * Button paint -- the local NM_THEME_* constants below give the
 * flat-button look (no 3D bevel, hover lightens bg, press darkens,
 * focus paints a 1 px accent line under the label). The tokens are
 * deliberately local: Live TV / Live Radio still use stock theme
 * buttons; if any of them later wants the same flat style we
 * promote these tokens into a shared theme service.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "newspaper_module.h"
#include "newspaper_service.h"
#include "newspaper_picker.h"
#include "newspaper_print.h"
#include "webview2_host_service.h"
#include "suite_shell.h"

/* Mirror the wv2 service's diag log so newspaper-side events land in
 * the same %TEMP%\mgt_wv2_service.log file. */
#ifndef DEBUG_WV2_SERVICE
#define DEBUG_WV2_SERVICE 0
#endif

#if DEBUG_WV2_SERVICE
static void nm_log(const char *fmt, ...)
{
    FILE *fp; char path[MAX_PATH]; DWORD n;
    va_list ap;
    n = GetEnvironmentVariableA("TEMP", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) lstrcpyA(path, "C:\\Windows\\Temp");
    lstrcatA(path, "\\mgt_wv2_service.log");
    fp = fopen(path, "a");
    if (!fp) return;
    fprintf(fp, "  NM    ");
    va_start(ap, fmt); vfprintf(fp, fmt, ap); va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}
#else
#define nm_log(...) ((void)0)
#endif

#define NM_CLASS  "MGTUnicornNewspaper"

/* Toolbar geometry. */
#define NM_BAR_H                40
#define NM_BTN_BACK_ISSUES_W    100
#define NM_BTN_SAVE_PDF_W       86
#define NM_BTN_PRINT_W          64
#define NM_BTN_H                28
#define NM_BTN_GAP              8
#define NM_BTN_MARGIN_X         12

/* Toolbar control IDs. */
#define IDC_NM_BACK_ISSUES      8106
#define IDC_NM_SAVE_PDF         8107
#define IDC_NM_PRINT            8108

#define WM_USER_NEWS_INDEX_READY (WM_USER + 71)
#define WM_USER_NEWS_INDEX_FAIL  (WM_USER + 72)

#define NM_INDICATOR_TIMER_ID    1
#define NM_INDICATOR_TIMER_MS    1000

/* Phase 2 flat-button theme tokens. Local; promote to a shared
 * service if Live TV / Live Radio adopt the same style. */
#define NM_THEME_BAR_BG         RGB(252, 252, 253)
#define NM_THEME_BAR_SEP        RGB(220, 220, 226)
#define NM_BTN_BG_NORMAL        RGB(245, 245, 247)
#define NM_BTN_BG_HOVER         RGB(235, 235, 240)
#define NM_BTN_BG_PRESSED       RGB(220, 220, 228)
#define NM_BTN_BG_DISABLED      RGB(248, 248, 250)
#define NM_BTN_TEXT             RGB( 28,  28,  32)
#define NM_BTN_TEXT_DIS         RGB(160, 160, 168)
#define NM_BTN_ACCENT           RGB(  0, 120, 212)

typedef struct NmState {
    NewspaperService *svc;
    WV2Host          *wv2;
    HWND              hwnd;

    HWND              btn_back_issues;
    HWND              btn_save_pdf;
    HWND              btn_print;
    HFONT             font_btn;       /* 9 pt Segoe UI */

    HANDLE            index_thread;
    BOOL              wv2_ready;
    BOOL              index_loaded;
    BOOL              navigated;
    BOOL              manifest_ready;  /* gates the Print button */

    char              current_id[16];
    int               current_page_idx;   /* 0-based view index */
    int               current_total;      /* views, not raw pages */

    /* Hover tracking for owner-draw buttons. */
    HWND              hover_btn;
    BOOL              tracking_mouse;

    /* Back-issues picker child window (lazy: created on first open). */
    NewspaperPicker  *picker;
} NmState;

static NmState *nm_get(HWND h)
{ return (NmState *)GetWindowLongPtrA(h, GWLP_USERDATA); }

/* Forward decls. */
static void nm_layout(HWND hwnd);
static void nm_navigate_to_issue(NmState *st, const char *issue_id);
static void nm_kick_manifest_fetch(NmState *st);
static void nm_invalidate_btn(NmState *st, HWND btn);

/* ------------------------------------------------------------------ */
/* Worker: load index.json off the UI thread.                          */
/* ------------------------------------------------------------------ */

static DWORD WINAPI nm_index_worker(LPVOID arg)
{
    HWND     hwnd = (HWND)arg;
    NmState *st   = nm_get(hwnd);
    if (!st || !st->svc) return 0;
    if (news_load_index(st->svc))
        PostMessageA(hwnd, WM_USER_NEWS_INDEX_READY, 0, 0);
    else
        PostMessageA(hwnd, WM_USER_NEWS_INDEX_FAIL, 0, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Navigation + WV2 callbacks.                                         */
/* ------------------------------------------------------------------ */

static void nm_navigate_to_issue(NmState *st, const char *issue_id)
{
    wchar_t url[256];
    if (!st || !st->wv2 || !issue_id || !*issue_id) return;
    lstrcpynA(st->current_id, issue_id, sizeof(st->current_id));
    _snwprintf(url, sizeof(url) / sizeof(url[0]),
               L"https://newspaper.greektimes.ca/reader.html?issue=%S",
               issue_id);
    url[sizeof(url) / sizeof(url[0]) - 1] = L'\0';
    wv2_navigate(st->wv2, url);
    st->navigated = TRUE;
    st->manifest_ready = FALSE;
    EnableWindow(st->btn_print, FALSE);
}

static void nm_navigate_to_latest(NmState *st)
{
    const char *latest;
    if (!st || !st->svc) return;
    latest = news_latest_id(st->svc);
    if (!latest || !*latest) return;
    if (st->current_id[0] && strcmp(st->current_id, latest) == 0 &&
        st->navigated)
        return;
    nm_navigate_to_issue(st, latest);
}

static void nm_wv2_ready_cb(WV2Host *h, void *u)
{
    NmState *st = (NmState *)u;
    (void)h;
    if (!st) return;
    nm_log("ready_cb fired index_loaded=%d", st->index_loaded ? 1 : 0);
    st->wv2_ready = TRUE;
    if (st->index_loaded) nm_navigate_to_latest(st);
}

static void nm_wv2_nav_cb(WV2Host *h, const wchar_t *uri, void *u)
{
    NmState *st = (NmState *)u;
    (void)h; (void)uri;
    if (!st) return;
    SetTimer(st->hwnd, NM_INDICATOR_TIMER_ID, NM_INDICATOR_TIMER_MS, NULL);
    /* Print needs the manifest cached; kick the fetch off the UI thread
     * so the button enables a beat after the navigation lands. */
    nm_kick_manifest_fetch(st);
}

/* The 1 s indicator-poll handler. The script returns a wide string
 * "idx,total"; parse two integers and update the cached pair so the
 * Print scope dialog can read the current view index. (The on-toolbar
 * indicator visual was removed 2026-06-16; the poll is retained only to
 * feed Print.) */
static void nm_indicator_result_cb(const wchar_t *result, void *user)
{
    NmState *st = (NmState *)user;
    int idx = 0, total = 0;
    char ascii[64];
    int  i;
    if (!st || !result) return;
    {
        const wchar_t *p = result;
        if (*p == L'"') p++;
        for (i = 0; i < (int)sizeof(ascii) - 1 && *p && *p != L'"'; p++)
            ascii[i++] = (*p > 0 && *p < 0x80) ? (char)*p : '?';
        ascii[i] = '\0';
    }
    if (sscanf(ascii, "%d,%d", &idx, &total) != 2) return;
    if (idx < 0 || total <= 0) return;
    if (idx == st->current_page_idx && total == st->current_total) return;
    st->current_page_idx = idx;
    st->current_total    = total;
}

static void nm_wv2_msg_cb(WV2Host *h, const wchar_t *msg, void *u)
{ (void)h; (void)msg; (void)u; }

/* ------------------------------------------------------------------ */
/* Manifest worker (gates the Print button).                           */
/* ------------------------------------------------------------------ */

static DWORD WINAPI nm_manifest_worker(LPVOID arg)
{
    HWND     hwnd = (HWND)arg;
    NmState *st   = nm_get(hwnd);
    const NewspaperManifest *m;
    char     issue_local[16];
    if (!st || !st->svc) return 0;
    lstrcpynA(issue_local, st->current_id, sizeof(issue_local));
    if (!issue_local[0]) return 0;
    m = news_fetch_manifest(st->svc, issue_local);
    if (m) {
        /* Stash a "manifest is ready" signal via PostMessage so the
         * UI thread can flip the Print button enable state. */
        PostMessageA(hwnd, WM_USER + 73, 0, 0);
    }
    return 0;
}

static void nm_kick_manifest_fetch(NmState *st)
{
    HANDLE h;
    if (!st || !st->svc || !st->current_id[0]) return;
    h = CreateThread(NULL, 0, nm_manifest_worker, (LPVOID)st->hwnd, 0, NULL);
    if (h) CloseHandle(h);
}

/* ------------------------------------------------------------------ */
/* Owner-draw flat button paint.                                       */
/* ------------------------------------------------------------------ */

static void nm_paint_button(NmState *st, LPDRAWITEMSTRUCT dis)
{
    RECT     rc = dis->rcItem;
    BOOL     enabled = (dis->itemState & ODS_DISABLED) == 0;
    BOOL     pressed = (dis->itemState & ODS_SELECTED) != 0;
    BOOL     focused = (dis->itemState & ODS_FOCUS) != 0;
    BOOL     hover   = (st->hover_btn == dis->hwndItem);
    HBRUSH   br;
    char     text[64];
    int      len;
    COLORREF bg;
    COLORREF ink;
    HFONT    old_font = NULL;

    if (!enabled)         bg = NM_BTN_BG_DISABLED;
    else if (pressed)     bg = NM_BTN_BG_PRESSED;
    else if (hover)       bg = NM_BTN_BG_HOVER;
    else                  bg = NM_BTN_BG_NORMAL;
    ink = enabled ? NM_BTN_TEXT : NM_BTN_TEXT_DIS;

    br = CreateSolidBrush(bg);
    FillRect(dis->hDC, &rc, br);
    DeleteObject(br);

    SetTextColor(dis->hDC, ink);
    SetBkMode   (dis->hDC, TRANSPARENT);
    if (st->font_btn) old_font = (HFONT)SelectObject(dis->hDC, st->font_btn);
    len = GetWindowTextA(dis->hwndItem, text, (int)sizeof(text));
    DrawTextA(dis->hDC, text, len, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (old_font) SelectObject(dis->hDC, old_font);

    if (focused && enabled) {
        /* 1 px accent line under the label, inset 8 px each side. */
        HPEN pen = CreatePen(PS_SOLID, 1, NM_BTN_ACCENT);
        HPEN old = (HPEN)SelectObject(dis->hDC, pen);
        int y = rc.bottom - 4;
        MoveToEx(dis->hDC, rc.left + 10, y, NULL);
        LineTo  (dis->hDC, rc.right - 10, y);
        SelectObject(dis->hDC, old);
        DeleteObject(pen);
    }
}

static void nm_invalidate_btn(NmState *st, HWND btn)
{ (void)st; if (btn) InvalidateRect(btn, NULL, TRUE); }

/* ------------------------------------------------------------------ */
/* Layout.                                                             */
/* ------------------------------------------------------------------ */

static void nm_layout(HWND hwnd)
{
    NmState *st = nm_get(hwnd);
    RECT     rc;
    int      x, y;
    if (!st) return;
    GetClientRect(hwnd, &rc);
    nm_log("nm_layout client=%dx%d visible=%d", (int)rc.right, (int)rc.bottom,
           IsWindowVisible(hwnd) ? 1 : 0);

    x = NM_BTN_MARGIN_X;
    y = (NM_BAR_H - NM_BTN_H) / 2;

    if (st->btn_back_issues)
        MoveWindow(st->btn_back_issues, x, y,
                   NM_BTN_BACK_ISSUES_W, NM_BTN_H, TRUE);
    x += NM_BTN_BACK_ISSUES_W + NM_BTN_GAP;

    if (st->btn_save_pdf)
        MoveWindow(st->btn_save_pdf, x, y,
                   NM_BTN_SAVE_PDF_W, NM_BTN_H, TRUE);
    x += NM_BTN_SAVE_PDF_W + NM_BTN_GAP;

    if (st->btn_print)
        MoveWindow(st->btn_print, x, y,
                   NM_BTN_PRINT_W, NM_BTN_H, TRUE);

    if (st->wv2) {
        HWND wv2_child = wv2_hwnd(st->wv2);
        if (wv2_child) {
            int top = NM_BAR_H;
            int h   = rc.bottom - top;
            if (h < 0) h = 0;
            MoveWindow(wv2_child, 0, top, rc.right, h, TRUE);
            {
                RECT bounds = { 0, 0, rc.right, h };
                wv2_set_bounds(st->wv2, &bounds);
            }
            /* If the picker is alive, keep it sized to the WV2 area. */
            if (st->picker) {
                HWND pk = picker_hwnd(st->picker);
                if (pk) MoveWindow(pk, 0, top, rc.right, h, TRUE);
            }
        }
    }
    InvalidateRect(hwnd, NULL, FALSE);   /* refresh toolbar + indicator */
}

/* ------------------------------------------------------------------ */
/* Toolbar handlers.                                                   */
/* ------------------------------------------------------------------ */

static void nm_on_picked_issue(const char *issue_id, void *user)
{
    NmState *st = (NmState *)user;
    if (!st || !issue_id || !*issue_id) return;
    nm_navigate_to_issue(st, issue_id);
}

/* Phase 2 follow-up "picker invisible" amendment: fires from every
 * picker dismiss path (close button, Esc, cell-click). The WebView2
 * surface paints through a DirectComposition swap chain that ignores
 * Win32 sibling z-order, so we have to hide the controller via
 * wv2_set_visible(FALSE) at picker_show time and restore at hide
 * time. Without this, the picker is alive and on top in Win32 terms
 * but the WebView2 composition overpaints it -- the user sees the
 * BookReader through what should be the overlay. */
static void nm_on_picker_dismiss(void *user)
{
    NmState *st = (NmState *)user;
    if (st && st->wv2) wv2_set_visible(st->wv2, TRUE);
}

static void nm_on_back_issues(NmState *st)
{
    RECT rc;
    HWND pk;
    int  body_top, body_h;
    if (!st) return;
    if (!st->picker) {
        st->picker = picker_create(st->hwnd, st->svc);
        if (!st->picker) return;
        picker_set_on_pick   (st->picker, nm_on_picked_issue,   st);
        picker_set_on_dismiss(st->picker, nm_on_picker_dismiss, st);
    }
    /* Phase 2 follow-up bug-2: explicitly size + position the picker
     * to the body area (BELOW the toolbar) BEFORE showing. */
    pk = picker_hwnd(st->picker);
    if (pk) {
        GetClientRect(st->hwnd, &rc);
        body_top = NM_BAR_H;
        body_h   = rc.bottom - body_top;
        if (body_h < 0) body_h = 0;
        MoveWindow(pk, 0, body_top, rc.right, body_h, TRUE);
    }
    /* Hide the WebView2 surface so its DirectComposition swap chain
     * stops overpainting our overlay. nm_on_picker_dismiss restores
     * visibility from every dismiss path. */
    if (st->wv2) wv2_set_visible(st->wv2, FALSE);
    picker_show(st->picker);
}

static void nm_on_save_pdf(NmState *st)
{
    OPENFILENAMEW ofn;
    wchar_t       path[MAX_PATH];
    char          err[320];

    if (!st || !st->svc || !st->current_id[0]) return;

    _snwprintf(path, MAX_PATH, L"%S.pdf", st->current_id);
    path[MAX_PATH - 1] = L'\0';

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = GetAncestor(st->hwnd, GA_ROOT);
    ofn.lpstrFilter = L"PDF Files\0*.pdf\0All Files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Save newspaper PDF";
    ofn.lpstrDefExt = L"pdf";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;

    EnableWindow(st->btn_save_pdf, FALSE);
    if (news_save_pdf(st->svc, st->current_id, path)) {
        /* No success dialog; the file appearing where they chose is
         * feedback enough. */
    } else {
        _snprintf(err, sizeof(err),
                  "Couldn't save PDF.\r\n\r\n%s",
                  news_last_error(st->svc));
        err[sizeof(err) - 1] = '\0';
        MessageBoxA(GetAncestor(st->hwnd, GA_ROOT), err,
                    "Save PDF", MB_OK | MB_ICONERROR);
    }
    EnableWindow(st->btn_save_pdf, TRUE);
}

static void nm_on_print(NmState *st)
{
    const NewspaperManifest *m;
    if (!st || !st->svc || !st->current_id[0]) return;
    m = news_fetch_manifest(st->svc, st->current_id);
    if (!m) {
        MessageBoxA(GetAncestor(st->hwnd, GA_ROOT),
                    "Manifest not loaded yet -- try again in a moment.",
                    "Print", MB_OK | MB_ICONINFORMATION);
        return;
    }
    print_run(GetAncestor(st->hwnd, GA_ROOT), st->svc, st->current_id, m,
              st->current_page_idx);
}

/* ------------------------------------------------------------------ */
/* WndProc.                                                            */
/* ------------------------------------------------------------------ */

static HFONT nm_make_font(int pt, int weight)
{
    LOGFONTA lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(pt, 96, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyA(lf.lfFaceName, "Segoe UI");
    return CreateFontIndirectA(&lf);
}

static LRESULT CALLBACK NmProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    NmState *st;
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = ((LPCREATESTRUCTA)lp)->hInstance;
        st = (NmState *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*st));
        if (!st) return -1;
        st->hwnd = hwnd;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)st);

        st->svc            = news_create(NULL);
        st->font_btn       = nm_make_font( 9, FW_NORMAL);

        st->btn_back_issues = CreateWindowA("BUTTON", "Back Issues",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, NM_BTN_BACK_ISSUES_W, NM_BTN_H, hwnd,
            (HMENU)(INT_PTR)IDC_NM_BACK_ISSUES, hInst, NULL);
        st->btn_save_pdf = CreateWindowA("BUTTON", "Save PDF",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, NM_BTN_SAVE_PDF_W, NM_BTN_H, hwnd,
            (HMENU)(INT_PTR)IDC_NM_SAVE_PDF, hInst, NULL);
        st->btn_print = CreateWindowA("BUTTON", "Print",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, NM_BTN_PRINT_W, NM_BTN_H, hwnd,
            (HMENU)(INT_PTR)IDC_NM_PRINT, hInst, NULL);

        EnableWindow(st->btn_back_issues, FALSE);
        EnableWindow(st->btn_save_pdf,    FALSE);
        EnableWindow(st->btn_print,       FALSE);

        /* WebView2 host. NULL user-data folder uses the default
         * %LOCALAPPDATA%\MGT Unicorn Suite\WebView2. */
        st->wv2 = wv2_create(hwnd, NULL);
        if (st->wv2) {
            wv2_set_ready_cb(st->wv2, nm_wv2_ready_cb, st);
            wv2_set_nav_cb  (st->wv2, nm_wv2_nav_cb,   st);
            wv2_set_msg_cb  (st->wv2, nm_wv2_msg_cb,   st);
        }

        if (st->svc)
            st->index_thread = CreateThread(NULL, 0, nm_index_worker,
                                            (LPVOID)hwnd, 0, NULL);
        nm_layout(hwnd);
        return 0;
    }

    case WM_SIZE:
        nm_layout(hwnd);
        return 0;

    case WM_ERASEBKGND: {
        HDC  hdc = (HDC)wp;
        RECT rc, bar;
        HBRUSH br;
        GetClientRect(hwnd, &rc);
        bar = rc;
        bar.bottom = NM_BAR_H;
        br = CreateSolidBrush(NM_THEME_BAR_BG);
        FillRect(hdc, &bar, br);
        DeleteObject(br);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC  hdc;
        RECT rc, bar;
        HBRUSH bg, sep;
        st = nm_get(hwnd);
        if (!st) break;
        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        bar = rc; bar.bottom = NM_BAR_H;
        bg = CreateSolidBrush(NM_THEME_BAR_BG);
        FillRect(hdc, &bar, bg);
        DeleteObject(bg);

        sep = CreateSolidBrush(NM_THEME_BAR_SEP);
        {
            RECT line = { rc.left, NM_BAR_H - 1, rc.right, NM_BAR_H };
            FillRect(hdc, &line, sep);
        }
        DeleteObject(sep);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        st = nm_get(hwnd);
        if (st && dis && dis->CtlType == ODT_BUTTON) {
            nm_paint_button(st, dis);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, NM_THEME_BAR_BG);
        SetBkMode ((HDC)wp, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH);

    case WM_COMMAND: {
        int id   = LOWORD(wp);
        int code = HIWORD(wp);
        st = nm_get(hwnd);
        if (st && code == BN_CLICKED) {
            switch (id) {
            case IDC_NM_BACK_ISSUES: nm_on_back_issues(st); break;
            case IDC_NM_SAVE_PDF:    nm_on_save_pdf  (st); break;
            case IDC_NM_PRINT:       nm_on_print     (st); break;
            }
        }
        return 0;
    }

    case WM_USER_NEWS_INDEX_READY:
        st = nm_get(hwnd);
        if (!st) return 0;
        st->index_loaded = TRUE;
        EnableWindow(st->btn_back_issues, TRUE);
        nm_invalidate_btn(st, st->btn_back_issues);
        if (st->wv2_ready) nm_navigate_to_latest(st);
        return 0;

    case WM_USER_NEWS_INDEX_FAIL:
        return 0;

    case (WM_USER + 73):   /* manifest worker completed */
        st = nm_get(hwnd);
        if (st) {
            st->manifest_ready = TRUE;
            EnableWindow(st->btn_save_pdf, TRUE);
            EnableWindow(st->btn_print,    TRUE);
            nm_invalidate_btn(st, st->btn_save_pdf);
            nm_invalidate_btn(st, st->btn_print);
        }
        return 0;

    case WM_TIMER:
        if (wp == NM_INDICATOR_TIMER_ID) {
            st = nm_get(hwnd);
            if (st && st->wv2 && wv2_ready(st->wv2)) {
                wv2_eval_js(st->wv2,
                    L"(MGT_BR && typeof MGT_BR.currentIndex==='function') "
                    L"? (MGT_BR.currentIndex()+','+MGT_BR.numLeafs) "
                    L": '-1,0'",
                    nm_indicator_result_cb, st);
            }
        }
        return 0;

    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        HWND  child = ChildWindowFromPointEx(hwnd, pt, CWP_SKIPINVISIBLE);
        st = nm_get(hwnd);
        if (!st) return 0;
        if (!st->tracking_mouse) {
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize    = sizeof(tme);
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            st->tracking_mouse = TRUE;
        }
        if (child != hwnd && (child == st->btn_back_issues ||
                              child == st->btn_save_pdf ||
                              child == st->btn_print)) {
            if (st->hover_btn != child) {
                HWND prev = st->hover_btn;
                st->hover_btn = child;
                if (prev)  InvalidateRect(prev, NULL, TRUE);
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
        st = nm_get(hwnd);
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
        st = nm_get(hwnd);
        if (st) {
            KillTimer(hwnd, NM_INDICATOR_TIMER_ID);
            if (st->picker) picker_destroy(st->picker);
            if (st->index_thread) {
                WaitForSingleObject(st->index_thread, 2000);
                CloseHandle(st->index_thread);
            }
            if (st->wv2) wv2_destroy(st->wv2);
            if (st->svc) news_destroy(st->svc);
            if (st->font_btn)       DeleteObject(st->font_btn);
            HeapFree(GetProcessHeap(), 0, st);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void nm_register_class_once(void)
{
    static LONG done = 0;
    WNDCLASSEXA wc;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = NmProc;
    wc.hInstance     = (HINSTANCE)GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = NULL;          /* we paint in WM_ERASEBKGND/WM_PAINT */
    wc.lpszClassName = NM_CLASS;
    RegisterClassExA(&wc);
}

HWND newspaper_module_create(HWND parent)
{
    RECT rc = {0, 0, 100, 100};
    HWND h;
    nm_register_class_once();
    if (parent) GetClientRect(parent, &rc);
    nm_log("newspaper_module_create(parent=%p parent_vis=%d size=%dx%d)",
           (void *)parent, parent ? IsWindowVisible(parent) : -1,
           (int)(rc.right - rc.left), (int)(rc.bottom - rc.top));
    h = CreateWindowExA(0, NM_CLASS, "",
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        parent, NULL, GetModuleHandleA(NULL), NULL);
    nm_log("newspaper_module_create returned hwnd=%p visible=%d",
           (void *)h, h ? IsWindowVisible(h) : -1);
    return h;
}
