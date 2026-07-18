/*
 * newspaper_picker.c - Back-issues overlay implementation.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * The picker is a single child HWND on top of the WebView2. Cover
 * thumbnails are fetched lazily by a worker thread that walks the
 * index, calls news_cover_thumb for each issue (which decodes the
 * JPEG and caches the HBITMAP), then posts WM_USER_COVER_READY to
 * the picker's UI thread for invalidation.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "newspaper_picker.h"
#include "newspaper_service.h"

#define NP_CLASS  "MGTUnicornNewspaperPicker"

/* Geometry (px at 96 DPI). */
#define NP_PAD                  24
#define NP_COLS                  4
#define NP_GUTTER_X             16
#define NP_GUTTER_Y             20
#define NP_CELL_W              220
#define NP_THUMB_W             200
#define NP_THUMB_H             280
#define NP_LABEL_AREA_H        56
#define NP_CELL_H             (NP_THUMB_H + NP_LABEL_AREA_H)
#define NP_CLOSE_BTN_SZ        24
#define NP_CLOSE_BTN_MARGIN    12
#define NP_TITLE_H             36
#define NP_BG                  RGB(248, 248, 250)
#define NP_TITLE_TEXT          RGB( 28,  28,  32)
#define NP_CELL_BG             RGB(255, 255, 255)
#define NP_CELL_BG_HOVER       RGB(243, 244, 248)
#define NP_CELL_BORDER         RGB(220, 220, 226)
#define NP_ID_TEXT             RGB( 24,  24,  28)
#define NP_TITLE_LINE_TEXT     RGB( 80,  80,  92)
#define NP_CLOSE_BG            RGB(243, 243, 247)
#define NP_CLOSE_BG_HOVER      RGB(228, 228, 235)
#define NP_CLOSE_TEXT          RGB( 60,  60,  72)

#define WM_USER_COVER_READY   (WM_USER + 80)

struct NewspaperPicker {
    HWND              hwnd;
    HWND              parent_module;
    NewspaperService *svc;
    HFONT             font_title;
    HFONT             font_id;
    HFONT             font_label;
    HFONT             font_close;

    int               scroll_y;       /* current vertical scroll (px) */
    int               content_h;      /* total content height (px) */
    int               hover_idx;      /* -1 = none, -2 = close button */

    HANDLE            cover_thread;
    BOOL              fetching;

    NsPickerOnPick    on_pick;
    void             *on_pick_user;
    NsPickerOnDismiss on_dismiss;
    void             *on_dismiss_user;
};

static NewspaperPicker *p_get(HWND h)
{ return (NewspaperPicker *)GetWindowLongPtrA(h, GWLP_USERDATA); }

/* ------------------------------------------------------------------ */
/* Helpers.                                                            */
/* ------------------------------------------------------------------ */

static int p_total_count(NewspaperPicker *p)
{ return p ? news_issue_count(p->svc) : 0; }

static int p_rows(NewspaperPicker *p)
{
    int n = p_total_count(p);
    return n > 0 ? ((n + NP_COLS - 1) / NP_COLS) : 0;
}

static void p_compute_geometry(NewspaperPicker *p, int *out_grid_x0,
                               int *out_grid_y0)
{
    RECT rc;
    int  grid_w, grid_h_unused, x0;
    (void)grid_h_unused;
    if (!p) return;
    GetClientRect(p->hwnd, &rc);
    grid_w = NP_COLS * NP_CELL_W + (NP_COLS - 1) * NP_GUTTER_X;
    x0 = (rc.right - grid_w) / 2;
    if (x0 < NP_PAD) x0 = NP_PAD;
    *out_grid_x0 = x0;
    *out_grid_y0 = NP_TITLE_H + NP_PAD;
}

static void p_compute_content_h(NewspaperPicker *p)
{
    int rows = p_rows(p);
    p->content_h = NP_TITLE_H + NP_PAD +
                   rows * NP_CELL_H +
                   (rows > 0 ? (rows - 1) * NP_GUTTER_Y : 0) +
                   NP_PAD;
}

static int p_cell_index_at(NewspaperPicker *p, int x_px, int y_px)
{
    int grid_x0, grid_y0;
    int n = p_total_count(p);
    int col, row, cell_x, cell_y, cell_idx;
    if (!p || n <= 0) return -1;
    p_compute_geometry(p, &grid_x0, &grid_y0);
    cell_x = x_px - grid_x0;
    cell_y = y_px + p->scroll_y - grid_y0;
    if (cell_x < 0 || cell_y < 0) return -1;
    col = cell_x / (NP_CELL_W + NP_GUTTER_X);
    row = cell_y / (NP_CELL_H + NP_GUTTER_Y);
    if (col >= NP_COLS) return -1;
    /* Reject hits in the gutters. */
    if (cell_x - col * (NP_CELL_W + NP_GUTTER_X) > NP_CELL_W) return -1;
    if (cell_y - row * (NP_CELL_H + NP_GUTTER_Y) > NP_CELL_H) return -1;
    cell_idx = row * NP_COLS + col;
    if (cell_idx >= n) return -1;
    return cell_idx;
}

static void p_close_rect(NewspaperPicker *p, RECT *out)
{
    RECT rc;
    if (!p) return;
    GetClientRect(p->hwnd, &rc);
    out->right  = rc.right  - NP_CLOSE_BTN_MARGIN;
    out->top    = NP_CLOSE_BTN_MARGIN;
    out->left   = out->right - NP_CLOSE_BTN_SZ;
    out->bottom = out->top   + NP_CLOSE_BTN_SZ;
}

static void p_update_scrollbar(NewspaperPicker *p)
{
    SCROLLINFO si;
    RECT rc;
    if (!p) return;
    GetClientRect(p->hwnd, &rc);
    p_compute_content_h(p);
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin   = 0;
    si.nMax   = p->content_h > 0 ? p->content_h - 1 : 0;
    si.nPage  = rc.bottom > 0 ? rc.bottom : 1;
    si.nPos   = p->scroll_y;
    SetScrollInfo(p->hwnd, SB_VERT, &si, TRUE);
    /* Clamp scroll position after geometry change. */
    if (p->scroll_y > p->content_h - (int)si.nPage)
        p->scroll_y = p->content_h - (int)si.nPage;
    if (p->scroll_y < 0) p->scroll_y = 0;
}

/* ------------------------------------------------------------------ */
/* Cover worker.                                                       */
/* ------------------------------------------------------------------ */

static DWORD WINAPI p_cover_worker(LPVOID arg)
{
    NewspaperPicker *p = (NewspaperPicker *)arg;
    int i, n;
    if (!p) return 0;
    n = p_total_count(p);
    for (i = 0; i < n; i++) {
        const NewspaperIssue *it = news_issue_at(p->svc, i);
        if (!it || !it->id[0]) continue;
        news_cover_thumb(p->svc, it->id, NP_THUMB_W, NP_THUMB_H);
        if (IsWindow(p->hwnd))
            PostMessageA(p->hwnd, WM_USER_COVER_READY, (WPARAM)i, 0);
    }
    p->fetching = FALSE;
    return 0;
}

static void p_start_cover_worker(NewspaperPicker *p)
{
    if (!p || p->fetching || p->cover_thread) return;
    p->fetching = TRUE;
    p->cover_thread = CreateThread(NULL, 0, p_cover_worker, (LPVOID)p,
                                   0, NULL);
}

/* ------------------------------------------------------------------ */
/* Paint.                                                              */
/* ------------------------------------------------------------------ */

static void p_paint_close_button(NewspaperPicker *p, HDC hdc)
{
    RECT  rc;
    HBRUSH bg;
    HFONT  old_font = NULL;
    BOOL   hover;
    p_close_rect(p, &rc);
    hover = (p->hover_idx == -2);
    bg = CreateSolidBrush(hover ? NP_CLOSE_BG_HOVER : NP_CLOSE_BG);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    SetTextColor(hdc, NP_CLOSE_TEXT);
    SetBkMode   (hdc, TRANSPARENT);
    if (p->font_close) old_font = (HFONT)SelectObject(hdc, p->font_close);
    DrawTextA(hdc, "X", 1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (old_font) SelectObject(hdc, old_font);
}

static void p_paint_cell(NewspaperPicker *p, HDC hdc, int idx,
                         int cx, int cy)
{
    const NewspaperIssue *it = news_issue_at(p->svc, idx);
    RECT cell  = { cx, cy, cx + NP_CELL_W, cy + NP_CELL_H };
    RECT thumb = { cx + (NP_CELL_W - NP_THUMB_W) / 2, cy,
                   cx + (NP_CELL_W - NP_THUMB_W) / 2 + NP_THUMB_W,
                   cy + NP_THUMB_H };
    RECT label = { cx, cy + NP_THUMB_H + 6, cx + NP_CELL_W,
                   cy + NP_THUMB_H + NP_LABEL_AREA_H };
    RECT title_line;
    HBRUSH bg;
    HBRUSH border;
    HPEN   pen, old_pen;
    HBRUSH hollow;
    BOOL   hover = (p->hover_idx == idx);
    HBITMAP thumb_bmp;
    HFONT old_font = NULL;

    if (!it) return;

    bg = CreateSolidBrush(hover ? NP_CELL_BG_HOVER : NP_CELL_BG);
    FillRect(hdc, &cell, bg);
    DeleteObject(bg);

    border = CreateSolidBrush(NP_CELL_BORDER);
    FrameRect(hdc, &cell, border);
    DeleteObject(border);
    (void)pen; (void)old_pen; (void)hollow;   /* reserved */

    /* Phase 2 follow-up bug-1: the UI thread paint must NEVER block
     * on a synchronous fetch. Use news_cover_peek (cache-only). If
     * the cover isn't ready yet, the cell paints with the white
     * card + id + title text; the worker thread will PostMessage
     * WM_USER_COVER_READY when the cover lands, triggering a repaint. */
    thumb_bmp = news_cover_peek(p->svc, it->id, NP_THUMB_W, NP_THUMB_H);
    if (thumb_bmp) {
        HDC src = CreateCompatibleDC(hdc);
        HBITMAP old = (HBITMAP)SelectObject(src, thumb_bmp);
        BitBlt(hdc, thumb.left, thumb.top, NP_THUMB_W, NP_THUMB_H,
               src, 0, 0, SRCCOPY);
        SelectObject(src, old);
        DeleteDC(src);
    }

    /* Cell labels: editorial direction 2026-06-15 cosmetic dispatch.
     * Bold line: "GREEK TIMES #<N>"  (strip the "gt" prefix).
     * Regular line: the date suffix after " - " in the manifest
     * title (e.g. "MAY 2026"). Both paths fall back to the raw
     * id / title if the expected format is missing, so the cell
     * never blanks out. */
    {
        char        top[32];
        const char *num = it->id;
        if (num[0] == 'g' && num[1] == 't' && num[2] != '\0')
            _snprintf(top, sizeof(top), "GREEK TIMES #%s", num + 2);
        else
            _snprintf(top, sizeof(top), "%s", it->id);
        top[sizeof(top) - 1] = '\0';

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, NP_ID_TEXT);
        if (p->font_id) old_font = (HFONT)SelectObject(hdc, p->font_id);
        DrawTextA(hdc, top, -1, &label,
                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        if (old_font) SelectObject(hdc, old_font);
    }

    {
        const char *date = it->title;
        const char *sep  = strstr(it->title, " - ");
        if (sep) date = sep + 3;

        title_line = label;
        title_line.top += 22;
        SetTextColor(hdc, NP_TITLE_LINE_TEXT);
        if (p->font_label) old_font = (HFONT)SelectObject(hdc, p->font_label);
        DrawTextA(hdc, date, -1, &title_line,
                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        if (old_font) SelectObject(hdc, old_font);
    }
}

static void p_paint(NewspaperPicker *p, HDC hdc)
{
    RECT rc, title_rc;
    HBRUSH bg;
    HFONT  old_font = NULL;
    int    grid_x0, grid_y0;
    int    n = p_total_count(p);
    int    i;

    GetClientRect(p->hwnd, &rc);
    bg = CreateSolidBrush(NP_BG);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    title_rc = rc;
    title_rc.bottom = NP_TITLE_H;
    title_rc.left  += NP_PAD;
    SetTextColor(hdc, NP_TITLE_TEXT);
    SetBkMode   (hdc, TRANSPARENT);
    if (p->font_title) old_font = (HFONT)SelectObject(hdc, p->font_title);
    DrawTextA(hdc, "Back Issues", -1, &title_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (old_font) SelectObject(hdc, old_font);

    p_paint_close_button(p, hdc);

    p_compute_geometry(p, &grid_x0, &grid_y0);
    for (i = 0; i < n; i++) {
        int col = i % NP_COLS;
        int row = i / NP_COLS;
        int cx  = grid_x0 + col * (NP_CELL_W + NP_GUTTER_X);
        int cy  = grid_y0 + row * (NP_CELL_H + NP_GUTTER_Y) - p->scroll_y;
        if (cy + NP_CELL_H < NP_TITLE_H) continue;
        if (cy > rc.bottom) break;
        p_paint_cell(p, hdc, i, cx, cy);
    }
}

/* ------------------------------------------------------------------ */
/* WndProc.                                                            */
/* ------------------------------------------------------------------ */

static HFONT p_make_font(int pt, int weight)
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

static LRESULT CALLBACK NpProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    NewspaperPicker *p;
    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc;
        HDC mem;
        HBITMAP bmp, old;
        RECT rc;
        p = p_get(hwnd);
        if (!p) break;
        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        mem = CreateCompatibleDC(hdc);
        bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        old = (HBITMAP)SelectObject(mem, bmp);
        p_paint(p, mem);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        p = p_get(hwnd);
        if (p) p_update_scrollbar(p);
        return 0;

    case WM_USER_COVER_READY:
        /* A cover became available -- just repaint the whole picker;
         * the cached HBITMAPs make subsequent paints cheap. */
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_VSCROLL: {
        SCROLLINFO si;
        int new_pos;
        p = p_get(hwnd);
        if (!p) break;
        ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask  = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        new_pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP:       new_pos -= 40;                break;
        case SB_LINEDOWN:     new_pos += 40;                break;
        case SB_PAGEUP:       new_pos -= si.nPage;          break;
        case SB_PAGEDOWN:     new_pos += si.nPage;          break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:new_pos = (int)HIWORD(wp);    break;
        }
        if (new_pos < 0) new_pos = 0;
        if (new_pos > si.nMax - (int)si.nPage + 1)
            new_pos = si.nMax - (int)si.nPage + 1;
        if (new_pos < 0) new_pos = 0;
        if (new_pos != p->scroll_y) {
            p->scroll_y = new_pos;
            si.fMask = SIF_POS;
            si.nPos  = new_pos;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        SHORT delta = (SHORT)HIWORD(wp);
        int steps = delta / WHEEL_DELTA;
        if (steps == 0) steps = delta > 0 ? 1 : -1;
        SendMessageA(hwnd, WM_VSCROLL,
                     MAKEWPARAM(steps > 0 ? SB_LINEUP : SB_LINEDOWN, 0), 0);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT close_rc;
        int  prev_hover;
        p = p_get(hwnd);
        if (!p) break;
        prev_hover = p->hover_idx;
        p_close_rect(p, &close_rc);
        if (PtInRect(&close_rc, (POINT){x, y}))
            p->hover_idx = -2;
        else
            p->hover_idx = p_cell_index_at(p, x, y);
        if (p->hover_idx != prev_hover) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT close_rc;
        int idx;
        p = p_get(hwnd);
        if (!p) break;
        p_close_rect(p, &close_rc);
        if (PtInRect(&close_rc, (POINT){x, y})) {
            picker_hide(p);
            return 0;
        }
        idx = p_cell_index_at(p, x, y);
        if (idx >= 0) {
            const NewspaperIssue *it = news_issue_at(p->svc, idx);
            char id_buf[16] = {0};
            NsPickerOnPick cb = p->on_pick;
            void *cb_user     = p->on_pick_user;
            if (it) lstrcpynA(id_buf, it->id, sizeof(id_buf));
            /* Hide FIRST so the dismiss callback restores the
             * underlying WebView2 visibility before the consumer
             * navigates it; otherwise the new issue would load
             * into a hidden surface and the user would see a
             * black gap until they alt-tab. */
            picker_hide(p);
            if (id_buf[0] && cb) cb(id_buf, cb_user);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            p = p_get(hwnd);
            if (p) picker_hide(p);
            return 0;
        }
        break;

    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void p_register_class_once(void)
{
    static LONG done = 0;
    WNDCLASSEXA wc;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = NpProc;
    wc.hInstance     = (HINSTANCE)GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_HAND);
    wc.hbrBackground = NULL;
    wc.lpszClassName = NP_CLASS;
    RegisterClassExA(&wc);
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */

NewspaperPicker *picker_create(HWND parent_module, NewspaperService *svc)
{
    NewspaperPicker *p;
    if (!parent_module || !svc) return NULL;
    p_register_class_once();
    p = (NewspaperPicker *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                     sizeof(*p));
    if (!p) return NULL;
    p->parent_module = parent_module;
    p->svc           = svc;
    p->hover_idx     = -1;
    p->font_title    = p_make_font(15, FW_SEMIBOLD);
    p->font_id       = p_make_font(11, FW_SEMIBOLD);
    p->font_label    = p_make_font( 9, FW_NORMAL);
    p->font_close    = p_make_font(11, FW_SEMIBOLD);
    /* Phase 2 follow-up bug-2: create at 1 x 1 and let the module's
     * nm_layout MoveWindow us into position. Previously we used
     * the parent's full GetClientRect which included the toolbar
     * band, so the picker's title strip painted ON TOP of the
     * Back Issues / Save PDF / Print buttons. The module now calls
     * MoveWindow(picker, 0, NM_BAR_H, w, h - NM_BAR_H) before
     * picker_show in nm_on_back_issues. */
    p->hwnd = CreateWindowExA(0, NP_CLASS, "",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VSCROLL,
        0, 0, 1, 1,
        parent_module, NULL, GetModuleHandleA(NULL), NULL);
    if (!p->hwnd) {
        if (p->font_title) DeleteObject(p->font_title);
        if (p->font_id)    DeleteObject(p->font_id);
        if (p->font_label) DeleteObject(p->font_label);
        if (p->font_close) DeleteObject(p->font_close);
        HeapFree(GetProcessHeap(), 0, p);
        return NULL;
    }
    SetWindowLongPtrA(p->hwnd, GWLP_USERDATA, (LONG_PTR)p);
    return p;
}

void picker_destroy(NewspaperPicker *p)
{
    if (!p) return;
    if (p->cover_thread) {
        WaitForSingleObject(p->cover_thread, 1500);
        CloseHandle(p->cover_thread);
        p->cover_thread = NULL;
    }
    if (p->hwnd) DestroyWindow(p->hwnd);
    if (p->font_title) DeleteObject(p->font_title);
    if (p->font_id)    DeleteObject(p->font_id);
    if (p->font_label) DeleteObject(p->font_label);
    if (p->font_close) DeleteObject(p->font_close);
    HeapFree(GetProcessHeap(), 0, p);
}

HWND picker_hwnd(NewspaperPicker *p)
{ return p ? p->hwnd : NULL; }

void picker_show(NewspaperPicker *p)
{
    if (!p || !p->hwnd) return;
    SetWindowPos(p->hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(p->hwnd);
    SetFocus(p->hwnd);
    p_update_scrollbar(p);
    p_start_cover_worker(p);
    InvalidateRect(p->hwnd, NULL, FALSE);
}

void picker_hide(NewspaperPicker *p)
{
    if (!p || !p->hwnd) return;
    ShowWindow(p->hwnd, SW_HIDE);
    /* Fire the dismiss callback for every dismiss path -- close
     * button, Esc, AND cell-click (which the WM_LBUTTONDOWN handler
     * routes through picker_hide before invoking on_pick). The
     * newspaper module uses this hook to wv2_set_visible(TRUE)
     * before the consumer navigates the WebView2 to the new issue. */
    if (p->on_dismiss) p->on_dismiss(p->on_dismiss_user);
}

void picker_set_on_pick(NewspaperPicker *p, NsPickerOnPick cb, void *user)
{
    if (!p) return;
    p->on_pick      = cb;
    p->on_pick_user = user;
}

void picker_set_on_dismiss(NewspaperPicker *p, NsPickerOnDismiss cb, void *user)
{
    if (!p) return;
    p->on_dismiss      = cb;
    p->on_dismiss_user = user;
}

int picker_cell_count(NewspaperPicker *p)
{ return p_total_count(p); }
