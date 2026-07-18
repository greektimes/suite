/*
 * newspaper_print.c - Print pipeline implementation.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * View-to-pages mapping for a paper with N raw pages:
 *
 *   total views L = (N + 2) / 2  (integer division; same for even / odd N)
 *
 *   view 0           -> [1]                              cover alone
 *   view L-1, N even -> [N]                              back cover alone
 *   view L-1, N odd  -> [N-1, N]                         last spread
 *   other view k     -> [2k, 2k+1]                       interior spread
 *
 * Scope "Current page" returns the left page of the current view
 * (or the single page when the view is a cover). Scope "Current
 * spread" returns the full view list (1 or 2 pages). Scope "All
 * pages" returns [1..N].
 *
 * The print flow uses PrintDlgExW for printer selection, StartDocW
 * for the job, and per-page StartPage / EndPage. JPEG bytes come
 * from news_fetch_page_jpeg; GDI+ flat C API decodes them, then
 * GdipDrawImageRectI draws onto the printer DC with aspect
 * preserved in the printable area minus a 0.25 inch margin.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <ole2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "newspaper_print.h"

/* ------------------------------------------------------------------ */
/* GDI+ flat extern decls (same pattern as the rest of the suite).     */
/* ------------------------------------------------------------------ */

typedef struct MgtGdiplusStartupInput_ {
    UINT32 GdiplusVersion;
    void  *DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} MgtGdiplusStartupInput;

extern int  WINAPI GdiplusStartup(ULONG_PTR *token,
                                  const MgtGdiplusStartupInput *input,
                                  void *output);
extern int  WINAPI GdipCreateBitmapFromStream(IStream *stream, void **bitmap);
extern int  WINAPI GdipDisposeImage(void *bitmap);
extern int  WINAPI GdipGetImageWidth(void *bitmap, UINT *w);
extern int  WINAPI GdipGetImageHeight(void *bitmap, UINT *h);
extern int  WINAPI GdipCreateFromHDC(HDC hdc, void **graphics);
extern int  WINAPI GdipDeleteGraphics(void *graphics);
extern int  WINAPI GdipDrawImageRectI(void *graphics, void *image,
                                      INT x, INT y, INT w, INT h);
extern int  WINAPI GdipSetInterpolationMode(void *graphics, int mode);
extern int  WINAPI GdipSetSmoothingMode(void *graphics, int mode);
extern int  WINAPI GdipSetPixelOffsetMode(void *graphics, int mode);

#define MGT_INTERPOLATION_HIGHQUALITY_BICUBIC  7
#define MGT_SMOOTHING_HIGHQUALITY              2
#define MGT_PIXEL_OFFSET_HALF                  4

static ULONG_PTR g_print_gdiplus_token = 0;
static BOOL      g_print_gdiplus_inited = FALSE;

static void print_ensure_gdiplus(void)
{
    MgtGdiplusStartupInput in;
    if (g_print_gdiplus_inited) return;
    ZeroMemory(&in, sizeof(in));
    in.GdiplusVersion = 1;
    if (GdiplusStartup(&g_print_gdiplus_token, &in, NULL) == 0)
        g_print_gdiplus_inited = TRUE;
}

/* ------------------------------------------------------------------ */
/* Scope dialog.                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    NS_SCOPE_CURRENT_PAGE   = 0,
    NS_SCOPE_CURRENT_SPREAD = 1,
    NS_SCOPE_ALL_PAGES      = 2,
    NS_SCOPE_CANCEL         = -1
} NsScope;

/* Dispatch 2026-06-15 Phase 2 follow-up bug-3: the Phase 2 cut used
 * a fixed-size DLGTEMPLATEEX struct (`WCHAR title[24]` etc.). The
 * spec requires variable-length NUL-terminated strings packed
 * sequentially; the fixed padding meant `DialogBoxIndirectParamW`
 * read `pointsize` from garbage and crashed before the dialog
 * appeared. The rewrite below writes a flat byte buffer field-by-
 * field with explicit DWORD alignment between items.
 *
 * DLGTEMPLATEEX header layout (packed bytes):
 *   WORD  dlgVer (=1) ; WORD signature (=0xFFFF)
 *   DWORD helpID ; DWORD exStyle ; DWORD style
 *   WORD  cDlgItems
 *   short x ; short y ; short cx ; short cy
 *   sz_Or_Ord menu          (WORD 0 = "no menu")
 *   sz_Or_Ord windowClass   (WORD 0 = "default dialog class")
 *   WCHAR title[]           (NUL-terminated)
 *   [if DS_SETFONT in style:]
 *     WORD pointsize ; WORD weight ; BYTE italic ; BYTE charset
 *     WCHAR typeface[]      (NUL-terminated)
 *
 * DLGITEMTEMPLATEEX layout (packed; must start on DWORD boundary):
 *   DWORD helpID ; DWORD exStyle ; DWORD style
 *   short x ; short y ; short cx ; short cy
 *   DWORD id
 *   sz_Or_Ord windowClass   (WORD 0xFFFF + WORD ordinal OR string)
 *   sz_Or_Ord title         (string OR ordinal)
 *   WORD extraCount         (=0 for our items)
 */

#define IDR_NS_CURRENT_PAGE    1101
#define IDR_NS_CURRENT_SPREAD  1102
#define IDR_NS_ALL_PAGES       1103

static NsScope g_scope_result  = NS_SCOPE_CANCEL;
static NsScope g_scope_default = NS_SCOPE_CURRENT_SPREAD;

static INT_PTR CALLBACK NsScopeDlgProc(HWND hwnd, UINT msg,
                                       WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        CheckRadioButton(hwnd, IDR_NS_CURRENT_PAGE, IDR_NS_ALL_PAGES,
                         IDR_NS_CURRENT_PAGE + (int)g_scope_default);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            if (IsDlgButtonChecked(hwnd, IDR_NS_CURRENT_PAGE) == BST_CHECKED)
                g_scope_result = NS_SCOPE_CURRENT_PAGE;
            else if (IsDlgButtonChecked(hwnd, IDR_NS_CURRENT_SPREAD) == BST_CHECKED)
                g_scope_result = NS_SCOPE_CURRENT_SPREAD;
            else
                g_scope_result = NS_SCOPE_ALL_PAGES;
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            g_scope_result = NS_SCOPE_CANCEL;
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* Byte-stream writer helpers. The buffer must be large enough; we
 * use a 1 KB stack buffer below and the template tops out under
 * ~400 bytes. */
static void buf_align_dword(unsigned char **pp)
{
    while ((ULONG_PTR)(*pp) & 3) { *(*pp)++ = 0; }
}
static void buf_w16(unsigned char **pp, WORD v)
{
    (*pp)[0] = (BYTE)(v & 0xFF);
    (*pp)[1] = (BYTE)((v >> 8) & 0xFF);
    *pp += 2;
}
static void buf_w32(unsigned char **pp, DWORD v)
{
    (*pp)[0] = (BYTE)(v & 0xFF);
    (*pp)[1] = (BYTE)((v >> 8)  & 0xFF);
    (*pp)[2] = (BYTE)((v >> 16) & 0xFF);
    (*pp)[3] = (BYTE)((v >> 24) & 0xFF);
    *pp += 4;
}
static void buf_short(unsigned char **pp, short v) { buf_w16(pp, (WORD)v); }
static void buf_wstring(unsigned char **pp, const wchar_t *s)
{
    while (s && *s) { buf_w16(pp, (WORD)*s++); }
    buf_w16(pp, 0);
}
static void buf_item(unsigned char **pp,
                     DWORD ex_style, DWORD style,
                     short x, short y, short cx, short cy,
                     DWORD id, WORD ctl_class_ordinal,
                     const wchar_t *text)
{
    buf_align_dword(pp);
    buf_w32(pp, 0);              /* helpID                            */
    buf_w32(pp, ex_style);       /* exStyle                           */
    buf_w32(pp, style);          /* style                             */
    buf_short(pp, x);
    buf_short(pp, y);
    buf_short(pp, cx);
    buf_short(pp, cy);
    buf_w32(pp, id);
    buf_w16(pp, 0xFFFF);         /* class ordinal marker              */
    buf_w16(pp, ctl_class_ordinal);
    buf_wstring(pp, text);       /* title                             */
    buf_w16(pp, 0);              /* extraCount                        */
}

static NsScope ns_show_scope_dialog(HWND parent, NsScope default_choice)
{
    unsigned char  buf[1024];
    unsigned char *p = buf;
    DWORD style;

    g_scope_default = default_choice;
    g_scope_result  = NS_SCOPE_CANCEL;

    /* DLGTEMPLATEEX header. */
    style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION |
            WS_SYSMENU | DS_SETFONT;
    buf_w16  (&p, 1);                       /* dlgVer                  */
    buf_w16  (&p, 0xFFFF);                  /* signature               */
    buf_w32  (&p, 0);                       /* helpID                  */
    buf_w32  (&p, 0);                       /* exStyle                 */
    buf_w32  (&p, style);                   /* style                   */
    buf_w16  (&p, 5);                       /* cDlgItems               */
    buf_short(&p, 0);                       /* x                       */
    buf_short(&p, 0);                       /* y                       */
    buf_short(&p, 220);                     /* cx (dlg units)          */
    buf_short(&p, 110);                     /* cy                      */
    buf_w16  (&p, 0);                       /* menu       (no menu)    */
    buf_w16  (&p, 0);                       /* windowClass (default)   */
    buf_wstring(&p, L"Print Newspaper");    /* title                   */
    /* DS_SETFONT trail. */
    buf_w16  (&p, 9);                       /* pointsize               */
    buf_w16  (&p, FW_NORMAL);               /* weight                  */
    *p++ = 0;                               /* italic                  */
    *p++ = DEFAULT_CHARSET;                 /* charset                 */
    buf_wstring(&p, L"Segoe UI");           /* typeface                */

    /* Items. Window class ordinal 0x0080 = BUTTON. */
    buf_item(&p, 0,
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
        14, 12, 192, 14, IDR_NS_CURRENT_PAGE,   0x0080,
        L"Current page");
    buf_item(&p, 0,
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        14, 32, 192, 14, IDR_NS_CURRENT_SPREAD, 0x0080,
        L"Current spread");
    buf_item(&p, 0,
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        14, 52, 192, 14, IDR_NS_ALL_PAGES,      0x0080,
        L"All pages");
    buf_item(&p, 0,
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_GROUP | WS_TABSTOP,
        58, 80, 56, 18, IDOK,                    0x0080,
        L"OK");
    buf_item(&p, 0,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        124, 80, 56, 18, IDCANCEL,               0x0080,
        L"Cancel");

    DialogBoxIndirectParamW((HINSTANCE)GetModuleHandleA(NULL),
                            (LPCDLGTEMPLATE)buf, parent, NsScopeDlgProc, 0);
    return g_scope_result;
}

/* ------------------------------------------------------------------ */
/* View-to-pages mapping.                                              */
/* ------------------------------------------------------------------ */

static void ns_view_pages(int page_count, int view, int *out_left, int *out_right)
{
    int views;
    *out_left = *out_right = 0;
    if (page_count <= 0) return;
    views = (page_count + 2) / 2;
    if (view < 0 || view >= views) return;
    if (view == 0) {
        *out_left = 1;
    } else if (view == views - 1 && (page_count % 2) == 0) {
        *out_left = page_count;
    } else {
        *out_left  = 2 * view;
        *out_right = 2 * view + 1;
        if (*out_right > page_count) *out_right = 0;
    }
}

/* Build the print page list. Returns count; caller passes a buffer
 * of at least `page_count` entries. */
static int ns_build_page_list(NsScope scope, int page_count,
                              int current_view_idx, int *out)
{
    int i, left, right, n = 0;
    switch (scope) {
    case NS_SCOPE_ALL_PAGES:
        for (i = 1; i <= page_count; i++) out[n++] = i;
        return n;
    case NS_SCOPE_CURRENT_SPREAD:
        ns_view_pages(page_count, current_view_idx, &left, &right);
        if (left)  out[n++] = left;
        if (right) out[n++] = right;
        return n;
    case NS_SCOPE_CURRENT_PAGE:
    default:
        ns_view_pages(page_count, current_view_idx, &left, &right);
        if (left) out[n++] = left;
        return n;
    }
}

/* ------------------------------------------------------------------ */
/* Render one page onto the printer DC.                                */
/* ------------------------------------------------------------------ */

static BOOL print_one_page(HDC printer_dc, NewspaperService *svc,
                           const char *issue_id, int page_no_1based,
                           char *err, size_t err_cap)
{
    unsigned char *body = NULL;
    DWORD          len = 0;
    HGLOBAL        hg = NULL;
    IStream       *stream = NULL;
    void          *gp_bitmap = NULL;
    void          *graphics = NULL;
    UINT           src_w = 0, src_h = 0;
    int            page_w, page_h, margin_x, margin_y;
    int            area_w, area_h;
    int            draw_w, draw_h, draw_x, draw_y;
    BOOL           ok = FALSE;
    int            dpi_x, dpi_y;

    if (!print_one_page) return FALSE;   /* unreachable but quiet -Werror */

    if (!news_fetch_page_jpeg(svc, issue_id, page_no_1based, &body, &len) || !body) {
        _snprintf(err, err_cap, "fetch page %d failed: %s",
                  page_no_1based, news_last_error(svc));
        return FALSE;
    }

    print_ensure_gdiplus();
    hg = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hg) { _snprintf(err, err_cap, "GlobalAlloc failed"); goto cleanup; }
    {
        void *dst = GlobalLock(hg);
        if (!dst) { _snprintf(err, err_cap, "GlobalLock failed"); goto cleanup; }
        memcpy(dst, body, len);
        GlobalUnlock(hg);
    }
    if (CreateStreamOnHGlobal(hg, TRUE, &stream) != S_OK || !stream) {
        _snprintf(err, err_cap, "CreateStreamOnHGlobal failed");
        goto cleanup;
    }
    hg = NULL;   /* stream owns it now */
    if (GdipCreateBitmapFromStream(stream, &gp_bitmap) != 0 || !gp_bitmap) {
        _snprintf(err, err_cap, "GdipCreateBitmapFromStream failed");
        goto cleanup;
    }
    GdipGetImageWidth (gp_bitmap, &src_w);
    GdipGetImageHeight(gp_bitmap, &src_h);
    if (src_w == 0 || src_h == 0) {
        _snprintf(err, err_cap, "decoded image has zero dimension");
        goto cleanup;
    }

    page_w = GetDeviceCaps(printer_dc, HORZRES);
    page_h = GetDeviceCaps(printer_dc, VERTRES);
    dpi_x  = GetDeviceCaps(printer_dc, LOGPIXELSX);
    dpi_y  = GetDeviceCaps(printer_dc, LOGPIXELSY);
    margin_x = dpi_x / 4;   /* 0.25 inch */
    margin_y = dpi_y / 4;
    area_w = page_w - 2 * margin_x;
    area_h = page_h - 2 * margin_y;
    if (area_w <= 0 || area_h <= 0) {
        _snprintf(err, err_cap, "printable area too small");
        goto cleanup;
    }
    if ((double)src_w / (double)src_h >
        (double)area_w / (double)area_h) {
        draw_w = area_w;
        draw_h = (int)((double)area_w * (double)src_h / (double)src_w + 0.5);
    } else {
        draw_h = area_h;
        draw_w = (int)((double)area_h * (double)src_w / (double)src_h + 0.5);
    }
    draw_x = margin_x + (area_w - draw_w) / 2;
    draw_y = margin_y + (area_h - draw_h) / 2;

    if (GdipCreateFromHDC(printer_dc, &graphics) != 0 || !graphics) {
        _snprintf(err, err_cap, "GdipCreateFromHDC failed");
        goto cleanup;
    }
    GdipSetInterpolationMode(graphics, MGT_INTERPOLATION_HIGHQUALITY_BICUBIC);
    GdipSetSmoothingMode    (graphics, MGT_SMOOTHING_HIGHQUALITY);
    GdipSetPixelOffsetMode  (graphics, MGT_PIXEL_OFFSET_HALF);
    if (GdipDrawImageRectI(graphics, gp_bitmap,
                           draw_x, draw_y, draw_w, draw_h) != 0) {
        _snprintf(err, err_cap, "GdipDrawImageRectI failed");
        goto cleanup;
    }
    ok = TRUE;

cleanup:
    if (graphics)  GdipDeleteGraphics(graphics);
    if (gp_bitmap) GdipDisposeImage(gp_bitmap);
    if (stream)    stream->lpVtbl->Release(stream);
    if (hg)        GlobalFree(hg);
    if (body)      HeapFree(GetProcessHeap(), 0, body);
    if (err) err[err_cap - 1] = '\0';
    return ok;
}

/* ------------------------------------------------------------------ */
/* print_run.                                                          */
/* ------------------------------------------------------------------ */

/* MinGW GCC does not implement __try / __except, so dispatch A3's
 * SEH wrap from the diagnostic phase is not portable here. The
 * Bug 3 fix is structural (the DLGTEMPLATEEX rewrite above) so a
 * synchronous crash should no longer happen. If a future bug
 * triggers an access violation inside print_run we'd want to lift
 * MinGW's __try1/__except1 macros from mingw-w64-headers; not
 * necessary today. */

BOOL print_run(HWND parent, NewspaperService *svc,
               const char *issue_id, const NewspaperManifest *manifest,
               int current_view_index_0_based)
{
    NsScope         scope;
    PRINTDLGW       pd;
    DOCINFOW        di;
    int             pages[256];
    int             count;
    int             views;
    NsScope         default_scope;
    HDC             dc;
    int             i;
    char            err[320] = {0};
    wchar_t         doc_name[128];
    BOOL            ok = TRUE;

    if (!svc || !issue_id || !*issue_id || !manifest ||
        manifest->page_count <= 0)
        return FALSE;

    /* Default scope: single-page views (cover or back-when-even) ->
     * "Current page"; otherwise "Current spread". */
    views = (manifest->page_count + 2) / 2;
    if (current_view_index_0_based == 0 ||
        (current_view_index_0_based == views - 1 &&
         (manifest->page_count % 2) == 0))
        default_scope = NS_SCOPE_CURRENT_PAGE;
    else
        default_scope = NS_SCOPE_CURRENT_SPREAD;

    scope = ns_show_scope_dialog(parent, default_scope);
    if (scope == NS_SCOPE_CANCEL) return FALSE;

    if (manifest->page_count > (int)(sizeof(pages) / sizeof(pages[0])))
        count = (int)(sizeof(pages) / sizeof(pages[0]));
    else
        count = ns_build_page_list(scope, manifest->page_count,
                                   current_view_index_0_based, pages);
    if (count <= 0) return FALSE;

    ZeroMemory(&pd, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner   = parent;
    pd.Flags       = PD_RETURNDC | PD_NOSELECTION | PD_NOPAGENUMS;
    if (!PrintDlgW(&pd) || !pd.hDC) {
        if (pd.hDC) DeleteDC(pd.hDC);
        if (pd.hDevMode)  GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
        return FALSE;
    }
    dc = pd.hDC;

    _snwprintf(doc_name, sizeof(doc_name) / sizeof(doc_name[0]),
               L"Montreal Greek Times - %S", issue_id);
    doc_name[sizeof(doc_name) / sizeof(doc_name[0]) - 1] = L'\0';

    ZeroMemory(&di, sizeof(di));
    di.cbSize      = sizeof(di);
    di.lpszDocName = doc_name;
    if (StartDocW(dc, &di) <= 0) {
        MessageBoxA(parent, "StartDoc failed", "Print", MB_OK | MB_ICONERROR);
        DeleteDC(dc);
        if (pd.hDevMode)  GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
        return FALSE;
    }

    for (i = 0; i < count; i++) {
        if (StartPage(dc) <= 0) {
            ok = FALSE;
            _snprintf(err, sizeof(err), "StartPage failed at page %d", pages[i]);
            break;
        }
        if (!print_one_page(dc, svc, issue_id, pages[i],
                            err, sizeof(err))) {
            EndPage(dc);
            ok = FALSE;
            break;
        }
        if (EndPage(dc) <= 0) {
            ok = FALSE;
            _snprintf(err, sizeof(err), "EndPage failed at page %d", pages[i]);
            break;
        }
    }

    if (ok) {
        EndDoc(dc);
    } else {
        AbortDoc(dc);
        MessageBoxA(parent, err[0] ? err : "Print failed.",
                    "Print", MB_OK | MB_ICONERROR);
    }
    DeleteDC(dc);
    if (pd.hDevMode)  GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
    return ok;
}
