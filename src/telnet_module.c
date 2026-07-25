/*
 * telnet_module.c - F6 Telnet client. VT220 emulation in a CRT-style
 *                    render area, with a local shell prompt and four
 *                    macro buttons for the Montreal Greek Times services.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Layout (top to bottom):
 *   y=14:  macro-button row (Telnet, Finger News, Finger Weather, QOTD)
 *   y=50:  status line
 *   y=88:  custom-painted terminal render area
 *
 * Mode state machine:
 *   F6_SHELL -> commands typed at "$ " prompt; macro buttons enabled
 *   F6_TELNET_CONNECTING / F6_TELNET_ACTIVE -> Telnet session
 *   F6_FINGER_READING / F6_QOTD_READING -> read-only sessions
 *
 * Macro buttons are owner-draw and follow the same gray-with-black-text
 * blueprint deviation as the top-row module switcher.
 *
 * Mouse wheel routing: during F6_TELNET_ACTIVE the wheel is forwarded to
 * the server as VT arrow sequences (3 ESC[A or ESC[B per notch), so the
 * Telnet News article view scrolls server-side. In every other mode the
 * wheel scrolls the local scrollback buffer.
 */

#include "telnet_module.h"
#include "vt_term.h"
#include "telnet_proto.h"
#include "finger_proto.h"
#include "qotd_proto.h"
#include "suite_clipboard.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- locked palette ---------- */
/* Dispatch amendment 2026-06-15-3 (item 2): foreground switched from
 * phosphor green RGB(0x33, 0xff, 0x33) to amber #FFB000, matching the
 * classic Wyse 50 / Burroughs B25 amber CRT look. F6 only -- F3
 * Gopher keeps its existing color choices. The cursor block and
 * selection highlight both pick up the new color automatically since
 * they read TELNET_COL_FG / g_hBrushTermFg. */
#define TELNET_COL_BG       RGB(0x0a, 0x0a, 0x0a)   /* near-black */
#define TELNET_COL_FG       RGB(0xff, 0xb0, 0x00)   /* amber phosphor */

/* ---------- IDs ---------- */
#define IDC_TELNET_BTN_TELNET    6101
#define IDC_TELNET_BTN_FINGER_N  6102
#define IDC_TELNET_BTN_FINGER_W  6103
#define IDC_TELNET_BTN_QOTD      6104
#define IDC_TELNET_STATUS        6105
#define IDC_TELNET_RENDER        6106
#define IDC_TELNET_BTN_ARCHIE    6107

#define TELNET_RENDER_CLASS  "MGTTelnetTerminalV1"

#define WM_TELNET_NOTIFY        (WM_USER + 31)
/* Posted from telnet_module_activate so the focus lands AFTER the
 * suite's switch_to_module finishes (which would otherwise clobber
 * any synchronous SetFocus we did during activate). */
#define WM_USER_FOCUS_RENDER    (WM_USER + 100)

#define ID_TIMER_CURSOR_BLINK     1
#define ID_TIMER_MACRO_AUTOENTER  2
#define ID_TIMER_SEL_AUTOSCROLL   3

/* Polish 6 (2026-05-22) context-menu IDs. */
#define ID_CTX_COPY        2001
#define ID_CTX_PASTE       2002
#define ID_CTX_SELECT_ALL  2003

#define TELNET_COLS_MIN  80
#define TELNET_ROWS_MIN  24
#define TELNET_SCROLLBK  1000

/* ---------- mode ---------- */

typedef enum {
    F6_SHELL = 0,
    F6_TELNET_CONNECTING,
    F6_TELNET_ACTIVE,
    F6_FINGER_READING,
    F6_QOTD_READING
} F6Mode;

/* ---------- module state ---------- */

static HWND   g_hContent       = NULL;
static HWND   g_hBtnTelnet     = NULL;
static HWND   g_hBtnFingerNews = NULL;
static HWND   g_hBtnFingerWx   = NULL;
static HWND   g_hBtnQotd       = NULL;
static HWND   g_hBtnArchie     = NULL;
static HWND   g_hStatus        = NULL;
static HWND   g_hRender        = NULL;

static BOOL   g_controls_created = FALSE;
static BOOL   g_render_class_reg = FALSE;
static HFONT  g_hFontTelnet      = NULL;
static HBRUSH g_hBrushTermBg     = NULL;
static HBRUSH g_hBrushTermFg     = NULL;
static int    g_cell_w           = 9;
static int    g_cell_h           = 18;

static VTTerm g_term;
static BOOL   g_term_inited      = FALSE;

static F6Mode g_mode             = F6_SHELL;
static BOOL   g_cursor_visible   = TRUE;

/* Active session pointers; exactly one is non-NULL at a time. */
static TelnetProto *g_active_telnet = NULL;
static FingerProto *g_active_finger = NULL;
static QotdProto   *g_active_qotd   = NULL;

/* Current connection address for the status line. */
static char   g_active_host[256];
static int    g_active_port = 0;

/* Shell input line. */
static char   g_shell_input[1024];
static int    g_shell_input_len = 0;
static int    g_shell_input_pos = 0;   /* insertion cursor, 0..len (in-line edit) */

/* Macro auto-Enter pending flag. */
static BOOL   g_macro_pending = FALSE;

/* Boot banner emit guard. */
static BOOL   g_banner_emitted = FALSE;

/* Last status text without the dimensions suffix, so we can re-render
 * the status line whenever the terminal resizes (WM_SIZE). */
static char   g_status_base[320] = "Local shell";

/* Polish 4 (2026-05-22) diagnostic: ring buffer of the last few
 * keyboard events seen by render_proc. Each slot holds a short token
 * like "vk=41", "ch=A", "vk^41" (^ for KEYUP). Surfaced as a KEY:
 * segment on the status line so we can see whether IsDialogMessage in
 * the suite's main loop is still swallowing keystrokes. */
#define KEY_LOG_DEPTH 4
static char   g_key_log[KEY_LOG_DEPTH][16];
static int    g_key_log_count = 0;

/* Polish 5 (2026-05-22): when WM_KEYDOWN handles a shell-mode letter
 * or digit by deriving it from the VK code (layout-independent), the
 * subsequent WM_CHAR for the same physical key would otherwise re-feed
 * the layout-translated codepage byte (e.g. Greek tau 0xF4 for 'T' on
 * a Greek keyboard) and either get dropped by the ASCII filter or land
 * as garbage. The flag tells WM_CHAR to swallow exactly one upcoming
 * char. Re-armed before every WM_CHAR delivery. */
static BOOL   g_swallow_next_char = FALSE;

/* Polish 6 (2026-05-22) mouse-selection state. */
static BOOL   g_sel_dragging       = FALSE;
static int    g_sel_anchor_row     = 0;   /* logical row */
static int    g_sel_anchor_col     = 0;
static int    g_sel_scroll_dir     = 0;   /* +1 = up into history, -1 = down */
static BOOL   g_sel_scroll_armed   = FALSE;

/* ---------- forward decls ---------- */

static LRESULT CALLBACK render_proc(HWND, UINT, WPARAM, LPARAM);
static void shell_dispatch(const char *cmd);
static void enter_shell_mode(BOOL write_prompt);
static void set_status_line(const char *text);
static void term_write(const char *s);
static void terminal_request_repaint(void);
static void enable_macro_buttons(BOOL enable);
static void cancel_macro_timer(void);
/* install_macro_button_subclasses / paint_macro_button / macro_btn_proc
 * were retired by dispatch amendment 2026-06-15-3 (item 1) when the
 * macro buttons switched from BS_OWNERDRAW to stock BS_PUSHBUTTON.
 * The Windows visual-styles manager now paints them in the same
 * theme as the F3 Gopher toolbar. */

/* Polish 6 (2026-05-22): selection + clipboard helpers. */
static void telnet_module_copy_selection(void);
static void telnet_module_paste(void);
static void pixel_to_cell(int x_px, int y_px, int *out_vis_row, int *out_col);
static void arm_sel_scroll_timer(HWND hwnd, int dir);
static void kill_sel_scroll_timer(HWND hwnd);
static void handle_char_shell(int ch);
static void send_to_server_bytes(const unsigned char *bytes, int n);

/* ---------- helpers ---------- */

static void measure_cell(HWND hwnd)
{
    HDC        hdc;
    HFONT      old;
    TEXTMETRIC tm;
    if (!hwnd || !g_hFontTelnet) return;
    hdc = GetDC(hwnd);
    old = (HFONT)SelectObject(hdc, g_hFontTelnet);
    GetTextMetrics(hdc, &tm);
    g_cell_w = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 9;
    g_cell_h = tm.tmHeight        > 0 ? tm.tmHeight     : 18;
    SelectObject(hdc, old);
    ReleaseDC(hwnd, hdc);
}

static void compute_grid_for_client(HWND hwnd, int *out_cols, int *out_rows)
{
    RECT rc;
    int  cw, rh;
    int  cols, rows;
    GetClientRect(hwnd, &rc);
    cw = g_cell_w > 0 ? g_cell_w : 9;
    rh = g_cell_h > 0 ? g_cell_h : 18;
    cols = (rc.right  - rc.left) / cw;
    rows = (rc.bottom - rc.top ) / rh;
    /* Hard floor at 80x24. Finger and QOTD do not honor NAWS, so their
     * 80-column output would wrap mid-word inside our local VT buffer
     * if cols ever dipped below 80. Above the floor we let cols grow
     * with the window so a wide window shows more columns; below the
     * floor we clamp and accept that the right edge clips. NAWS sent
     * to Telnet stays pinned at 80x24 in telnet_proto regardless. */
    if (cols < TELNET_COLS_MIN) cols = TELNET_COLS_MIN;
    if (rows < TELNET_ROWS_MIN) rows = TELNET_ROWS_MIN;
    *out_cols = cols;
    *out_rows = rows;
}

static void term_write(const char *s)
{
    if (!s || !*s) return;
    vt_term_feed_bytes(&g_term, (const unsigned char *)s, strlen(s));
    /* Dispatch amendment 2026-06-15-2 (item C): pin the visible
     * window to the bottom of the buffer after every local emit so
     * the boot banner, "Connection closed.", and the "$ " prompt
     * always land on a visible row. The wire-data path already calls
     * scroll_to_bottom in on_evt_data, but the locally-emitted text
     * (prompts, error messages, banner) used to leave scroll_offset
     * wherever the wheel/PageUp left it. The user can scroll back up
     * any time with the wheel; the next local emit will re-pin, which
     * is the documented "bottom-pinned until user scrolls back" rule
     * from the dispatch. */
    if (g_term_inited) vt_term_scroll_to_bottom(&g_term);
}

static void terminal_request_repaint(void)
{
    if (g_hRender) InvalidateRect(g_hRender, NULL, FALSE);
}

static void key_log_push(const char *tok)
{
    int slot;
    if (!tok) return;
    slot = g_key_log_count % KEY_LOG_DEPTH;
    strncpy(g_key_log[slot], tok, sizeof(g_key_log[slot]) - 1);
    g_key_log[slot][sizeof(g_key_log[slot]) - 1] = '\0';
    g_key_log_count += 1;
}

static void key_log_format(char *out, size_t cap)
{
    int i, count, start, slot;
    out[0] = '\0';
    if (g_key_log_count <= 0) return;
    count = g_key_log_count < KEY_LOG_DEPTH ? g_key_log_count : KEY_LOG_DEPTH;
    start = g_key_log_count - count;
    for (i = 0; i < count; i++) {
        slot = (start + i) % KEY_LOG_DEPTH;
        if (i > 0) strncat(out, " ", cap - strlen(out) - 1);
        strncat(out, g_key_log[slot], cap - strlen(out) - 1);
    }
}

/* Polish 2 / 3 / 4 (2026-05-22) diagnostic: append "[<cols>x<rows>
 * CR=N LF=M KEY: ...]" so the live terminal grid dimensions,
 * wire-level CR/LF accounting, and the most recent keyboard events
 * are visible in the status bar. KEY: was added in polish 4 to triage
 * IsDialogMessage swallowing letter keystrokes. */
static void render_status_line(void)
{
    char out[640];
    char keylog[128];
    int  cols, rows;
    unsigned long cr, lf;
    if (!g_hStatus) return;
    cols = g_term_inited ? g_term.cols     : 0;
    rows = g_term_inited ? g_term.rows     : 0;
    cr   = g_term_inited ? g_term.stat_cr  : 0;
    lf   = g_term_inited ? g_term.stat_lf  : 0;
    key_log_format(keylog, sizeof(keylog));
    if (keylog[0]) {
        _snprintf(out, sizeof(out),
                  "%s  [%dx%d  CR=%lu LF=%lu  KEY: %s]",
                  g_status_base, cols, rows, cr, lf, keylog);
    } else {
        _snprintf(out, sizeof(out),
                  "%s  [%dx%d  CR=%lu LF=%lu]",
                  g_status_base, cols, rows, cr, lf);
    }
    out[sizeof(out) - 1] = '\0';
    SetWindowTextA(g_hStatus, out);
}

static void set_status_line(const char *text)
{
    strncpy(g_status_base, text ? text : "", sizeof(g_status_base) - 1);
    g_status_base[sizeof(g_status_base) - 1] = '\0';
    render_status_line();
}

static void enable_macro_buttons(BOOL enable)
{
    if (g_hBtnTelnet)     EnableWindow(g_hBtnTelnet,     enable);
    if (g_hBtnFingerNews) EnableWindow(g_hBtnFingerNews, enable);
    if (g_hBtnFingerWx)   EnableWindow(g_hBtnFingerWx,   enable);
    if (g_hBtnQotd)       EnableWindow(g_hBtnQotd,       enable);
    if (g_hBtnArchie)     EnableWindow(g_hBtnArchie,     enable);
}

static void cancel_macro_timer(void)
{
    if (g_macro_pending && g_hRender) {
        KillTimer(g_hRender, ID_TIMER_MACRO_AUTOENTER);
        g_macro_pending = FALSE;
    }
}

/* ---------- selection helpers (polish 6) ---------- */

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

static void telnet_module_copy_selection(void)
{
    char *text;
    if (!g_term_inited || !g_term.sel_active) return;
    text = vt_term_select_extract(&g_term);
    if (!text) return;
    suite_clipboard_put_text(g_hRender, text);
    free(text);
}

static void telnet_module_paste(void)
{
    char *text;
    char *p;
    int   skip_lf = 0;
    text = suite_clipboard_get_text(g_hRender);
    if (!text) return;
    for (p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (skip_lf && c == '\n') { skip_lf = 0; continue; }
        skip_lf = 0;
        if (c == '\r' || c == '\n') {
            if (c == '\r') skip_lf = 1;
            if (g_mode == F6_SHELL) {
                handle_char_shell('\r');
            } else if (g_mode == F6_TELNET_ACTIVE) {
                unsigned char cr = '\r';
                send_to_server_bytes(&cr, 1);
            }
        } else if (c >= 0x20 && c < 0x7F) {
            if (g_mode == F6_SHELL) {
                handle_char_shell((int)c);
            } else if (g_mode == F6_TELNET_ACTIVE) {
                send_to_server_bytes(&c, 1);
            }
        }
        /* Non-printable bytes other than \r\n: skip. */
    }
    free(text);
}

/* ---------- boot banner ---------- */

static void emit_boot_banner(void)
{
    term_write("The Montreal Greek Times Unicorn Suite Terminal\r\n");
    term_write("Type 'help' to list available commands,\r\n");
    term_write("or select one of the shortcut buttons above:\r\n");
    term_write("Telnet News, Finger News, Finger Weather, QOTD, or Archie.\r\n");
    term_write("(c) 2026 Dimitri Papadopoulos, The Montreal Greek Times\r\n");
    term_write("\r\n");
    term_write("$ ");
    g_banner_emitted = TRUE;
}

/* ---------- shell mode entry ---------- */

static void enter_shell_mode(BOOL write_prompt)
{
    g_mode = F6_SHELL;
    g_shell_input_len = 0;
    g_shell_input_pos = 0;
    g_shell_input[0] = '\0';
    if (write_prompt) {
        term_write("$ ");
    }
    set_status_line("Local shell");
    enable_macro_buttons(TRUE);
    terminal_request_repaint();
}

/* ---------- connection helpers ---------- */

static int parse_host_port(const char *spec, char *out_host, size_t host_cap,
                           int default_port, int *out_port)
{
    const char *colon;
    size_t      hlen;

    if (!spec || !*spec) return 0;

    colon = strchr(spec, ':');
    if (colon) {
        hlen = (size_t)(colon - spec);
        if (hlen >= host_cap) hlen = host_cap - 1;
        memcpy(out_host, spec, hlen);
        out_host[hlen] = '\0';
        *out_port = atoi(colon + 1);
        if (*out_port <= 0 || *out_port > 65535) *out_port = default_port;
    } else {
        hlen = strlen(spec);
        if (hlen >= host_cap) hlen = host_cap - 1;
        memcpy(out_host, spec, hlen);
        out_host[hlen] = '\0';
        *out_port = default_port;
    }
    return out_host[0] ? 1 : 0;
}

static void open_telnet(const char *spec)
{
    char host[256];
    int  port;
    char status_buf[320];
    int  rc;

    if (!parse_host_port(spec, host, sizeof(host), 23, &port)) {
        term_write("telnet: missing host\r\n");
        enter_shell_mode(TRUE);
        return;
    }

    strncpy(g_active_host, host, sizeof(g_active_host) - 1);
    g_active_host[sizeof(g_active_host) - 1] = '\0';
    g_active_port = port;

    _snprintf(status_buf, sizeof(status_buf),
              "Connecting to %s:%d...", host, port);
    set_status_line(status_buf);
    {
        char line[320];
        _snprintf(line, sizeof(line), "Connecting to %s:%d...\r\n", host, port);
        term_write(line);
    }

    rc = telnet_proto_open(host, port,
                           g_hRender, WM_TELNET_NOTIFY,
                           &g_active_telnet);
    if (rc != 0 || !g_active_telnet) {
        term_write("Failed to start Telnet connection.\r\n");
        enter_shell_mode(TRUE);
        return;
    }
    g_mode = F6_TELNET_CONNECTING;
    enable_macro_buttons(FALSE);
    terminal_request_repaint();
}

static void open_finger(const char *user_at_host)
{
    const char *at;
    char        user[128];
    char        host[256];
    int         rc;
    size_t      n;
    char        status_buf[320];

    if (!user_at_host || !*user_at_host) {
        term_write("finger: missing user@host\r\n");
        enter_shell_mode(TRUE);
        return;
    }
    at = strchr(user_at_host, '@');
    if (!at) {
        term_write("finger: expected user@host\r\n");
        enter_shell_mode(TRUE);
        return;
    }
    n = (size_t)(at - user_at_host);
    if (n >= sizeof(user)) n = sizeof(user) - 1;
    memcpy(user, user_at_host, n);
    user[n] = '\0';
    strncpy(host, at + 1, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    if (!host[0]) {
        term_write("finger: missing host\r\n");
        enter_shell_mode(TRUE);
        return;
    }

    strncpy(g_active_host, host, sizeof(g_active_host) - 1);
    g_active_host[sizeof(g_active_host) - 1] = '\0';
    g_active_port = 79;
    _snprintf(status_buf, sizeof(status_buf),
              "Reading from %s:79...", host);
    set_status_line(status_buf);

    rc = finger_proto_open(host, 79, user,
                           g_hRender, WM_TELNET_NOTIFY,
                           &g_active_finger);
    if (rc != 0 || !g_active_finger) {
        term_write("Failed to start Finger query.\r\n");
        enter_shell_mode(TRUE);
        return;
    }
    g_mode = F6_FINGER_READING;
    enable_macro_buttons(FALSE);
    terminal_request_repaint();
}

static void open_qotd_raw(const char *host, int port)
{
    int  rc;
    char status_buf[320];

    strncpy(g_active_host, host, sizeof(g_active_host) - 1);
    g_active_host[sizeof(g_active_host) - 1] = '\0';
    g_active_port = port;
    _snprintf(status_buf, sizeof(status_buf),
              "Reading from %s:%d...", host, port);
    set_status_line(status_buf);

    rc = qotd_proto_open(host, port, g_hRender, WM_TELNET_NOTIFY, &g_active_qotd);
    if (rc != 0 || !g_active_qotd) {
        term_write("Failed to open raw TCP connection.\r\n");
        enter_shell_mode(TRUE);
        return;
    }
    g_mode = F6_QOTD_READING;
    enable_macro_buttons(FALSE);
    terminal_request_repaint();
}

static void close_active_session(BOOL announce)
{
    if (g_active_telnet) { telnet_proto_close(g_active_telnet); g_active_telnet = NULL; }
    if (g_active_finger) { finger_proto_close(g_active_finger); g_active_finger = NULL; }
    if (g_active_qotd)   { qotd_proto_close  (g_active_qotd);   g_active_qotd   = NULL; }
    if (announce) term_write("Connection closed.\r\n");
    enter_shell_mode(TRUE);
}

/* ---------- shell command dispatch ---------- */

static void rtrim(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) {
        s[--n] = '\0';
    }
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int copy_token(const char **pp, char *out, size_t cap)
{
    const char *p = *pp;
    size_t i = 0;
    p = skip_ws(p);
    while (*p && *p != ' ' && *p != '\t' && i + 1 < cap) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    *pp = p;
    return (int)i;
}

static void shell_dispatch(const char *cmd_in)
{
    char        line[1024];
    char        verb[32];
    const char *p;

    strncpy(line, cmd_in, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    rtrim(line);

    p = skip_ws(line);
    if (!*p) return;          /* empty input: no error, just new prompt */

    copy_token(&p, verb, sizeof(verb));
    p = skip_ws(p);

    if (strcmp(verb, "help") == 0) {
        term_write("Available commands:\r\n");
        term_write("  telnet <host>[:port]      open a Telnet connection\r\n");
        term_write("  finger <user>@<host>      query a Finger service\r\n");
        term_write("  nc <host> <port>          raw TCP read\r\n");
        term_write("  clear                     clear the terminal\r\n");
        term_write("  help                      show this help\r\n");
        term_write("  exit / quit               close active connection\r\n");
        term_write("Archie interactive: telnet archie.greektimes.ca:2323\r\n");
        term_write("  (then: find <term>, exact <term>, prog, help, quit)\r\n");
        term_write("(Use the buttons above for quick access.)\r\n");
        term_write("$ ");
        return;
    }
    if (strcmp(verb, "clear") == 0) {
        vt_term_clear_all(&g_term);
        term_write("$ ");
        terminal_request_repaint();
        return;
    }
    if (strcmp(verb, "exit") == 0 || strcmp(verb, "quit") == 0) {
        if (g_active_telnet || g_active_finger || g_active_qotd) {
            close_active_session(TRUE);
        } else {
            term_write("No active connection.\r\n");
            term_write("$ ");
        }
        return;
    }
    if (strcmp(verb, "telnet") == 0) {
        char host_spec[300];
        copy_token(&p, host_spec, sizeof(host_spec));
        if (!host_spec[0]) {
            term_write("usage: telnet <host>[:port]\r\n");
            term_write("$ ");
            return;
        }
        open_telnet(host_spec);
        return;
    }
    if (strcmp(verb, "finger") == 0) {
        char target[300];
        copy_token(&p, target, sizeof(target));
        if (!target[0]) {
            term_write("usage: finger <user>@<host>\r\n");
            term_write("$ ");
            return;
        }
        open_finger(target);
        return;
    }
    if (strcmp(verb, "nc") == 0) {
        char host[256];
        char portstr[16];
        int  port;
        copy_token(&p, host, sizeof(host));
        copy_token(&p, portstr, sizeof(portstr));
        if (!host[0] || !portstr[0]) {
            term_write("usage: nc <host> <port>\r\n");
            term_write("$ ");
            return;
        }
        port = atoi(portstr);
        if (port <= 0 || port > 65535) {
            term_write("nc: invalid port\r\n");
            term_write("$ ");
            return;
        }
        open_qotd_raw(host, port);
        return;
    }

    {
        char msg[200];
        _snprintf(msg, sizeof(msg), "%s: command not found\r\n", verb);
        term_write(msg);
        term_write("$ ");
    }
}

/* ---------- macro button helper ---------- */

static void macro_inject(const char *text)
{
    int i, len;

    if (g_mode != F6_SHELL) return;

    /* Erase whatever the user already typed locally. Move the cursor to
     * the end first so the destructive "\b \b" run erases the whole line
     * regardless of where the edit cursor sits. */
    for (i = g_shell_input_pos; i < g_shell_input_len; i++) {
        char b[2]; b[0] = g_shell_input[i]; b[1] = '\0'; term_write(b);
    }
    for (i = 0; i < g_shell_input_len; i++) term_write("\b \b");
    g_shell_input_len = 0;

    /* Set buffer + local echo. */
    strncpy(g_shell_input, text, sizeof(g_shell_input) - 1);
    g_shell_input[sizeof(g_shell_input) - 1] = '\0';
    len = (int)strlen(g_shell_input);
    g_shell_input_len = len;
    g_shell_input_pos = len;
    term_write(g_shell_input);
    terminal_request_repaint();

    /* Arm the auto-Enter timer.
     * Dispatch amendment 2026-06-15 (item 3): wait time dropped from
     * 2000 ms to 1000 ms so the shortcut feels less laggy. */
    cancel_macro_timer();
    if (g_hRender) {
        SetTimer(g_hRender, ID_TIMER_MACRO_AUTOENTER, 1000, NULL);
        g_macro_pending = TRUE;
    }
}

/* Triggered both by manual Enter and by the auto-Enter timer. */
static void shell_commit_enter(void)
{
    cancel_macro_timer();
    term_write("\r\n");
    {
        char tmp[1024];
        strncpy(tmp, g_shell_input, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        g_shell_input_len = 0;
        g_shell_input_pos = 0;
        g_shell_input[0]  = '\0';
        shell_dispatch(tmp);
    }
    terminal_request_repaint();
}

/* ---------- input dispatch (WM_CHAR / WM_KEYDOWN on render) ---------- */

static void send_to_server_bytes(const unsigned char *bytes, int n)
{
    if (g_mode == F6_TELNET_ACTIVE && g_active_telnet) {
        telnet_proto_send(g_active_telnet, bytes, (size_t)n);
    }
}

static void send_to_server_str(const char *s)
{
    send_to_server_bytes((const unsigned char *)s, (int)strlen(s));
}

/* Re-render the shell input line after a mid-line edit and leave the
 * terminal cursor at g_shell_input_pos. Uses only BS / overwrite (the
 * same primitives the destructive "\b \b" backspace relied on), so it
 * needs no CSI support from vt_term. `prev_cursor_off` is where the
 * terminal cursor sat before the edit (== old g_shell_input_pos);
 * `prev_len` is the old line length, so a now-shorter line gets its
 * trailing leftover cells erased. (Command lines do not wrap in
 * practice; a line longer than one terminal row is the one case this
 * BS-based redraw cannot cross.) */
static void shell_render_edit(int prev_cursor_off, int prev_len)
{
    int i, extra;
    for (i = 0; i < prev_cursor_off; i++) term_write("\b");   /* to input start */
    if (g_shell_input_len > 0) term_write(g_shell_input);      /* rewrite line */
    extra = prev_len - g_shell_input_len;                      /* erase leftovers */
    for (i = 0; i < extra; i++) term_write(" ");
    for (i = 0; i < extra; i++) term_write("\b");
    for (i = g_shell_input_len; i > g_shell_input_pos; i--) term_write("\b");
}

static void handle_char_shell(int ch)
{
    if (g_macro_pending) cancel_macro_timer();

    if (ch == '\r') {
        shell_commit_enter();
        return;
    }
    if (ch == '\b' || ch == 0x7F) {   /* Backspace: delete char before cursor */
        if (g_shell_input_pos > 0) {
            int prev_pos = g_shell_input_pos;
            int prev_len = g_shell_input_len;
            if (prev_pos == prev_len) {
                /* Fast path: deleting at end (identical to old behavior). */
                g_shell_input_len--;
                g_shell_input_pos--;
                g_shell_input[g_shell_input_len] = '\0';
                term_write("\b \b");
            } else {
                memmove(&g_shell_input[prev_pos - 1], &g_shell_input[prev_pos],
                        (size_t)(prev_len - prev_pos) + 1);   /* include NUL */
                g_shell_input_len--;
                g_shell_input_pos--;
                shell_render_edit(prev_pos, prev_len);
            }
            terminal_request_repaint();
        }
        return;
    }
    if (ch == 0x0C) {  /* Ctrl-L = clear */
        int i;
        vt_term_clear_all(&g_term);
        term_write("$ ");
        if (g_shell_input_len > 0) term_write(g_shell_input);
        for (i = g_shell_input_len; i > g_shell_input_pos; i--) term_write("\b");
        terminal_request_repaint();
        return;
    }
    if (ch == 0x03) {  /* Ctrl-C = cancel current line */
        term_write("^C\r\n$ ");
        g_shell_input_len = 0;
        g_shell_input_pos = 0;
        g_shell_input[0] = '\0';
        terminal_request_repaint();
        return;
    }
    if (ch >= 0x20 && ch < 0x7F) {     /* insert printable at cursor */
        if (g_shell_input_len < (int)sizeof(g_shell_input) - 1) {
            int prev_pos = g_shell_input_pos;
            int prev_len = g_shell_input_len;
            char buf[2];
            buf[0] = (char)ch; buf[1] = '\0';
            if (prev_pos == prev_len) {
                /* Fast path: appending at end (identical to old behavior). */
                g_shell_input[prev_len] = (char)ch;
                g_shell_input_len++;
                g_shell_input_pos++;
                g_shell_input[g_shell_input_len] = '\0';
                term_write(buf);
            } else {
                memmove(&g_shell_input[prev_pos + 1], &g_shell_input[prev_pos],
                        (size_t)(prev_len - prev_pos) + 1);   /* include NUL */
                g_shell_input[prev_pos] = (char)ch;
                g_shell_input_len++;
                g_shell_input_pos++;
                shell_render_edit(prev_pos, prev_len);
            }
            terminal_request_repaint();
        }
    }
}

static void handle_char_telnet(int ch)
{
    /* Local echo only when the server is NOT echoing for us (RFC 857:
     * absent an active DO/WILL ECHO the client is responsible for echo).
     * A line service like the interactive Archie server (udp is the query
     * side; 2323 is this line interface) neither echoes nor accepts CR
     * alone, so without local echo the user types blind. Character-mode
     * servers that send WILL ECHO handle their own echo and get none from
     * us, avoiding a double echo. */
    BOOL echo = g_active_telnet && !telnet_proto_server_echo(g_active_telnet);

    if (ch == '\r') {
        /* NVT Enter is CR-LF (RFC 854). The interactive Archie service
         * ignores a bare CR and only acts on a line terminated by LF, so
         * CR alone made typing appear to do nothing. CR-LF works for it
         * and for the other Telnet targets. */
        unsigned char crlf[2] = { '\r', '\n' };
        send_to_server_bytes(crlf, 2);
        if (echo) { term_write("\r\n"); terminal_request_repaint(); }
        return;
    }
    if (ch == '\b' || ch == 0x7F) {
        /* Telnet servers usually want DEL (0x7F) for backspace. */
        unsigned char b = 0x7F;
        send_to_server_bytes(&b, 1);
        if (echo) { term_write("\b \b"); terminal_request_repaint(); }
        return;
    }
    if (ch >= 0x20 && ch < 0x7F) {
        unsigned char b = (unsigned char)ch;
        send_to_server_bytes(&b, 1);
        if (echo) { char s[2]; s[0] = (char)ch; s[1] = '\0'; term_write(s);
                    terminal_request_repaint(); }
        return;
    }
    /* Other C0 controls pass through as-is (not locally echoed). */
    if (ch > 0 && ch < 0x20) {
        unsigned char b = (unsigned char)ch;
        send_to_server_bytes(&b, 1);
    }
}

static void handle_vk_arrow(int vk)
{
    /* Routing per F6 spec (revised 2026-05-22 polish 2):
     *   Up/Down arrows         only act in F6_TELNET_ACTIVE, where they
     *                          forward to the server. In shell / Finger
     *                          reading / QOTD reading they are NO-OPS
     *                          (no local scroll). Local scroll lives on
     *                          Page Up/Down and the mouse wheel.
     *   Page Up/Down + wheel   forward in F6_TELNET_ACTIVE, otherwise
     *                          scroll the local VT buffer. */
    if (g_mode == F6_TELNET_ACTIVE) {
        switch (vk) {
        case VK_UP:    send_to_server_str("\x1b[A"); break;
        case VK_DOWN:  send_to_server_str("\x1b[B"); break;
        case VK_RIGHT: send_to_server_str("\x1b[C"); break;
        case VK_LEFT:  send_to_server_str("\x1b[D"); break;
        case VK_HOME:  send_to_server_str("\x1b[H"); break;
        case VK_END:   send_to_server_str("\x1b[F"); break;
        case VK_PRIOR: send_to_server_str("\x1b[5~"); break;
        case VK_NEXT:  send_to_server_str("\x1b[6~"); break;
        }
    } else if (g_mode == F6_SHELL) {
        /* Local shell line editing: Left/Right move the insertion cursor
         * within g_shell_input (non-destructive BS to go left, re-emit the
         * char under the cursor to go right); Home/End jump to the ends.
         * Page Up/Down still scroll the local VT buffer. */
        switch (vk) {
        case VK_LEFT:
            if (g_shell_input_pos > 0) {
                g_shell_input_pos--;
                term_write("\b");
                terminal_request_repaint();
            }
            break;
        case VK_RIGHT:
            if (g_shell_input_pos < g_shell_input_len) {
                char b[2];
                b[0] = g_shell_input[g_shell_input_pos]; b[1] = '\0';
                g_shell_input_pos++;
                term_write(b);
                terminal_request_repaint();
            }
            break;
        case VK_HOME:
            while (g_shell_input_pos > 0) { g_shell_input_pos--; term_write("\b"); }
            terminal_request_repaint();
            break;
        case VK_END:
            while (g_shell_input_pos < g_shell_input_len) {
                char b[2];
                b[0] = g_shell_input[g_shell_input_pos]; b[1] = '\0';
                g_shell_input_pos++;
                term_write(b);
            }
            terminal_request_repaint();
            break;
        case VK_PRIOR: vt_term_scroll_by(&g_term,  g_term.rows);
                       terminal_request_repaint(); break;
        case VK_NEXT:  vt_term_scroll_by(&g_term, -g_term.rows);
                       terminal_request_repaint(); break;
        default: break;
        }
    } else {
        switch (vk) {
        case VK_PRIOR: vt_term_scroll_by(&g_term,  g_term.rows); break;
        case VK_NEXT:  vt_term_scroll_by(&g_term, -g_term.rows); break;
        default: return;
        }
        terminal_request_repaint();
    }
}

static void handle_wheel(int notches)
{
    if (g_mode == F6_TELNET_ACTIVE) {
        int abs_n = notches < 0 ? -notches : notches;
        int i;
        for (i = 0; i < abs_n; i++) {
            send_to_server_str(notches > 0 ? "\x1b[A\x1b[A\x1b[A" : "\x1b[B\x1b[B\x1b[B");
        }
    } else {
        vt_term_scroll_by(&g_term, notches * 3);
        terminal_request_repaint();
    }
}

/* ---------- WM_TELNET_NOTIFY arrival ---------- */

static void on_evt_connected(void)
{
    char buf[320];
    if (g_mode == F6_TELNET_CONNECTING) {
        g_mode = F6_TELNET_ACTIVE;
        _snprintf(buf, sizeof(buf), "Connected to %s:%d",
                  g_active_host, g_active_port);
        set_status_line(buf);
    }
    /* Finger and QOTD also get TELNET_EVT_CONNECTED; their status was
     * already set to "Reading from ..." at open. Nothing to do here. */
}

static void on_evt_data(TelnetDataChunk *chunk)
{
    if (!chunk) return;
    if (chunk->data && chunk->len > 0) {
        vt_term_feed_bytes(&g_term, chunk->data, (size_t)chunk->len);
        vt_term_scroll_to_bottom(&g_term);
        render_status_line();   /* surface fresh CR/LF counts */
        terminal_request_repaint();
    }
    telnet_proto_free_chunk(chunk);
}

static void on_evt_closed(void)
{
    if (g_active_telnet) { telnet_proto_close(g_active_telnet); g_active_telnet = NULL; }
    if (g_active_finger) { finger_proto_close(g_active_finger); g_active_finger = NULL; }
    if (g_active_qotd)   { qotd_proto_close  (g_active_qotd);   g_active_qotd   = NULL; }
    term_write("\r\nConnection closed.\r\n");
    enter_shell_mode(TRUE);
}

static void on_evt_error(const char *msg)
{
    if (!msg) msg = "Connection error";
    {
        char buf[320];
        _snprintf(buf, sizeof(buf), "Error: %s\r\n", msg);
        term_write(buf);
    }
}

/* ---------- render window proc ---------- */

/* paint_terminal: double-buffered render. All drawing goes to an
 * off-screen memory DC, then a single BitBlt of the dirty rect lands
 * on screen. WM_ERASEBKGND returns 1 above, so Windows does not fill
 * the client area before WM_PAINT. The combination eliminates the
 * flash users were seeing while typing fast.
 *
 * Cursor cell rule: blink-on paints a cell-aligned phosphor block,
 * then redraws the character at the cursor in BG color with
 * TRANSPARENT mode so the block stays exactly one cell wide. Blink-off
 * paints nothing extra; the cursor cell is just whatever the main
 * row loop already wrote there. This avoids the spillover artifact
 * that the previous OPAQUE-overlay version produced (the character's
 * own background advance can extend a pixel past tmAveCharWidth and
 * left those slivers behind when the block came down). */
static void paint_terminal(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC         hdc;
    HDC         mem_dc   = NULL;
    HBITMAP     mem_bmp  = NULL;
    HBITMAP     old_bmp  = NULL;
    HFONT       old_font = NULL;
    RECT        rc;
    int         row, col;
    int         px, py;
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
        /* Fall back to direct paint on the screen DC. */
        FillRect(hdc, &ps.rcPaint, g_hBrushTermBg);
        EndPaint(hwnd, &ps);
        return;
    }
    old_bmp = (HBITMAP)SelectObject(mem_dc, mem_bmp);

    FillRect(mem_dc, &rc, g_hBrushTermBg);

    if (g_term_inited) {
        old_font = (HFONT)SelectObject(mem_dc, g_hFontTelnet);
        SetTextColor(mem_dc, TELNET_COL_FG);
        SetBkColor  (mem_dc, TELNET_COL_BG);
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
            /* Polish 6 (2026-05-22): split each row into runs of same
             * selection state and render each run with the matching
             * style. Non-selected runs use OPAQUE bg on phosphor green;
             * selected runs paint a green block via FillRect then
             * write the character in BG color with TRANSPARENT mode
             * (the same cell-aligned trick polish 1 used for the
             * cursor block, so glyph advances cannot bleed outside). */
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
                    if (g_hBrushTermFg) FillRect(mem_dc, &rs, g_hBrushTermFg);
                    SetTextColor(mem_dc, TELNET_COL_BG);
                    SetBkMode   (mem_dc, TRANSPARENT);
                } else {
                    SetTextColor(mem_dc, TELNET_COL_FG);
                    SetBkColor  (mem_dc, TELNET_COL_BG);
                    SetBkMode   (mem_dc, OPAQUE);
                }
                TextOutA(mem_dc, x_px, py, buf + run_start, run_len);
                run_start = run_end;
            }
        }

        if (g_cursor_visible && g_term.scroll_offset == 0) {
            RECT cur;
            px = g_term.cursor_x * g_cell_w;
            py = g_term.cursor_y * g_cell_h;
            cur.left   = px;
            cur.top    = py;
            cur.right  = px + g_cell_w;
            cur.bottom = py + g_cell_h;
            if (g_hBrushTermFg) FillRect(mem_dc, &cur, g_hBrushTermFg);
            {
                const VTCell *cells = vt_term_get_visible(&g_term, g_term.cursor_y);
                if (cells && g_term.cursor_x < g_term.cols) {
                    unsigned char ch = (unsigned char)cells[g_term.cursor_x].ch;
                    char s[2];
                    if (ch < 0x20 || ch >= 0x7F) ch = ' ';
                    s[0] = (char)ch; s[1] = '\0';
                    SetTextColor(mem_dc, TELNET_COL_BG);
                    SetBkMode   (mem_dc, TRANSPARENT);
                    TextOutA(mem_dc, px, py, s, 1);
                }
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

static LRESULT CALLBACK render_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        measure_cell(hwnd);
        if (!g_term_inited) {
            vt_term_init(&g_term, TELNET_COLS_MIN, TELNET_ROWS_MIN, TELNET_SCROLLBK);
            /* Polish 3 (2026-05-22): the F6 module talks to RFC 1288
             * Finger and RFC 865 QOTD services that send bare LF
             * between rows (verified live: CR=7 LF=43 on a single
             * Finger News response). Without LF-implies-CR the cursor
             * advances row but stays at the previous column, producing
             * the staircase Dimitri reported. Enabling the option on
             * vt_term restores the cooked-mode line discipline these
             * services implicitly assume. RFC 854 Telnet still works
             * because its CR LF translates to CR + (CR LF) which is
             * idempotent: cursor_x is already 0 when LF arrives. */
            vt_term_set_lf_to_crlf(&g_term, 1);
            g_term_inited = TRUE;
        }
        SetTimer(hwnd, ID_TIMER_CURSOR_BLINK, 500, NULL);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    /* Polish 4 (2026-05-22): the suite's main message loop runs
     * IsDialogMessage when focus is inside g_hwndContent, which by
     * default eats Tab / arrows / Enter / mnemonic-matching letter
     * keys for dialog navigation. The render window is custom-painted
     * and owns its own keyboard, so we declare DLGC_WANTALLKEYS (plus
     * the more specific WANTCHARS / WANTARROWS for older Windows
     * versions that consult those individually). Without this, letter
     * keystrokes were intermittently consumed by the mnemonic search
     * before reaching our WM_CHAR handler. */
    case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTALLKEYS;

    case WM_PAINT:
        paint_terminal(hwnd);
        return 0;

    case WM_SIZE: {
        int cols, rows;
        compute_grid_for_client(hwnd, &cols, &rows);
        if (g_term_inited && (cols != g_term.cols || rows != g_term.rows)) {
            vt_term_resize(&g_term, cols, rows);
            render_status_line();   /* diagnostic suffix tracks new dims */
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (wParam == ID_TIMER_CURSOR_BLINK) {
            g_cursor_visible = !g_cursor_visible;
            if (g_term.scroll_offset == 0) {
                RECT cur;
                cur.left   = g_term.cursor_x * g_cell_w;
                cur.top    = g_term.cursor_y * g_cell_h;
                cur.right  = cur.left + g_cell_w;
                cur.bottom = cur.top  + g_cell_h;
                InvalidateRect(hwnd, &cur, FALSE);
            }
            return 0;
        }
        if (wParam == ID_TIMER_MACRO_AUTOENTER) {
            KillTimer(hwnd, ID_TIMER_MACRO_AUTOENTER);
            g_macro_pending = FALSE;
            if (g_mode == F6_SHELL) shell_commit_enter();
            return 0;
        }
        if (wParam == ID_TIMER_SEL_AUTOSCROLL) {
            POINT pt;
            int vis, col, logical_row;
            if (!g_sel_dragging) {
                kill_sel_scroll_timer(hwnd);
                return 0;
            }
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
        if (y < 0)              arm_sel_scroll_timer(hwnd, +1);
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
        /* A click without drag (anchor == lead) clears the selection
         * so a stray click does not leave a stuck single-cell highlight. */
        if (g_term.sel_anchor_row == g_term.sel_lead_row &&
            g_term.sel_anchor_col == g_term.sel_lead_col) {
            vt_term_select_clear(&g_term);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_RBUTTONDOWN: {
        HMENU hm = CreatePopupMenu();
        UINT  fc = (g_term_inited && g_term.sel_active) ? MF_ENABLED : MF_GRAYED;
        UINT  fp = suite_clipboard_has_text() ? MF_ENABLED : MF_GRAYED;
        POINT pt;
        if (!hm) return 0;
        AppendMenuA(hm, MF_STRING | fc, ID_CTX_COPY,        "Copy\tCtrl+C");
        AppendMenuA(hm, MF_STRING | fp, ID_CTX_PASTE,       "Paste\tCtrl+V");
        AppendMenuA(hm, MF_SEPARATOR,    0,                  NULL);
        AppendMenuA(hm, MF_STRING,       ID_CTX_SELECT_ALL, "Select All\tCtrl+A");
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        ClientToScreen(hwnd, &pt);
        SetFocus(hwnd);
        TrackPopupMenu(hm, TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                       pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(hm);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_CTX_COPY) {
            telnet_module_copy_selection();
            return 0;
        }
        if (id == ID_CTX_PASTE) {
            telnet_module_paste();
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
        int vk = (int)wParam;
        char tok[16];
        BOOL ctrl;
        _snprintf(tok, sizeof(tok), "vk=%X", vk);
        tok[sizeof(tok) - 1] = '\0';
        key_log_push(tok);
        render_status_line();
        /* Polish 6 (2026-05-22): Ctrl-A / Ctrl-C / Ctrl-V handled
         * before anything else so they cannot fall through to the
         * arrow handler or the polish-5 VK-shell derivation. */
        ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl) {
            if (vk == 'A') {
                vt_term_select_all(&g_term);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (vk == 'C') {
                if (g_term_inited && g_term.sel_active) {
                    telnet_module_copy_selection();
                    vt_term_select_clear(&g_term);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                /* No selection: fall through to existing Ctrl-C semantics.
                 * Active Telnet: send 0x03 to server.
                 * Shell: handle_char_shell(0x03) clears the line. */
                if (g_mode == F6_TELNET_ACTIVE) {
                    unsigned char b = 0x03;
                    send_to_server_bytes(&b, 1);
                    return 0;
                }
                if (g_mode == F6_SHELL) {
                    handle_char_shell(0x03);
                    return 0;
                }
                return 0;
            }
            if (vk == 'V') {
                telnet_module_paste();
                return 0;
            }
        }
        switch (vk) {
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
            handle_vk_arrow(vk);
            return 0;
        }
        /* Polish 5 (2026-05-22): layout-independent shell input.
         * On non-English keyboard layouts (Greek, Russian, ...) the
         * WM_CHAR for an A-Z key carries the layout-translated codepage
         * byte (e.g. 0xF4 Greek tau for 'T' on Greek). That byte is
         * outside our 0x20-0x7E shell input filter, so the keystroke
         * vanishes. Shell commands are themselves ASCII (telnet,
         * finger, help, clear, exit, nc, quit) so we derive the
         * character directly from the VK + shift/caps state and feed
         * it ourselves. The flag arms WM_CHAR to swallow the duplicate
         * the dispatch loop will deliver right after. Telnet active
         * sessions intentionally keep the WM_CHAR pass-through so a
         * user could type the layout's native characters into a server
         * that understands them. */
        if (g_mode == F6_SHELL) {
            BOOL shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            BOOL caps  = (GetKeyState(VK_CAPITAL) & 1) != 0;
            int  ch    = 0;
            if (vk >= 'A' && vk <= 'Z') {
                BOOL upper = (shift != caps);
                ch = upper ? vk : (vk + 32);
            } else if (vk >= '0' && vk <= '9') {
                if (!shift) ch = vk;
            } else if (vk == VK_SPACE) {
                ch = ' ';
            }
            if (ch) {
                handle_char_shell(ch);
                g_swallow_next_char = TRUE;
                return 0;
            }
        }
        break;
    }

    case WM_KEYUP: {
        int vk = (int)wParam;
        char tok[16];
        _snprintf(tok, sizeof(tok), "vk^%X", vk);
        tok[sizeof(tok) - 1] = '\0';
        key_log_push(tok);
        render_status_line();
        break;
    }

    case WM_CHAR: {
        int ch = (int)wParam;
        char tok[16];
        if (ch >= 0x20 && ch < 0x7F)
            _snprintf(tok, sizeof(tok), "ch=%c", (char)ch);
        else
            _snprintf(tok, sizeof(tok), "ch=%X", ch);
        tok[sizeof(tok) - 1] = '\0';
        key_log_push(tok);
        render_status_line();
        /* Polish 5: if WM_KEYDOWN already fed this keystroke into the
         * shell via the VK path, drop the layout-translated duplicate
         * that the dispatch loop generates from the same key. */
        if (g_swallow_next_char) {
            g_swallow_next_char = FALSE;
            return 0;
        }
        if (g_mode == F6_SHELL) {
            /* Letters/digits/space are handled at WM_KEYDOWN; this path
             * still fires for Enter (0x0D), Backspace (0x08), Ctrl-L
             * (0x0C), Ctrl-C (0x03), and any layout-emitted punctuation
             * that happens to be ASCII. handle_char_shell filters. */
            handle_char_shell(ch);
        } else if (g_mode == F6_TELNET_ACTIVE) {
            handle_char_telnet(ch);
        }
        /* F6_TELNET_CONNECTING / F6_FINGER_READING / F6_QOTD_READING:
         * ignore keystrokes. */
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int notches = delta / WHEEL_DELTA;
        if (notches == 0) notches = delta > 0 ? 1 : -1;
        handle_wheel(notches);
        return 0;
    }

    case WM_TELNET_NOTIFY:
        switch (wParam) {
        case TELNET_EVT_CONNECTED: on_evt_connected();                       return 0;
        case TELNET_EVT_DATA:      on_evt_data((TelnetDataChunk *)lParam);   return 0;
        case TELNET_EVT_ERROR:     on_evt_error((const char *)lParam);       return 0;
        case TELNET_EVT_CLOSED:    on_evt_closed();                          return 0;
        }
        return 0;

    case WM_USER_FOCUS_RENDER:
        SetFocus(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER_CURSOR_BLINK);
        cancel_macro_timer();
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
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
    wc.lpszClassName = TELNET_RENDER_CLASS;
    RegisterClassExA(&wc);
    g_render_class_reg = TRUE;
}

void telnet_module_init(void)
{
    /* Real allocation happens at first activate. */
}

void telnet_module_shutdown(void)
{
    if (g_active_telnet) { telnet_proto_close(g_active_telnet); g_active_telnet = NULL; }
    if (g_active_finger) { finger_proto_close(g_active_finger); g_active_finger = NULL; }
    if (g_active_qotd)   { qotd_proto_close  (g_active_qotd);   g_active_qotd   = NULL; }
    if (g_term_inited)   { vt_term_free(&g_term); g_term_inited = FALSE; }
    if (g_hFontTelnet)   { DeleteObject(g_hFontTelnet); g_hFontTelnet = NULL; }
    if (g_hBrushTermBg)  { DeleteObject(g_hBrushTermBg); g_hBrushTermBg = NULL; }
    if (g_hBrushTermFg)  { DeleteObject(g_hBrushTermFg); g_hBrushTermFg = NULL; }
}

void telnet_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    g_hContent = content;

    if (g_controls_created) {
        ShowWindow(g_hBtnTelnet,     SW_SHOW);
        ShowWindow(g_hBtnFingerNews, SW_SHOW);
        ShowWindow(g_hBtnFingerWx,   SW_SHOW);
        ShowWindow(g_hBtnQotd,       SW_SHOW);
        ShowWindow(g_hBtnArchie,     SW_SHOW);
        ShowWindow(g_hRender,        SW_SHOW);
        GetClientRect(content, &rc);
        telnet_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);
        PostMessageA(g_hRender, WM_USER_FOCUS_RENDER, 0, 0);
        return;
    }

    if (!g_hFontTelnet) {
        /* Dispatch amendment 2026-06-15 (item 4): Glass TTY retired in
         * favor of Cascadia Mono, which ships with Windows 11 and is
         * Dimitri's established SecureCRT terminal face on the unicorn
         * server. Size is 12 pt at 96 DPI (-MulDiv(12, dpi, 72) gives
         * the negative height GDI expects). If Cascadia Mono is missing
         * (older Windows / pre-2020 update), fall back to Consolas, then
         * to the generic FIXED_PITCH | FF_MODERN default. */
        HDC dc = GetDC(NULL);
        int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
        int height_12pt;
        if (dc) ReleaseDC(NULL, dc);
        if (dpi <= 0) dpi = 96;
        height_12pt = -MulDiv(12, dpi, 72);
        g_hFontTelnet = CreateFontA(height_12pt, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Cascadia Mono");
        if (!g_hFontTelnet)
            g_hFontTelnet = CreateFontA(height_12pt, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Consolas");
        if (!g_hFontTelnet)
            g_hFontTelnet = CreateFontA(height_12pt, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH | FF_MODERN, NULL);
    }
    if (!g_hBrushTermBg) g_hBrushTermBg = CreateSolidBrush(TELNET_COL_BG);
    if (!g_hBrushTermFg) g_hBrushTermFg = CreateSolidBrush(TELNET_COL_FG);

    register_render_class(hInst);

    /* Dispatch amendment 2026-06-15-3 (item 1): use the stock
     * Windows BS_PUSHBUTTON theme to match F3 Gopher's toolbar
     * (gopher_module.c:1998-2023 uses the same style). Drops the
     * owner-draw + subclass machinery this file used to carry; the
     * standard theme handles enabled / hover / pressed / disabled
     * rendering, gives Dimitri the look he asked for at the visual
     * gate, and lets us delete paint_macro_button entirely. */
    /* Shortcut bookmarks. The F9-F12 accelerator hints were dropped from
     * the captions on 2026-07-24 when the suite retired the F1-F12
     * accelerator table (the retro section ran out of function keys); the
     * buttons are click-only now. Archie opens the interactive Archie
     * service at archie.greektimes.ca:2323 (port 23 there is Telnet News,
     * so 2323 is explicit). */
    g_hBtnTelnet = CreateWindowA("BUTTON", "Telnet News",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 100, 36, content,
        (HMENU)(INT_PTR)IDC_TELNET_BTN_TELNET, hInst, NULL);
    g_hBtnFingerNews = CreateWindowA("BUTTON", "Finger News",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 100, 36, content,
        (HMENU)(INT_PTR)IDC_TELNET_BTN_FINGER_N, hInst, NULL);
    g_hBtnFingerWx = CreateWindowA("BUTTON", "Finger Weather",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 100, 36, content,
        (HMENU)(INT_PTR)IDC_TELNET_BTN_FINGER_W, hInst, NULL);
    g_hBtnQotd = CreateWindowA("BUTTON", "QOTD",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 100, 36, content,
        (HMENU)(INT_PTR)IDC_TELNET_BTN_QOTD, hInst, NULL);
    g_hBtnArchie = CreateWindowA("BUTTON", "Archie",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 100, 36, content,
        (HMENU)(INT_PTR)IDC_TELNET_BTN_ARCHIE, hInst, NULL);

    if (g_hFontUI) {
        SendMessageA(g_hBtnTelnet,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hBtnFingerNews, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hBtnFingerWx,   WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hBtnQotd,       WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hBtnArchie,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }

    /* Dispatch amendment 2026-06-15-2 (item 5): the "Local shell
     * [cols x rows CR=N LF=M KEY: ...]" diagnostic status line is
     * retired. The status STATIC is no longer created; render_status_line
     * and set_status_line are guarded by !g_hStatus and become no-ops,
     * which keeps the call sites compiling without an audit. The render
     * area grows upward into the reclaimed vertical space (see
     * telnet_module_resize). The status_base buffer + KEY: log are
     * still maintained internally as cheap diagnostics in case a
     * future build wants to wire them to a different surface (e.g.
     * the suite-wide status bar in Retro Mode). */
    g_hStatus = NULL;

    g_hRender = CreateWindowExA(WS_EX_CLIENTEDGE, TELNET_RENDER_CLASS, "",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 100, content,
        (HMENU)(INT_PTR)IDC_TELNET_RENDER, hInst, NULL);

    g_controls_created = TRUE;

    GetClientRect(content, &rc);
    telnet_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);

    if (!g_banner_emitted) emit_boot_banner();

    PostMessageA(g_hRender, WM_USER_FOCUS_RENDER, 0, 0);
}

void telnet_module_deactivate(HWND content)
{
    (void)content;
    if (!g_controls_created) return;
    ShowWindow(g_hBtnTelnet,     SW_HIDE);
    ShowWindow(g_hBtnFingerNews, SW_HIDE);
    ShowWindow(g_hBtnFingerWx,   SW_HIDE);
    ShowWindow(g_hBtnQotd,       SW_HIDE);
    ShowWindow(g_hBtnArchie,     SW_HIDE);
    ShowWindow(g_hRender,        SW_HIDE);
}

void telnet_module_resize(HWND content, int w, int h)
{
    const int margin  = 16;
    const int btn_h   = 36;
    const int btn_gap = 8;
    const int bar_gap = 8;
    int btn_w;
    int y, x;
    int bar_y;
    int render_y;
    int render_h;

    (void)content;
    if (!g_controls_created) return;

    btn_w = (w - 2 * margin - 4 * btn_gap) / 5;
    if (btn_w < 80) btn_w = 80;

    /* Top: shortcut bar (Telnet News / Finger News / Finger Weather /
     * QOTD / Archie). Dispatch 2026-06-25: moved from the bottom of the
     * view to the top, directly below the tab row. */
    bar_y = margin;

    /* Terminal box fills the rest, shifted down by the bar height plus
     * the same 8 px gap the bar used to leave above it at the bottom.
     * render_h runs down to the bottom margin -- no overlap, no gap. */
    render_y = bar_y + btn_h + bar_gap;
    render_h = h - margin - render_y;
    if (render_h < 200) render_h = 200;
    MoveWindow(g_hRender, margin, render_y, w - 2 * margin, render_h, TRUE);

    x = margin;
    y = bar_y;
    MoveWindow(g_hBtnTelnet,     x, y, btn_w, btn_h, TRUE); x += btn_w + btn_gap;
    MoveWindow(g_hBtnFingerNews, x, y, btn_w, btn_h, TRUE); x += btn_w + btn_gap;
    MoveWindow(g_hBtnFingerWx,   x, y, btn_w, btn_h, TRUE); x += btn_w + btn_gap;
    MoveWindow(g_hBtnQotd,       x, y, btn_w, btn_h, TRUE); x += btn_w + btn_gap;
    MoveWindow(g_hBtnArchie,     x, y, btn_w, btn_h, TRUE);
}

/* Shared shortcut trigger for the macro-button clicks. `id` is one of the
 * IDC_TELNET_BTN_* button ids. Archie connects to the interactive Archie
 * service on port 2323 (a plain find/exact/prog/help/quit line protocol);
 * port 23 on that host is the Telnet News service, so 2323 is explicit. */
static void trigger_shortcut(int id)
{
    const char *cmd = NULL;
    switch (id) {
    case IDC_TELNET_BTN_TELNET:    cmd = "telnet telnet.greektimes.ca";        break;
    case IDC_TELNET_BTN_FINGER_N:  cmd = "finger news@finger.greektimes.ca";   break;
    case IDC_TELNET_BTN_FINGER_W:  cmd = "finger weather@finger.greektimes.ca"; break;
    case IDC_TELNET_BTN_QOTD:      cmd = "nc qotd.greektimes.ca 17";           break;
    case IDC_TELNET_BTN_ARCHIE:    cmd = "telnet archie.greektimes.ca:2323";   break;
    default: return;
    }
    /* Dispatch amendment 2026-06-15 (item 5): disable all four
     * shortcuts at click time. The connect path will keep them
     * disabled while the session is alive; enter_shell_mode (called
     * from on_evt_closed, on_evt_error -> closed, close_active_session,
     * and the open_*-failure branches) re-enables all four together
     * when the connection ends, so the UI never gets stuck. */
    enable_macro_buttons(FALSE);
    macro_inject(cmd);
    SetFocus(g_hRender);
}

BOOL telnet_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id   = LOWORD(wParam);
    int code = HIWORD(wParam);

    (void)content; (void)lParam;

    if (code == BN_CLICKED) {
        switch (id) {
        case IDC_TELNET_BTN_TELNET:
        case IDC_TELNET_BTN_FINGER_N:
        case IDC_TELNET_BTN_FINGER_W:
        case IDC_TELNET_BTN_QOTD:
        case IDC_TELNET_BTN_ARCHIE:
            trigger_shortcut(id);
            return TRUE;
        default:
            return FALSE;
        }
    }
    return FALSE;
}

BOOL telnet_module_has_unsaved(void)
{
    return FALSE;
}

