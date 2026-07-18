/*
 * irc_module.c - F8 Retro IRC viewer. Receive-only broadcast view of the
 *                #retro channel on irc.greektimes.ca, rendered amber-on-
 *                black in the same Retro Terminal look as F6 Telnet.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * Architecture (client-side shared-service rule):
 *   irc_client.c owns the socket, IRCv3 CAP negotiation, registration,
 *   parsing, PING/PONG, and silent reconnect; it posts parsed
 *   IrcMessage events and knows nothing about GDI. This module
 *   subscribes, filters to #retro PRIVMSG/NOTICE, and feeds a private
 *   VTTerm that it paints with the F6 amber recipe (Cascadia Mono, amber
 *   #FFB000 on near-black, double-buffered). No input field, no cursor,
 *   no macro buttons -- a scrollable broadcast pane.
 *
 * Presentation rules (per dispatch):
 *   - One line per message: [HH:MM:SS] text, timestamp from the server's
 *     @time= tag converted to workstation-local (Montreal), else the
 *     local clock.
 *   - Only PRIVMSG / NOTICE targeting #retro are rendered. All numerics,
 *     MOTD, JOIN/PART/QUIT, CAP, and connection status are suppressed by
 *     irc_client before they ever reach us (clean broadcast, no notices).
 *   - Auto-scroll to bottom on new lines; pause auto-scroll while the
 *     user has scrolled up (vt_term_feed_bytes pins to bottom, so we
 *     capture the offset before feeding and re-apply it after).
 *   - A bounded seen-msgid set drops duplicates when the +H replay
 *     re-sends recent backlog after a reconnect.
 */

#include "irc_module.h"
#include "vt_term.h"
#include "irc_client.h"
#include "suite_clipboard.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- config defaults (no Suite-wide config store exists; every
 * module keeps its greektimes.ca endpoint as named #defines) ---------- */
#define IRC_DEFAULT_HOST         "irc.greektimes.ca"
#define IRC_DEFAULT_PORT         6667
#define IRC_DEFAULT_PORT_STR     "6667"     /* connect-bar field default */
#define IRC_DEFAULT_CHANNEL      "#retro"
#define IRC_DEFAULT_CHANNEL_NAME "retro"    /* bare name for the field (# in label) */
#define IRC_DEFAULT_NICK_PATTERN "mgtview-XXXX"

/* ---------- palette (matches F6 amber; kept as a local literal, same as
 * F3 Gopher keeps its own -- there is no shared colors header) ---------- */
#define IRC_COL_BG   RGB(0x0a, 0x0a, 0x0a)   /* near-black */
#define IRC_COL_FG   RGB(0xff, 0xb0, 0x00)   /* amber phosphor */

/* ---------- IDs ---------- */
#define IDC_IRC_RENDER         6201
#define IDC_IRC_SERVER_EDIT    6202
#define IDC_IRC_PORT_EDIT      6203
#define IDC_IRC_CHAN_EDIT      6204
#define IDC_IRC_CONNECT_BTN    6205
#define IDC_IRC_DISCONNECT_BTN 6206
#define IRC_RENDER_CLASS      "MGTIrcViewerV1"

#define WM_IRC_NOTIFY         (WM_USER + 41)
#define WM_USER_FOCUS_RENDER  (WM_USER + 100)

#define ID_TIMER_SEL_AUTOSCROLL  3
#define ID_CTX_COPY        2001
#define ID_CTX_SELECT_ALL  2003

#define IRC_COLS_MIN   80
#define IRC_ROWS_MIN   24
#define IRC_SCROLLBK   2000

/* ---------- seen-msgid dedup set (bounded FIFO ring) ----------
 * Linear-scan membership over a fixed ring; message rate is a handful
 * per minute plus a ~60-line backlog burst, so O(N) per message with
 * N<=4096 is negligible. When the ring fills, the oldest entry is
 * overwritten (FIFO eviction), so a long-running session cannot grow
 * the set without bound. */
#define IRC_SEEN_MAX   4096
static char g_seen[IRC_SEEN_MAX][64];
static int  g_seen_head  = 0;   /* next slot to write (== oldest when full) */
static int  g_seen_count = 0;

/* ---------- module state ---------- */
static HWND   g_hContent        = NULL;
static HWND   g_hRender         = NULL;
static HWND   g_hServerLbl,  g_hServerEdit;
static HWND   g_hPortLbl,    g_hPortEdit;
static HWND   g_hChanLbl,    g_hHashLbl,   g_hChanEdit;
static HWND   g_hConnectBtn, g_hDisconnectBtn;
static BOOL   g_controls_created = FALSE;
static BOOL   g_render_class_reg  = FALSE;
static HFONT  g_hFontIrc         = NULL;
static HBRUSH g_hBrushBg         = NULL;
static HBRUSH g_hBrushFg         = NULL;
static int    g_cell_w           = 9;
static int    g_cell_h           = 18;

static VTTerm g_term;
static BOOL   g_term_inited      = FALSE;

static IrcClient *g_client        = NULL;

/* UI session gate (mirrors the WAIS Connect/Disconnect bar). Pre-connect:
 * Server/Port/Channel + Connect reachable, Disconnect grayed. Connected/
 * active: those locked, Disconnect reachable. The manual Connect starts
 * irc_client; a user Disconnect fully closes it (no silent reconnect).
 * Unexpected mid-session drops are handled inside irc_client (silent
 * reconnect + msgid dedup) and do NOT change this UI state. */
static BOOL   g_connected        = FALSE;

/* Active channel (the "#name" form) and host committed by Connect;
 * on_irc_message filters against the channel, and the registration status
 * line ("Connected to <host> <channel>") uses both. */
static char   g_active_channel[64] = IRC_DEFAULT_CHANNEL;
static char   g_active_host[256]   = IRC_DEFAULT_HOST;

/* Mouse-selection state (same model as F6 polish 6). */
static BOOL g_sel_dragging     = FALSE;
static int  g_sel_anchor_row   = 0;   /* logical row */
static int  g_sel_anchor_col   = 0;
static int  g_sel_scroll_dir   = 0;   /* +1 = up into history, -1 = down */
static BOOL g_sel_scroll_armed = FALSE;

/* ---------- forward decls ---------- */
static LRESULT CALLBACK render_proc(HWND, UINT, WPARAM, LPARAM);
static void pixel_to_cell(int x_px, int y_px, int *out_vis_row, int *out_col);
static void arm_sel_scroll_timer(HWND hwnd, int dir);
static void kill_sel_scroll_timer(HWND hwnd);
static void irc_copy_selection(void);
static void irc_set_connected_state(BOOL connected);
static void irc_on_connect(HWND content);
static void irc_on_disconnect(HWND content);

/* ---------- seen-msgid helpers ---------- */

static int seen_contains(const char *id)
{
    int i;
    for (i = 0; i < g_seen_count; i++)
        if (strcmp(g_seen[i], id) == 0) return 1;
    return 0;
}

static void seen_add(const char *id)
{
    strncpy(g_seen[g_seen_head], id, sizeof(g_seen[0]) - 1);
    g_seen[g_seen_head][sizeof(g_seen[0]) - 1] = '\0';
    g_seen_head = (g_seen_head + 1) % IRC_SEEN_MAX;
    if (g_seen_count < IRC_SEEN_MAX) g_seen_count++;
}

/* ---------- grid helpers ---------- */

static void measure_cell(HWND hwnd)
{
    HDC        hdc;
    HFONT      old;
    TEXTMETRIC tm;
    if (!hwnd || !g_hFontIrc) return;
    hdc = GetDC(hwnd);
    old = (HFONT)SelectObject(hdc, g_hFontIrc);
    GetTextMetrics(hdc, &tm);
    g_cell_w = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 9;
    g_cell_h = tm.tmHeight        > 0 ? tm.tmHeight     : 18;
    SelectObject(hdc, old);
    ReleaseDC(hwnd, hdc);
}

static void compute_grid_for_client(HWND hwnd, int *out_cols, int *out_rows)
{
    RECT rc;
    int  cw, rh, cols, rows;
    GetClientRect(hwnd, &rc);
    cw = g_cell_w > 0 ? g_cell_w : 9;
    rh = g_cell_h > 0 ? g_cell_h : 18;
    cols = (rc.right  - rc.left) / cw;
    rows = (rc.bottom - rc.top ) / rh;
    /* Floor at 80x24 so our formatted "[HH:MM:SS] " + long weather/alert
     * lines have a stable minimum width; above the floor cols grow with
     * the window so a wide pane wraps less. */
    if (cols < IRC_COLS_MIN) cols = IRC_COLS_MIN;
    if (rows < IRC_ROWS_MIN) rows = IRC_ROWS_MIN;
    *out_cols = cols;
    *out_rows = rows;
}

/* Sync the render pane's real WS_VSCROLL scrollbar to the vt_term
 * scrollback. Position runs top(0)..scrollback_count, page = visible rows;
 * vt_term's offset is "lines back from bottom", so position = sb - offset
 * (offset 0 == live bottom == thumb at bottom). SIF_DISABLENOSCROLL keeps
 * the bar visible-but-disabled when there is nothing to scroll. */
static void update_vscrollbar(HWND hwnd)
{
    SCROLLINFO si;
    int sb, rows, offset;
    if (!hwnd || !g_term_inited) return;
    sb     = vt_term_scrollback_count(&g_term);
    rows   = g_term.rows;
    offset = vt_term_scroll_offset(&g_term);
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin   = 0;
    si.nMax   = sb + rows - 1;    /* total logical lines - 1 */
    si.nPage  = (UINT)(rows > 0 ? rows : 1);
    si.nPos   = sb - offset;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

/* Feed one already-formatted line, honoring the auto-scroll pause rule.
 * vt_term_feed_bytes pins scroll_offset to 0, so we snapshot the offset
 * first: if the user was scrolled up (offset>0) we re-apply offset+delta
 * (delta = rows newly pushed into scrollback) to keep their view fixed
 * on the same content; if they were at the bottom we stay pinned. */
static void feed_line(const char *s)
{
    int old_offset, before, after, delta;
    if (!g_term_inited || !s) return;
    old_offset = vt_term_scroll_offset(&g_term);
    before     = vt_term_scrollback_count(&g_term);
    vt_term_feed_bytes(&g_term, (const unsigned char *)s, strlen(s));
    if (old_offset > 0) {
        after = vt_term_scrollback_count(&g_term);
        delta = after - before;
        vt_term_scroll_by(&g_term, old_offset + delta);  /* clamps internally */
    }
    update_vscrollbar(g_hRender);   /* reflect new scrollback / position */
}

/* ---------- incoming message ---------- */

static void on_irc_message(IrcMessage *m)
{
    char line[1200];
    if (!m) return;

    /* Filter: only the active channel, PRIVMSG/NOTICE (kind already
     * guaranteed by irc_client). CASEMAPPING=ascii, so _stricmp is
     * correct. g_active_channel is the "#name" committed by Connect. */
    if (_stricmp(m->target, g_active_channel) != 0) {
        irc_client_free_message(m);
        return;
    }

    /* Dedup: drop backlog the +H replay re-sends after a reconnect. */
    if (m->msgid[0]) {
        if (seen_contains(m->msgid)) {
            irc_client_free_message(m);
            return;
        }
        seen_add(m->msgid);
    }

    _snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s\r\n",
              m->when_local.wHour, m->when_local.wMinute,
              m->when_local.wSecond, m->text);
    line[sizeof(line) - 1] = '\0';

    feed_line(line);
    if (g_hRender) InvalidateRect(g_hRender, NULL, FALSE);

    irc_client_free_message(m);
}

/* Server banner line (Connecting/Connected/CAP/numerics/MOTD). Rendered
 * un-timestamped with HexChat's "* " server-notice prefix. Only arrives on
 * a manual connect's initial session (the transport gates the banner). */
static void on_irc_system(IrcMessage *m)
{
    char line[1200];
    if (!m) return;
    _snprintf(line, sizeof(line), "* %s\r\n", m->text);
    line[sizeof(line) - 1] = '\0';
    feed_line(line);
    if (g_hRender) InvalidateRect(g_hRender, NULL, FALSE);
    irc_client_free_message(m);
}

/* Terminal pre-registration failure on a manual connect. Render the reason
 * HexChat-style, set the status bar (not a frozen "Connecting..."), release
 * the client handle (idempotent), and return to the disconnected UI state. */
static void on_irc_failed(IrcMessage *m)
{
    const char *reason = (m && m->text[0]) ? m->text : "Could not connect";
    char        line[1200];

    _snprintf(line, sizeof(line), "* Connection failed: %s\r\n", reason);
    line[sizeof(line) - 1] = '\0';
    feed_line(line);
    if (g_hRender) InvalidateRect(g_hRender, NULL, FALSE);

    {
        char st[320];
        _snprintf(st, sizeof(st), "Connection failed: %s", reason);
        st[sizeof(st) - 1] = '\0';
        suite_set_status(st);
    }

    if (g_client) { irc_client_close(g_client); g_client = NULL; }
    irc_set_connected_state(FALSE);

    if (m) irc_client_free_message(m);
}

/* Registration reached (numeric 001). Replace the "Connecting..." status
 * with the real connected state. Guarded on g_connected so a stray event
 * arriving just after a user Disconnect cannot resurrect a stale status.
 * LPARAM is unused for this event -- nothing to free. */
static void on_irc_registered(void)
{
    char st[400];
    if (!g_connected) return;
    _snprintf(st, sizeof(st), "Connected to %s %s",
              g_active_host, g_active_channel);
    st[sizeof(st) - 1] = '\0';
    suite_set_status(st);
}

/* ---------- selection helpers ---------- */

static void pixel_to_cell(int x_px, int y_px, int *out_vis_row, int *out_col)
{
    int cw = g_cell_w > 0 ? g_cell_w : 9;
    int ch = g_cell_h > 0 ? g_cell_h : 18;
    int v  = y_px / ch;
    int c  = x_px / cw;
    int rows_max = g_term_inited ? g_term.rows : 1;
    int cols_max = g_term_inited ? g_term.cols : 1;
    if (v < 0)         v = 0;
    if (v >= rows_max) v = rows_max - 1;
    if (c < 0)         c = 0;
    if (c >= cols_max) c = cols_max - 1;
    *out_vis_row = v;
    *out_col     = c;
}

static void arm_sel_scroll_timer(HWND hwnd, int dir)
{
    if (!hwnd) return;
    g_sel_scroll_dir = dir;
    if (!g_sel_scroll_armed) {
        SetTimer(hwnd, ID_TIMER_SEL_AUTOSCROLL, 80, NULL);
        g_sel_scroll_armed = TRUE;
    }
}

static void kill_sel_scroll_timer(HWND hwnd)
{
    if (g_sel_scroll_armed && hwnd) {
        KillTimer(hwnd, ID_TIMER_SEL_AUTOSCROLL);
        g_sel_scroll_armed = FALSE;
    }
    g_sel_scroll_dir = 0;
}

static void irc_copy_selection(void)
{
    char *text;
    if (!g_term_inited || !g_term.sel_active) return;
    text = vt_term_select_extract(&g_term);
    if (!text) return;
    suite_clipboard_put_text(g_hRender, text);
    free(text);
}

/* ---------- paint ---------- */

/* Double-buffered amber-on-black render, ported from F6 paint_terminal
 * minus the cursor block (a receive-only viewer has no input cursor).
 * Selected runs paint an amber block then re-draw the glyph in BG color
 * with TRANSPARENT mode, so glyph advances cannot bleed past a cell. */
static void paint_view(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC         hdc;
    HDC         mem_dc   = NULL;
    HBITMAP     mem_bmp  = NULL;
    HBITMAP     old_bmp  = NULL;
    HFONT       old_font = NULL;
    RECT        rc;
    int         row, col;
    int         py;
    int         client_w, client_h;

    hdc = BeginPaint(hwnd, &ps);
    GetClientRect(hwnd, &rc);
    client_w = rc.right  - rc.left;
    client_h = rc.bottom - rc.top;
    if (client_w <= 0 || client_h <= 0) { EndPaint(hwnd, &ps); return; }

    mem_dc  = CreateCompatibleDC(hdc);
    mem_bmp = CreateCompatibleBitmap(hdc, client_w, client_h);
    if (!mem_dc || !mem_bmp) {
        if (mem_bmp) DeleteObject(mem_bmp);
        if (mem_dc)  DeleteDC(mem_dc);
        FillRect(hdc, &ps.rcPaint, g_hBrushBg);
        EndPaint(hwnd, &ps);
        return;
    }
    old_bmp = (HBITMAP)SelectObject(mem_dc, mem_bmp);

    FillRect(mem_dc, &rc, g_hBrushBg);

    if (g_term_inited) {
        old_font = (HFONT)SelectObject(mem_dc, g_hFontIrc);
        SetTextColor(mem_dc, IRC_COL_FG);
        SetBkColor  (mem_dc, IRC_COL_BG);
        SetBkMode   (mem_dc, OPAQUE);

        for (row = 0; row < g_term.rows; row++) {
            const VTCell *cells = vt_term_get_visible(&g_term, row);
            char buf[512];
            int  logical_row, run_start;
            if (!cells) continue;
            if (g_term.cols >= (int)sizeof(buf)) continue;
            for (col = 0; col < g_term.cols; col++) {
                unsigned char ch = (unsigned char)cells[col].ch;
                if (ch < 0x20 || ch >= 0x7F) ch = ' ';
                buf[col] = (char)ch;
            }
            py = row * g_cell_h;
            logical_row = vt_term_visible_to_logical(&g_term, row);
            run_start = 0;
            while (run_start < g_term.cols) {
                int run_sel = vt_term_cell_selected(&g_term, logical_row, run_start);
                int run_end = run_start + 1;
                int run_len, x_px;
                while (run_end < g_term.cols &&
                       vt_term_cell_selected(&g_term, logical_row, run_end) == run_sel) {
                    run_end++;
                }
                run_len = run_end - run_start;
                x_px = run_start * g_cell_w;
                if (run_sel) {
                    RECT rs;
                    rs.left   = x_px;
                    rs.top    = py;
                    rs.right  = x_px + run_len * g_cell_w;
                    rs.bottom = py + g_cell_h;
                    if (g_hBrushFg) FillRect(mem_dc, &rs, g_hBrushFg);
                    SetTextColor(mem_dc, IRC_COL_BG);
                    SetBkMode   (mem_dc, TRANSPARENT);
                } else {
                    SetTextColor(mem_dc, IRC_COL_FG);
                    SetBkColor  (mem_dc, IRC_COL_BG);
                    SetBkMode   (mem_dc, OPAQUE);
                }
                TextOutA(mem_dc, x_px, py, buf + run_start, run_len);
                run_start = run_end;
            }
        }

        if (old_font) SelectObject(mem_dc, old_font);
    }

    BitBlt(hdc,
           ps.rcPaint.left, ps.rcPaint.top,
           ps.rcPaint.right  - ps.rcPaint.left,
           ps.rcPaint.bottom - ps.rcPaint.top,
           mem_dc,
           ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);

    SelectObject(mem_dc, old_bmp);
    DeleteObject(mem_bmp);
    DeleteDC(mem_dc);
    EndPaint(hwnd, &ps);
}

/* ---------- render window proc ---------- */

static LRESULT CALLBACK render_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        measure_cell(hwnd);
        if (!g_term_inited) {
            vt_term_init(&g_term, IRC_COLS_MIN, IRC_ROWS_MIN, IRC_SCROLLBK);
            g_term_inited = TRUE;
        }
        update_vscrollbar(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    /* Custom-painted window owns its own keyboard: claim all keys so the
     * suite's IsDialogMessage loop cannot swallow arrows / letters. */
    case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTALLKEYS;

    case WM_PAINT:
        paint_view(hwnd);
        return 0;

    case WM_SIZE: {
        int cols, rows;
        compute_grid_for_client(hwnd, &cols, &rows);
        if (g_term_inited && (cols != g_term.cols || rows != g_term.rows))
            vt_term_resize(&g_term, cols, rows);
        update_vscrollbar(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (wParam == ID_TIMER_SEL_AUTOSCROLL) {
            POINT pt;
            int vis, col, logical_row;
            if (!g_sel_dragging) { kill_sel_scroll_timer(hwnd); return 0; }
            if (g_sel_scroll_dir > 0)      vt_term_scroll_by(&g_term, +1);
            else if (g_sel_scroll_dir < 0) vt_term_scroll_by(&g_term, -1);
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            pixel_to_cell(pt.x, pt.y, &vis, &col);
            logical_row = vt_term_visible_to_logical(&g_term, vis);
            vt_term_select_set(&g_term, g_sel_anchor_row, g_sel_anchor_col,
                                        logical_row, col);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        int vis, col, logical_row;
        SetFocus(hwnd);
        if (!g_term_inited) return 0;
        pixel_to_cell(x, y, &vis, &col);
        logical_row = vt_term_visible_to_logical(&g_term, vis);
        g_sel_anchor_row = logical_row;
        g_sel_anchor_col = col;
        g_sel_dragging   = TRUE;
        SetCapture(hwnd);
        vt_term_select_set(&g_term, logical_row, col, logical_row, col);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x, y, vis, col, logical_row;
        RECT rc;
        if (!g_sel_dragging) return 0;
        x = GET_X_LPARAM(lParam);
        y = GET_Y_LPARAM(lParam);
        GetClientRect(hwnd, &rc);
        if (y < 0)               arm_sel_scroll_timer(hwnd, +1);
        else if (y >= rc.bottom) arm_sel_scroll_timer(hwnd, -1);
        else                     kill_sel_scroll_timer(hwnd);
        pixel_to_cell(x, y, &vis, &col);
        logical_row = vt_term_visible_to_logical(&g_term, vis);
        vt_term_select_set(&g_term, g_sel_anchor_row, g_sel_anchor_col,
                                    logical_row, col);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!g_sel_dragging) return 0;
        ReleaseCapture();
        g_sel_dragging = FALSE;
        kill_sel_scroll_timer(hwnd);
        if (g_term.sel_anchor_row == g_term.sel_lead_row &&
            g_term.sel_anchor_col == g_term.sel_lead_col) {
            vt_term_select_clear(&g_term);   /* bare click clears selection */
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_RBUTTONDOWN: {
        HMENU hm = CreatePopupMenu();
        UINT  fc = (g_term_inited && g_term.sel_active) ? MF_ENABLED : MF_GRAYED;
        POINT pt;
        if (!hm) return 0;
        AppendMenuA(hm, MF_STRING | fc, ID_CTX_COPY,        "Copy\tCtrl+C");
        AppendMenuA(hm, MF_SEPARATOR,    0,                  NULL);
        AppendMenuA(hm, MF_STRING,       ID_CTX_SELECT_ALL, "Select All\tCtrl+A");
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        ClientToScreen(hwnd, &pt);
        SetFocus(hwnd);
        TrackPopupMenu(hm, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(hm);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_CTX_COPY) {
            irc_copy_selection();
            return 0;
        }
        if (id == ID_CTX_SELECT_ALL) {
            vt_term_select_all(&g_term);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    }

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_KEYDOWN: {
        int  vk   = (int)wParam;
        BOOL ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && vk == 'A') {
            vt_term_select_all(&g_term);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (ctrl && vk == 'C') {
            if (g_term_inited && g_term.sel_active) {
                irc_copy_selection();
                vt_term_select_clear(&g_term);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        switch (vk) {
        case VK_UP:    vt_term_scroll_by(&g_term,  1);          /* one line up */
                       update_vscrollbar(hwnd);
                       InvalidateRect(hwnd, NULL, FALSE); return 0;
        case VK_DOWN:  vt_term_scroll_by(&g_term, -1);          /* one line down */
                       update_vscrollbar(hwnd);
                       InvalidateRect(hwnd, NULL, FALSE); return 0;
        case VK_PRIOR: vt_term_scroll_by(&g_term,  g_term.rows);
                       update_vscrollbar(hwnd);
                       InvalidateRect(hwnd, NULL, FALSE); return 0;
        case VK_NEXT:  vt_term_scroll_by(&g_term, -g_term.rows);
                       update_vscrollbar(hwnd);
                       InvalidateRect(hwnd, NULL, FALSE); return 0;
        case VK_HOME:  vt_term_scroll_by(&g_term,  vt_term_scrollback_count(&g_term));
                       update_vscrollbar(hwnd);
                       InvalidateRect(hwnd, NULL, FALSE); return 0;
        case VK_END:   vt_term_scroll_to_bottom(&g_term);
                       update_vscrollbar(hwnd);
                       InvalidateRect(hwnd, NULL, FALSE); return 0;
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int notches = delta / WHEEL_DELTA;
        if (notches == 0) notches = delta > 0 ? 1 : -1;
        vt_term_scroll_by(&g_term, notches * 3);
        update_vscrollbar(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_VSCROLL: {
        int sb     = g_term_inited ? vt_term_scrollback_count(&g_term) : 0;
        int rows   = g_term_inited ? g_term.rows : 1;
        int offset = g_term_inited ? vt_term_scroll_offset(&g_term) : 0;
        int pos    = sb - offset;    /* current top-line position */
        switch (LOWORD(wParam)) {
        case SB_LINEUP:    pos -= 1;    break;
        case SB_LINEDOWN:  pos += 1;    break;
        case SB_PAGEUP:    pos -= rows; break;
        case SB_PAGEDOWN:  pos += rows; break;
        case SB_TOP:       pos = 0;     break;
        case SB_BOTTOM:    pos = sb;    break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si;
            ZeroMemory(&si, sizeof(si));
            si.cbSize = sizeof(si);
            si.fMask  = SIF_TRACKPOS;
            GetScrollInfo(hwnd, SB_VERT, &si);
            pos = si.nTrackPos;
            break;
        }
        default: return 0;
        }
        if (pos < 0)  pos = 0;
        if (pos > sb) pos = sb;
        vt_term_scroll_by(&g_term, (sb - pos) - offset);   /* -> target offset */
        update_vscrollbar(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_IRC_NOTIFY:
        if (wParam == IRC_EVT_MESSAGE)         on_irc_message((IrcMessage *)lParam);
        else if (wParam == IRC_EVT_SYSTEM)     on_irc_system((IrcMessage *)lParam);
        else if (wParam == IRC_EVT_FAILED)     on_irc_failed((IrcMessage *)lParam);
        else if (wParam == IRC_EVT_REGISTERED) on_irc_registered();
        return 0;

    case WM_USER_FOCUS_RENDER:
        SetFocus(hwnd);
        return 0;

    case WM_DESTROY:
        kill_sel_scroll_timer(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ---------- connect-bar session gate (mirrors WAIS) ---------- */

/* Pre-connect: Server/Port/Channel + Connect enabled, Disconnect grayed.
 * Connected/active: those grayed, Disconnect enabled. Same field/button
 * lifecycle and timing as wais_set_connected_state. */
static void irc_set_connected_state(BOOL connected)
{
    g_connected = connected;

    EnableWindow(g_hServerEdit,     !connected);
    EnableWindow(g_hPortEdit,       !connected);
    EnableWindow(g_hChanEdit,       !connected);
    EnableWindow(g_hConnectBtn,     !connected);

    EnableWindow(g_hDisconnectBtn,   connected);
}

/* Connect: read host/port/channel from the fields, normalize the channel
 * to a single leading '#', start irc_client with those values, and flip
 * to the connected state. A fresh manual Connect clears the terminal and
 * resets the seen-msgid set so the session starts clean. */
static void irc_on_connect(HWND content)
{
    char        host[256], ports[16], chan[64], chanhash[64];
    int         port_int;
    const char *c;

    (void)content;
    if (g_connected) return;

    GetWindowTextA(g_hServerEdit, host,  sizeof(host));
    GetWindowTextA(g_hPortEdit,   ports, sizeof(ports));
    GetWindowTextA(g_hChanEdit,   chan,  sizeof(chan));

    if (!host[0] || !ports[0] || !chan[0]) {
        suite_set_status("Server, port, and channel are required.");
        return;
    }
    port_int = atoi(ports);
    if (port_int <= 0 || port_int > 65535) {
        suite_set_status("Port must be 1-65535.");
        return;
    }

    /* Strip any leading '#' the user typed, then prepend exactly one, so
     * "retro" and "#retro" both yield "#retro". */
    c = chan;
    while (*c == '#') c++;
    _snprintf(chanhash, sizeof(chanhash), "#%s", c);
    chanhash[sizeof(chanhash) - 1] = '\0';
    strncpy(g_active_channel, chanhash, sizeof(g_active_channel) - 1);
    g_active_channel[sizeof(g_active_channel) - 1] = '\0';
    strncpy(g_active_host, host, sizeof(g_active_host) - 1);
    g_active_host[sizeof(g_active_host) - 1] = '\0';

    /* Fresh manual session: wipe the pane and the dedup set. */
    if (g_term_inited) vt_term_clear_all(&g_term);
    g_seen_head  = 0;
    g_seen_count = 0;
    if (g_hRender) InvalidateRect(g_hRender, NULL, FALSE);

    /* show_banner = 1: this is a manual connect, so the transport forwards
     * the HexChat-style registration banner (Connecting/Connected/CAP/
     * numerics/MOTD) as IRC_EVT_SYSTEM lines. Automatic mid-session
     * reconnects suppress it. */
    if (irc_client_open(host, port_int, g_active_channel,
                        IRC_DEFAULT_NICK_PATTERN, 1,
                        g_hRender, WM_IRC_NOTIFY, &g_client) != 0 || !g_client) {
        suite_set_status("Failed to start IRC connection.");
        return;
    }

    irc_set_connected_state(TRUE);
    {
        char st[320];
        _snprintf(st, sizeof(st), "Connecting to %s:%d %s...",
                  host, port_int, g_active_channel);
        st[sizeof(st) - 1] = '\0';
        suite_set_status(st);
    }
    if (g_hRender) SetFocus(g_hRender);
}

/* Disconnect: fully stop the transport (irc_client_close halts the
 * background reconnect loop, so a user Disconnect does NOT silently
 * reconnect) and return to the disconnected state with the fields
 * editable. Already-rendered lines stay in the pane. */
static void irc_on_disconnect(HWND content)
{
    (void)content;
    if (g_client) { irc_client_close(g_client); g_client = NULL; }
    irc_set_connected_state(FALSE);
    suite_set_status("Disconnected.");
    SetFocus(g_hServerEdit);
}

/* ---------- module entry points ---------- */

static void register_render_class(HINSTANCE hInst)
{
    WNDCLASSEXA wc;
    if (g_render_class_reg) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = render_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_IBEAM);
    wc.hbrBackground = NULL;     /* we erase in WM_PAINT */
    wc.lpszClassName = IRC_RENDER_CLASS;
    RegisterClassExA(&wc);
    g_render_class_reg = TRUE;
}

void irc_module_init(void)
{
    /* Real allocation happens at first activate. */
}

void irc_module_shutdown(void)
{
    if (g_client)      { irc_client_close(g_client); g_client = NULL; }
    if (g_term_inited) { vt_term_free(&g_term); g_term_inited = FALSE; }
    if (g_hFontIrc)    { DeleteObject(g_hFontIrc);  g_hFontIrc  = NULL; }
    if (g_hBrushBg)    { DeleteObject(g_hBrushBg);  g_hBrushBg  = NULL; }
    if (g_hBrushFg)    { DeleteObject(g_hBrushFg);  g_hBrushFg  = NULL; }
}

void irc_module_resize(HWND content, int w, int h)
{
    const int margin = 16;
    const int row_h  = 26;
    const int btn_h  = 30;
    int render_y;

    (void)content;
    if (!g_controls_created) return;

    /* Connect bar (row 1): Server / Port / Channel with clear spacing
     * (~34px between groups so Port no longer sits against Channel).
     * Connect + Disconnect anchor right. The "#" is a cosmetic static
     * butted against the Channel edit's left edge (SS_RIGHT), so it reads
     * "Channel:  #[retro]" while the edit still holds the bare "retro". */
    MoveWindow(g_hServerLbl,  margin,       18, 55,  22,    TRUE);
    MoveWindow(g_hServerEdit, margin + 58,  14, 220, row_h, TRUE);
    MoveWindow(g_hPortLbl,    margin + 312, 18, 35,  22,    TRUE);
    MoveWindow(g_hPortEdit,   margin + 350, 14, 55,  row_h, TRUE);
    MoveWindow(g_hChanLbl,    margin + 439, 18, 58,  22,    TRUE);
    MoveWindow(g_hHashLbl,    margin + 505, 14, 12,  row_h, TRUE);
    MoveWindow(g_hChanEdit,   margin + 517, 14, 130, row_h, TRUE);
    {
        const int conn_w   = 85;
        const int disc_w   = 95;
        const int conn_gap = 8;
        int disc_x = w - margin - disc_w;
        int conn_x = disc_x - conn_gap - conn_w;
        MoveWindow(g_hConnectBtn,    conn_x, 12, conn_w, btn_h, TRUE);
        MoveWindow(g_hDisconnectBtn, disc_x, 12, disc_w, btn_h, TRUE);
    }

    /* Terminal fills the rest, below the bar (same reserve-top-strip
     * pattern as the WAIS output pane), shortened by the bar height so
     * nothing overlaps. */
    render_y = 52;
    MoveWindow(g_hRender, margin, render_y,
               w - 2 * margin, h - render_y - margin, TRUE);
}

void irc_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    g_hContent = content;

    /* Re-activation: show all controls back and preserve session state
     * (connected/disconnected, rendered lines, field values). Focus the
     * render pane if connected, else the Server field -- same as WAIS. */
    if (g_controls_created) {
        ShowWindow(g_hServerLbl,     SW_SHOW);
        ShowWindow(g_hServerEdit,    SW_SHOW);
        ShowWindow(g_hPortLbl,       SW_SHOW);
        ShowWindow(g_hPortEdit,      SW_SHOW);
        ShowWindow(g_hChanLbl,       SW_SHOW);
        ShowWindow(g_hHashLbl,       SW_SHOW);
        ShowWindow(g_hChanEdit,      SW_SHOW);
        ShowWindow(g_hConnectBtn,    SW_SHOW);
        ShowWindow(g_hDisconnectBtn, SW_SHOW);
        ShowWindow(g_hRender,        SW_SHOW);
        GetClientRect(content, &rc);
        irc_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);
        if (g_connected) PostMessageA(g_hRender, WM_USER_FOCUS_RENDER, 0, 0);
        else             SetFocus(g_hServerEdit);
        return;
    }

    if (!g_hFontIrc) {
        /* Same face chain as F6 Telnet: Cascadia Mono -> Consolas ->
         * generic FIXED_PITCH | FF_MODERN, 12 pt at the display DPI. */
        HDC dc = GetDC(NULL);
        int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
        int height_12pt;
        if (dc) ReleaseDC(NULL, dc);
        if (dpi <= 0) dpi = 96;
        height_12pt = -MulDiv(12, dpi, 72);
        g_hFontIrc = CreateFontA(height_12pt, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Cascadia Mono");
        if (!g_hFontIrc)
            g_hFontIrc = CreateFontA(height_12pt, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Consolas");
        if (!g_hFontIrc)
            g_hFontIrc = CreateFontA(height_12pt, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH | FF_MODERN, NULL);
    }
    if (!g_hBrushBg) g_hBrushBg = CreateSolidBrush(IRC_COL_BG);
    if (!g_hBrushFg) g_hBrushFg = CreateSolidBrush(IRC_COL_FG);

    register_render_class(hInst);

    /* Connect bar: STATIC labels + WS_EX_CLIENTEDGE edits + push buttons,
     * styled exactly like the WAIS Server/Port/Database bar. The # lives
     * in the "Channel #:" label; the edit holds the bare name. */
    g_hServerLbl = CreateWindowA("STATIC", "Server:",
        WS_CHILD | WS_VISIBLE, 0, 0, 55, 22, content, NULL, hInst, NULL);
    g_hServerEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", IRC_DEFAULT_HOST,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 240, 26, content, (HMENU)(INT_PTR)IDC_IRC_SERVER_EDIT, hInst, NULL);
    g_hPortLbl = CreateWindowA("STATIC", "Port:",
        WS_CHILD | WS_VISIBLE, 0, 0, 40, 22, content, NULL, hInst, NULL);
    g_hPortEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", IRC_DEFAULT_PORT_STR,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        0, 0, 50, 26, content, (HMENU)(INT_PTR)IDC_IRC_PORT_EDIT, hInst, NULL);
    g_hChanLbl = CreateWindowA("STATIC", "Channel:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 58, 22,
        content, NULL, hInst, NULL);
    /* Cosmetic "#" prefix butted against the edit's left edge: SS_RIGHT so
     * the glyph hugs the box, SS_CENTERIMAGE to vertically center it with
     * the edit text. The edit still holds the bare channel name; the
     * connect logic prepends the real '#'. */
    g_hHashLbl = CreateWindowA("STATIC", "#",
        WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE, 0, 0, 12, 26,
        content, NULL, hInst, NULL);
    g_hChanEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", IRC_DEFAULT_CHANNEL_NAME,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 130, 26, content, (HMENU)(INT_PTR)IDC_IRC_CHAN_EDIT, hInst, NULL);
    g_hConnectBtn = CreateWindowA("BUTTON", "Connect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 85, 30, content, (HMENU)(INT_PTR)IDC_IRC_CONNECT_BTN, hInst, NULL);
    g_hDisconnectBtn = CreateWindowA("BUTTON", "Disconnect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 95, 30, content, (HMENU)(INT_PTR)IDC_IRC_DISCONNECT_BTN, hInst, NULL);

    if (g_hFontUI) {
        SendMessageA(g_hServerLbl,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hServerEdit,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hPortLbl,       WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hPortEdit,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hChanLbl,       WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hHashLbl,       WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hChanEdit,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hConnectBtn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hDisconnectBtn, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }

    g_hRender = CreateWindowExA(WS_EX_CLIENTEDGE, IRC_RENDER_CLASS, "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL,
        0, 0, 100, 100, content,
        (HMENU)(INT_PTR)IDC_IRC_RENDER, hInst, NULL);

    g_controls_created = TRUE;

    GetClientRect(content, &rc);
    irc_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);

    /* Manual connection model (matches WAIS): nothing connects on
     * activation. Start pre-connect -- Server/Port/Channel + Connect
     * reachable, Disconnect grayed -- and wait for the user to press
     * Connect. Focus the Server field like WAIS does pre-connect. */
    irc_set_connected_state(FALSE);
    SetFocus(g_hServerEdit);
}

void irc_module_deactivate(HWND content)
{
    (void)content;
    if (!g_controls_created) return;
    /* Non-destructive: hide controls, preserve session + rendered lines
     * across module switches (module-state-preserved discipline). An
     * active irc_client keeps running in the background. */
    ShowWindow(g_hServerLbl,     SW_HIDE);
    ShowWindow(g_hServerEdit,    SW_HIDE);
    ShowWindow(g_hPortLbl,       SW_HIDE);
    ShowWindow(g_hPortEdit,      SW_HIDE);
    ShowWindow(g_hChanLbl,       SW_HIDE);
    ShowWindow(g_hHashLbl,       SW_HIDE);
    ShowWindow(g_hChanEdit,      SW_HIDE);
    ShowWindow(g_hConnectBtn,    SW_HIDE);
    ShowWindow(g_hDisconnectBtn, SW_HIDE);
    ShowWindow(g_hRender,        SW_HIDE);
}

BOOL irc_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id    = LOWORD(wParam);
    int notif = HIWORD(wParam);
    (void)lParam;

    switch (id) {
    case IDC_IRC_CONNECT_BTN:
        if (notif == BN_CLICKED) { irc_on_connect(content);    return TRUE; }
        break;
    case IDC_IRC_DISCONNECT_BTN:
        if (notif == BN_CLICKED) { irc_on_disconnect(content); return TRUE; }
        break;
    }
    return FALSE;   /* the render child handles its own context-menu WM_COMMAND */
}

BOOL irc_module_has_unsaved(void)
{
    return FALSE;
}
