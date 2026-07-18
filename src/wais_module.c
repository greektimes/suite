/*
 * wais_module.c - WAIS Search module: GUI + Z39.50-1988 orchestration.
 *
 * UI matches the standalone MGTWAIS x64 client (docs/wais2.png and
 * docs/wais3.png) per user editorial direction: 3 control rows above a
 * single multi-line output pane. This deviates from BLUEPRINT 4.2 (no
 * separate list + view pane) and BLUEPRINT 4.3 (server/port/db are
 * editable with a Connect button, not hardwired).
 *
 * The orchestration drives the preserved freeWAIS-sf 2.2.14 protocol
 * code in src/wais/ directly inline (the Suite has no subprocess split
 * per BLUEPRINT 2 "Backend binary model"), mirroring the cli_search /
 * cli_fetch pattern from MGTWAIS/source/src/wais_cli_engine.c.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

/* win_platform.h MUST come first: it includes winsock2.h before windows.h. */
#include "win_platform.h"

#include "wais_module.h"
#include "suite_shell.h"

#include "wais/cdialect.h"
#include "wais/cutil.h"
#include "wais/zutil.h"
#include "wais/wprot.h"
#include "wais/wutil.h"
#include "wais/wmessage.h"
#include "wais/sockets.h"
#include "wais/ui.h"

/* ztype1.h (via ui.h) does `#define WORD "sw"` for the Z39.50-1988 type-code
 * system, which clobbers the Windows WORD typedef and breaks LOWORD/HIWORD
 * macros used in WM_COMMAND handling. Restore Windows definition; the
 * protocol .c files are compiled separately and never see windows.h. */
#undef WORD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* freeWAIS-sf transport uses g_wais_socket as the active socket. The
 * background probe writes to this directly so init_connection and
 * interpret_message can recv on it. */
extern SOCKET g_wais_socket;

/* Server bindings: defaults at startup. Editable via the Server/Port/
 * Database fields and the Connect button. */
#define WAIS_DEFAULT_HOST    "wais.greektimes.ca"
#define WAIS_DEFAULT_PORT    "210"
#define WAIS_DEFAULT_DB      "greektimes"
#define WAIS_MAX_DOCS        40
#define WAIS_CHARS_PER_PAGE  10000
#define WAIS_DOC_MAX_BYTES   (1024 * 1024)

/* Module-local control IDs (1000+). */
#define IDC_WAIS_SEARCH_EDIT     1001
#define IDC_WAIS_SEARCH_BTN      1002
#define IDC_WAIS_SERVER_EDIT     1003
#define IDC_WAIS_PORT_EDIT       1004
#define IDC_WAIS_DB_EDIT         1005
#define IDC_WAIS_CONNECT_BTN     1006
#define IDC_WAIS_BACK_BTN        1007
#define IDC_WAIS_FWD_BTN         1008
#define IDC_WAIS_DOC_EDIT        1009
#define IDC_WAIS_VIEW_BTN        1010
#define IDC_WAIS_OUTPUT          1011
#define IDC_WAIS_DISCONNECT_BTN  1012

/* Live control handles. */
static HWND g_w_search_lbl, g_w_search_edit, g_w_search_btn;
static HWND g_w_server_lbl, g_w_server_edit;
static HWND g_w_port_lbl,   g_w_port_edit;
static HWND g_w_db_lbl,     g_w_db_edit;
static HWND g_w_connect_btn, g_w_disconnect_btn;
static HWND g_w_proto_lbl;
static HWND g_w_back_btn, g_w_fwd_btn;
static HWND g_w_doc_lbl, g_w_doc_edit;
static HWND g_w_view_btn;
static HWND g_w_output;

/* UI session gate. The Z39.50-1988 wire is stateless (each Search/View
 * opens its own connection), but the UI exposes a Connect/Disconnect
 * session like ARPANET FTP-Mail. Pre-connect: only Server/Port/Database
 * /Connect reachable. Connected: Search/Doc/buttons/Disconnect reach-
 * able, Server/Port/Database/Connect locked. */
static BOOL g_w_connected = FALSE;

/* Subclasses. */
static WNDPROC g_orig_search_proc = NULL;
static WNDPROC g_orig_doc_proc    = NULL;
static WNDPROC g_orig_output_proc = NULL;
static WNDPROC g_orig_field_proc  = NULL;   /* shared by server/port/db */

/* True once the controls have been created. On subsequent activations we
 * just ShowWindow them back so all in-progress state (search query,
 * cached results, doc body, server/port/db values, scroll position)
 * survives module switches. */
static BOOL g_w_controls_created = FALSE;

/* Active server bindings (committed by Connect, used by Search/View). */
static char g_w_active_host[256];
static char g_w_active_port[16];
static char g_w_active_db[128];

/* Cached views and search response. */
static char *g_w_results_text  = NULL;   /* formatted results listing */
static char *g_w_document_text = NULL;   /* formatted document body  */
static int   g_w_current_view  = 0;      /* 1=results, 2=document */

static SearchResponseAPDU *g_w_response = NULL;
static WAISSearchResponse *g_w_info     = NULL;
static int                 g_w_count    = 0;

/* Forward declarations. */
static void wais_on_search(HWND content);
static void wais_on_view(HWND content);
static void wais_on_connect(HWND content);
static void wais_on_disconnect(HWND content);
static void wais_show_view(int which);
static void wais_set_output_text(const char *utf8);
static void wais_clear_results(void);
static void wais_status(HWND content, const char *text);
static void wais_set_connected_state(BOOL connected);

/* ------------------------------------------------------------------ */
/* Status line helper: set and flush so the user sees the transition. */
/* ------------------------------------------------------------------ */

static void wais_status(HWND content, const char *text)
{
    HWND main_window;
    suite_set_status(text);
    main_window = GetParent(content);
    if (main_window) UpdateWindow(main_window);
}

/* ------------------------------------------------------------------ */
/* Subclass: search edit. Enter triggers Search.                       */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK SearchEditSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_GETDLGCODE) {
        MSG *pmsg = (MSG *)l;
        LRESULT base = CallWindowProcA(g_orig_search_proc, h, m, w, l);
        if (pmsg && pmsg->message == WM_KEYDOWN &&
            (pmsg->wParam == VK_RETURN ||
             pmsg->wParam == VK_UP    || pmsg->wParam == VK_DOWN ||
             pmsg->wParam == VK_PRIOR || pmsg->wParam == VK_NEXT))
            return base | DLGC_WANTMESSAGE;
        return base;
    }
    if (m == WM_KEYDOWN) {
        switch (w) {
        case VK_RETURN: wais_on_search(GetParent(h)); return 0;
        /* Up/Down/PgUp/PgDn forward to the output pane so the user can
         * scroll the body without first clicking inside it. Left/Right
         * stay normal so the caret can move within the text field. */
        case VK_UP:    if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_LINEUP,   0); return 0;
        case VK_DOWN:  if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_LINEDOWN, 0); return 0;
        case VK_PRIOR: if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_PAGEUP,   0); return 0;
        case VK_NEXT:  if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_PAGEDOWN, 0); return 0;
        }
    }
    if (m == WM_CHAR && w == '\r') return 0;
    return CallWindowProcA(g_orig_search_proc, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Subclass: output edit. Arrow / page / home / end keys scroll the   */
/* view instead of moving the caret; caret is hidden on focus so the  */
/* output pane reads as a read-only viewer, not an editor.            */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK OutputSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_GETDLGCODE: {
        /* Claim arrow keys so the (now-bypassed) IsDialogMessage path
         * would never steal them even if the shell's focus check ever
         * fails. */
        LRESULT base = CallWindowProcA(g_orig_output_proc, h, m, w, l);
        return base | DLGC_WANTARROWS;
    }
    case WM_SETFOCUS: {
        LRESULT r = CallWindowProcA(g_orig_output_proc, h, m, w, l);
        HideCaret(h);
        return r;
    }
    case WM_LBUTTONDOWN: {
        LRESULT r = CallWindowProcA(g_orig_output_proc, h, m, w, l);
        HideCaret(h);
        return r;
    }
    case WM_KEYDOWN:
        switch (w) {
        case VK_UP:    SendMessageA(h, WM_VSCROLL, SB_LINEUP,    0); return 0;
        case VK_DOWN:  SendMessageA(h, WM_VSCROLL, SB_LINEDOWN,  0); return 0;
        case VK_PRIOR: SendMessageA(h, WM_VSCROLL, SB_PAGEUP,    0); return 0;
        case VK_NEXT:  SendMessageA(h, WM_VSCROLL, SB_PAGEDOWN,  0); return 0;
        case VK_HOME:  SendMessageA(h, WM_VSCROLL, SB_TOP,       0); return 0;
        case VK_END:   SendMessageA(h, WM_VSCROLL, SB_BOTTOM,    0); return 0;
        case VK_LEFT:  SendMessageA(h, WM_HSCROLL, SB_LINELEFT,  0); return 0;
        case VK_RIGHT: SendMessageA(h, WM_HSCROLL, SB_LINERIGHT, 0); return 0;
        }
        break;
    case WM_CONTEXTMENU: {
        /* Replace the default edit context menu (which lists Cut/Copy/
         * Paste/Delete/Select All, with Cut/Paste/Delete grayed out on
         * a read-only edit) with a clean two-item viewer menu. Paste is
         * never offered, even as a disabled item. */
        HMENU menu = CreatePopupMenu();
        DWORD sel_start = 0, sel_end = 0;
        BOOL has_sel;
        POINT pt;
        int cmd;

        SendMessageA(h, EM_GETSEL, (WPARAM)&sel_start, (LPARAM)&sel_end);
        has_sel = (sel_end > sel_start);

        AppendMenuA(menu, MF_STRING | (has_sel ? 0 : MF_GRAYED), 1, "Copy\tCtrl+C");
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(menu, MF_STRING, 2, "Select All\tCtrl+A");

        pt.x = (int)(short)LOWORD(l);
        pt.y = (int)(short)HIWORD(l);
        if (pt.x == -1 && pt.y == -1) {
            /* Keyboard-invoked (Shift+F10): position near the control. */
            RECT rc;
            GetWindowRect(h, &rc);
            pt.x = rc.left + 40;
            pt.y = rc.top + 40;
        }
        cmd = TrackPopupMenu(menu,
                             TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_TOPALIGN | TPM_LEFTALIGN,
                             pt.x, pt.y, 0, h, NULL);
        DestroyMenu(menu);

        if (cmd == 1) SendMessageA(h, WM_COPY, 0, 0);
        else if (cmd == 2) SendMessageA(h, EM_SETSEL, 0, (LPARAM)-1);
        return 0;
    }
    }
    return CallWindowProcA(g_orig_output_proc, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Subclass: generic field edit. Up/Down/PgUp/PgDn forward to the      */
/* output pane's vertical scroll so the user can scroll the body       */
/* without leaving a Server/Port/Database field. Left/Right pass       */
/* through to the default proc for in-field cursor movement.           */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK FieldScrollSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_GETDLGCODE) {
        MSG *pmsg = (MSG *)l;
        LRESULT base = CallWindowProcA(g_orig_field_proc, h, m, w, l);
        if (pmsg && pmsg->message == WM_KEYDOWN &&
            (pmsg->wParam == VK_UP    || pmsg->wParam == VK_DOWN ||
             pmsg->wParam == VK_PRIOR || pmsg->wParam == VK_NEXT))
            return base | DLGC_WANTMESSAGE;
        return base;
    }
    if (m == WM_KEYDOWN) {
        switch (w) {
        case VK_UP:    if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_LINEUP,   0); return 0;
        case VK_DOWN:  if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_LINEDOWN, 0); return 0;
        case VK_PRIOR: if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_PAGEUP,   0); return 0;
        case VK_NEXT:  if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_PAGEDOWN, 0); return 0;
        }
    }
    return CallWindowProcA(g_orig_field_proc, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Subclass: doc # edit. Enter triggers View.                          */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK DocEditSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_GETDLGCODE) {
        MSG *pmsg = (MSG *)l;
        LRESULT base = CallWindowProcA(g_orig_doc_proc, h, m, w, l);
        if (pmsg && pmsg->message == WM_KEYDOWN &&
            (pmsg->wParam == VK_RETURN ||
             pmsg->wParam == VK_UP    || pmsg->wParam == VK_DOWN ||
             pmsg->wParam == VK_PRIOR || pmsg->wParam == VK_NEXT))
            return base | DLGC_WANTMESSAGE;
        return base;
    }
    if (m == WM_KEYDOWN) {
        switch (w) {
        case VK_RETURN: wais_on_view(GetParent(h)); return 0;
        case VK_UP:    if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_LINEUP,   0); return 0;
        case VK_DOWN:  if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_LINEDOWN, 0); return 0;
        case VK_PRIOR: if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_PAGEUP,   0); return 0;
        case VK_NEXT:  if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_PAGEDOWN, 0); return 0;
        }
    }
    if (m == WM_CHAR && w == '\r') return 0;
    return CallWindowProcA(g_orig_doc_proc, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Free cached state.                                                  */
/* ------------------------------------------------------------------ */

static void wais_clear_results(void)
{
    if (g_w_response) {
        freeWAISSearchResponse(g_w_response->DatabaseDiagnosticRecords);
        freeSearchResponseAPDU(g_w_response);
        g_w_response = NULL;
        g_w_info     = NULL;
    }
    g_w_count = 0;
    if (g_w_results_text)  { free(g_w_results_text);  g_w_results_text  = NULL; }
    if (g_w_document_text) { free(g_w_document_text); g_w_document_text = NULL; }
    g_w_current_view = 0;
    if (g_w_output) SetWindowTextA(g_w_output, "");
}

/* ------------------------------------------------------------------ */
/* Render the formatted output: convert UTF-8 to wide, replace \x01    */
/* markers with U+2588 (FULL BLOCK), and SetWindowTextW.               */
/* ------------------------------------------------------------------ */

static void wais_set_output_text(const char *utf8)
{
    int      src_len, wide_len;
    wchar_t *wide;
    int      i;

    if (!utf8 || !*utf8) { SetWindowTextW(g_w_output, L""); return; }

    src_len = (int)strlen(utf8);
    wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8, src_len, NULL, 0);
    if (wide_len <= 0) { SetWindowTextA(g_w_output, utf8); return; }

    /* Reserve 2 extra wchars for the leading blank line ("\r\n") that
     * gives the body one full line of breathing room from the top edge
     * of the output pane, plus 1 for the NUL terminator. */
    wide = (wchar_t *)malloc((size_t)(wide_len + 3) * sizeof(wchar_t));
    if (!wide) { SetWindowTextA(g_w_output, utf8); return; }

    wide[0] = L'\r';
    wide[1] = L'\n';
    MultiByteToWideChar(CP_UTF8, 0, utf8, src_len, wide + 2, wide_len);
    wide[wide_len + 2] = L'\0';

    /* Replace \x01 markers with U+2588 FULL BLOCK for score bar render. */
    for (i = 2; i < wide_len + 2; i++) {
        if (wide[i] == 0x01) wide[i] = 0x2588;
    }

    SetWindowTextW(g_w_output, wide);
    free(wide);

    /* Scroll the view to the top so the first line is visible after a
     * fresh text load (defaults to bottom or partial first-line clip
     * under some themes). EM_SETSEL + EM_SCROLLCARET puts the caret at
     * offset 0 and scrolls accordingly. */
    SendMessageA(g_w_output, EM_SETSEL, 0, 0);
    SendMessageA(g_w_output, EM_SCROLLCARET, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Switch the output pane between cached Results and Document views.   */
/* ------------------------------------------------------------------ */

static void wais_show_view(int which)
{
    if (which == 1 && g_w_results_text) {
        wais_set_output_text(g_w_results_text);
        g_w_current_view = 1;
    } else if (which == 2 && g_w_document_text) {
        wais_set_output_text(g_w_document_text);
        g_w_current_view = 2;
    }
}

/* ------------------------------------------------------------------ */
/* Connect: commit edits from server/port/db fields to active state.  */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Toggle the UI between pre-connect and connected states. Mirrors    */
/* the ARPANET FTP-Mail session gate. Pre-connect: only Server, Port, */
/* Database, Connect reachable. Connected: everything else reachable, */
/* Server/Port/Database/Connect locked.                                */
/* ------------------------------------------------------------------ */

static void wais_set_connected_state(BOOL connected)
{
    g_w_connected = connected;

    EnableWindow(g_w_server_edit,    !connected);
    EnableWindow(g_w_port_edit,      !connected);
    EnableWindow(g_w_db_edit,        !connected);
    EnableWindow(g_w_connect_btn,    !connected);

    EnableWindow(g_w_disconnect_btn,  connected);
    EnableWindow(g_w_search_edit,     connected);
    EnableWindow(g_w_search_btn,      connected);
    EnableWindow(g_w_doc_edit,        connected);
    EnableWindow(g_w_view_btn,        connected);
    EnableWindow(g_w_back_btn,        connected);
    EnableWindow(g_w_fwd_btn,         connected);
}

/* ------------------------------------------------------------------ */
/* Backgrounded Connect probe — implementation infrastructure.        */
/*                                                                     */
/* The freeWAIS-sf 2.2.x recv path is blocking with no timeouts. If a */
/* non-WAIS service (e.g. Google's HTTP front end on port 80) accepts */
/* the TCP connection but never speaks Z39.50-1988, the UI thread     */
/* would hang for minutes on the Init recv. The fix is twofold:        */
/*  (1) all probe network I/O runs on a worker thread, never the UI   */
/*      thread; the UI stays responsive throughout.                    */
/*  (2) every socket operation has a bounded timeout:                  */
/*      - TCP connect: WAIS_TIMEOUT_SECONDS (non-blocking + select).  */
/*      - Init recv:   WAIS_TIMEOUT_SECONDS (via SO_RCVTIMEO).         */
/*      - Search recv: WAIS_TIMEOUT_SECONDS (via SO_RCVTIMEO).         */
/* Cumulative max ~9s, well under the "about 8 seconds" backstop.     */
/*                                                                     */
/* Communication: worker writes outcome + status_msg into a heap-     */
/* allocated WAISProbeCtx, then InterlockedExchange's the `done` flag */
/* to 1. The UI thread polls via a SetTimer callback at 100ms and     */
/* processes the result when done == 1, then frees the ctx.            */
/* ------------------------------------------------------------------ */

#define WAIS_TIMEOUT_SECONDS  3
#define WAIS_PROBE_TIMER_ID   0xA001

typedef struct WAISProbeCtx {
    HWND          content;
    char          host[256];
    int           port;
    char          db[128];

    /* Worker writes these, then InterlockedExchange's `done` to 1.   */
    volatile LONG done;
    int           outcome;        /* 0 = connected; nonzero = failure */
    char          status_msg[400];
} WAISProbeCtx;

static WAISProbeCtx *g_w_probe          = NULL;
static UINT_PTR      g_w_probe_timer_id = 0;

/* TCP connect with bounded timeout. Returns INVALID_SOCKET on any
 * failure (refused, timed out, DNS). Same shape as arpa_connect_timeout
 * in the ARPANET module. */
static SOCKET wais_connect_timeout(const char *host, int port, int seconds)
{
    SOCKET             s;
    struct sockaddr_in addr;
    struct hostent    *he;
    unsigned long      a;
    u_long             mode;
    int                rc;
    fd_set             wfds, efds;
    struct timeval     tv;
    int                err = 0, errlen = sizeof(err), sel;

    memset(&addr, 0, sizeof(addr));
    a = inet_addr(host);
    if (a != INADDR_NONE) {
        addr.sin_addr.s_addr = a;
        addr.sin_family      = AF_INET;
    } else {
        he = gethostbyname(host);
        if (!he) return INVALID_SOCKET;
        addr.sin_family = he->h_addrtype;
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    addr.sin_port = htons((unsigned short)port);

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    rc = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (rc != 0) {
        FD_ZERO(&wfds); FD_SET(s, &wfds);
        FD_ZERO(&efds); FD_SET(s, &efds);
        tv.tv_sec  = seconds;
        tv.tv_usec = 0;
        sel = select(0, NULL, &wfds, &efds, &tv);
        if (sel <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen) != 0 || err != 0) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    mode = 0;
    ioctlsocket(s, FIONBIO, &mode);
    return s;
}

/* Worker thread: runs the three-stage probe. ZERO UI access from here;
 * results go through ctx, polled by the UI thread. */
static DWORD WINAPI wais_probe_worker(LPVOID arg)
{
    WAISProbeCtx       *ctx        = (WAISProbeCtx *)arg;
    SOCKET              s          = INVALID_SOCKET;
    char               *req        = NULL, *resp = NULL;
    long                msglen     = BUFSZ, reqlen;
    long                init_result;
    char                userInfo[500], hostname[80];
    DWORD               tmo_ms     = WAIS_TIMEOUT_SECONDS * 1000;
    SearchResponseAPDU *response   = NULL;

    /* Stage (a): TCP connect with bounded timeout. */
    s = wais_connect_timeout(ctx->host, ctx->port, WAIS_TIMEOUT_SECONDS);
    if (s == INVALID_SOCKET) {
        ctx->outcome = 1;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "Cannot connect to %s:%d.", ctx->host, ctx->port);
        goto done;
    }

    /* All recvs from here on are bounded by SO_RCVTIMEO. */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo_ms, sizeof(tmo_ms));

    /* Wire into freeWAIS-sf: init_connection and interpret_message read
     * g_wais_socket internally. The FILE* arg is ignored by the Win
     * port (see sockets_win.c). */
    g_wais_socket = s;

    req  = (char *)s_malloc((size_t)msglen);
    resp = (char *)s_malloc((size_t)msglen);
    if (!req || !resp) {
        ctx->outcome = 4;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg), "Out of memory.");
        goto cleanup;
    }

    mygethostname(hostname, 80);
    _snprintf(userInfo, sizeof(userInfo),
              "MGT-Unicorn-Suite/0.1.0-mvp %s", hostname);

    /* Stage (b): Init exchange. recv is bounded by SO_RCVTIMEO; if the
     * peer never speaks (HTTP front end, etc.), init_connection returns
     * a non-positive result after ~WAIS_TIMEOUT_SECONDS. */
    init_result = init_connection(req, resp, msglen, stderr, userInfo);
    if (init_result <= 0) {
        ctx->outcome = 2;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "%s:%d does not speak WAIS Z39.50-1988.",
                  ctx->host, ctx->port);
        goto cleanup;
    }

    /* Stage (c): probe Search against the typed database. Single-letter
     * benign query, 1-record limit. Branch on SearchResponseAPDU
     * .SearchStatus per zprot.h: TRUE = db accepted (regardless of hit
     * count); FALSE / no-response / parse-fail = db invalid. */
    reqlen = msglen;
    if (!generate_search_apdu(req + HEADER_LENGTH, &reqlen,
                              "x", ctx->db, NULL, (long)1)) {
        ctx->outcome = 4;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "Failed to build probe Search Request.");
        goto cleanup;
    }
    if (interpret_message(req, msglen - reqlen, resp, msglen,
                          stderr, (boolean)false) == 0) {
        ctx->outcome = 3;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "Database \"%s\" not found on %s:%d.",
                  ctx->db, ctx->host, ctx->port);
        goto cleanup;
    }

    readSearchResponseAPDU(&response, resp + HEADER_LENGTH);
    if (response && response->SearchStatus) {
        ctx->outcome = 0;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "Connected to %s:%d, database \"%s\".",
                  ctx->host, ctx->port, ctx->db);
    } else {
        ctx->outcome = 3;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "Database \"%s\" not found on %s:%d.",
                  ctx->db, ctx->host, ctx->port);
    }
    if (response) freeSearchResponseAPDU(response);

cleanup:
    if (req)  s_free(req);
    if (resp) s_free(resp);
    if (s != INVALID_SOCKET) closesocket(s);
    g_wais_socket = INVALID_SOCKET;

done:
    InterlockedExchange(&ctx->done, 1);
    return 0;
}

/* UI-thread polling timer: triggers on a 100ms cadence while a probe
 * is in flight. When the worker has set done = 1, this fires the UI
 * transition exactly once and frees the ctx. */
static void CALLBACK wais_probe_timer_proc(HWND hwnd, UINT msg,
                                           UINT_PTR id, DWORD time)
{
    WAISProbeCtx *ctx;
    (void)msg; (void)time;

    if (!g_w_probe || !g_w_probe->done) return;

    KillTimer(hwnd, id);
    g_w_probe_timer_id = 0;

    ctx = g_w_probe;
    g_w_probe = NULL;

    wais_status(ctx->content, ctx->status_msg);

    if (ctx->outcome == 0) {
        wais_set_connected_state(TRUE);
        SetFocus(g_w_search_edit);
    } else {
        /* Failure path: stay pre-connect, re-enable Connect button. */
        EnableWindow(g_w_connect_btn, TRUE);
    }

    free(ctx);
}

/* ------------------------------------------------------------------ */
/* Connect: three-stage reachability + validity probe.                */
/*                                                                     */
/*  (a) TCP connect to server:port. On failure: stay pre-connect,     */
/*      local message "Cannot connect to <server>:<port>."             */
/*  (b) Z39.50-1988 Init exchange. On failure: stay pre-connect,      */
/*      local message "<server>:<port> does not speak WAIS            */
/*      Z39.50-1988."                                                  */
/*  (c) Probe Search Request against the typed database with a benign */
/*      single-letter query and a 1-record limit. If the server       */
/*      returns no response, returns SearchStatus = FALSE, or fails    */
/*      to parse, the database is invalid: stay pre-connect, local    */
/*      message "Database \"<db>\" not found on <server>:<port>."     */
/*      Z39.50-1988 carries no database name in Init, so a real        */
/*      Search exchange is the only honest way to validate the db.    */
/*      A valid database with zero hits still reads as connected      */
/*      because the server returns a well-formed SearchResponse with  */
/*      SearchStatus = TRUE and ResultCount = 0.                       */
/*                                                                     */
/* Only after all three stages pass does the UI flip to connected.    */
/* The probe socket closes immediately; Search and View still open    */
/* their own per-operation sockets, same stateless pattern as the     */
/* ARPANET FTP-Mail module.                                            */
/* ------------------------------------------------------------------ */

static void wais_on_connect(HWND content)
{
    char          host[256], port[16], db[128];
    int           port_int;
    HANDLE        thr;
    WAISProbeCtx *ctx;

    /* Single probe in flight at a time. The Connect button is also */
    /* disabled below; this is the belt to that suspenders.          */
    if (g_w_probe) return;

    GetWindowTextA(g_w_server_edit, host, sizeof(host));
    GetWindowTextA(g_w_port_edit,   port, sizeof(port));
    GetWindowTextA(g_w_db_edit,     db,   sizeof(db));

    if (!host[0] || !port[0] || !db[0]) {
        wais_status(content, "Server, port, and database are required.");
        return;
    }
    port_int = atoi(port);
    if (port_int <= 0 || port_int > 65535) {
        wais_status(content, "Port must be 1-65535.");
        return;
    }

    strncpy(g_w_active_host, host, sizeof(g_w_active_host) - 1);
    strncpy(g_w_active_port, port, sizeof(g_w_active_port) - 1);
    strncpy(g_w_active_db,   db,   sizeof(g_w_active_db)   - 1);
    g_w_active_host[sizeof(g_w_active_host)-1] = '\0';
    g_w_active_port[sizeof(g_w_active_port)-1] = '\0';
    g_w_active_db[sizeof(g_w_active_db)-1]     = '\0';

    ctx = (WAISProbeCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        wais_status(content, "Out of memory.");
        return;
    }
    ctx->content = content;
    ctx->port    = port_int;
    strncpy(ctx->host, g_w_active_host, sizeof(ctx->host) - 1);
    strncpy(ctx->db,   g_w_active_db,   sizeof(ctx->db)   - 1);

    g_w_probe = ctx;

    /* Lock Connect and show the pre-banner transient. The UI stays
     * fully responsive (the worker runs on its own thread). */
    EnableWindow(g_w_connect_btn, FALSE);
    wais_status(content, "Connecting...");

    thr = CreateThread(NULL, 0, wais_probe_worker, ctx, 0, NULL);
    if (!thr) {
        wais_status(content, "Cannot start probe thread.");
        EnableWindow(g_w_connect_btn, TRUE);
        free(ctx);
        g_w_probe = NULL;
        return;
    }
    CloseHandle(thr);   /* detach; worker runs to completion on its own */

    /* Poll the worker's done flag at 100ms; the timer proc handles UI
     * transition exactly once when the worker finishes. */
    g_w_probe_timer_id = SetTimer(content, WAIS_PROBE_TIMER_ID,
                                  100, wais_probe_timer_proc);
}

/* ------------------------------------------------------------------ */
/* Disconnect: stateless wire (nothing to QUIT in Z39.50-1988), so    */
/* this is purely a UI reset. Clears search/Doc#/output pane and any  */
/* cached search/document state. Returns the UI to pre-connect with   */
/* Server/Port/Database editable again.                                */
/* ------------------------------------------------------------------ */

static void wais_on_disconnect(HWND content)
{
    SetWindowTextA(g_w_search_edit, "");
    SetWindowTextA(g_w_doc_edit,    "");
    SetWindowTextA(g_w_output,      "");

    wais_clear_results();
    g_w_current_view = 0;

    wais_status(content, "Disconnected.");
    wais_set_connected_state(FALSE);
    SetFocus(g_w_server_edit);
}

/* ------------------------------------------------------------------ */
/* Search: open Z39.50-1988, send Search APDU, parse, render results. */
/* ------------------------------------------------------------------ */

static void wais_on_search(HWND content)
{
    char       query[1024];
    char      *req = NULL, *resp = NULL;
    long       msglen = BUFSZ, reqlen;
    FILE      *conn = NULL;
    char       userInfo[500];
    char       hostname[80];
    long       init_result;
    SearchResponseAPDU *response = NULL;
    WAISSearchResponse *info;
    long       i;
    char       line[1024];
    char      *out_buf = NULL;
    size_t     out_cap = 0, out_len = 0;
    char       status[96];
    int        port_int;

    GetWindowTextA(g_w_search_edit, query, sizeof(query));
    if (!query[0]) {
        wais_status(content, "Enter a query and press Search.");
        return;
    }

    wais_clear_results();

    /* Pre-allocate output buffer for the results listing. */
    out_cap = 8192;
    out_buf = (char *)malloc(out_cap);
    if (!out_buf) {
        wais_status(content, "Out of memory.");
        return;
    }
    out_buf[0] = '\0';

    wais_status(content, "Connecting...");

    req  = (char *)s_malloc((size_t)msglen);
    resp = (char *)s_malloc((size_t)msglen);
    if (!req || !resp) {
        wais_status(content, "Out of memory.");
        if (req)  s_free(req);
        if (resp) s_free(resp);
        free(out_buf);
        return;
    }

    port_int = atoi(g_w_active_port);
    if (port_int <= 0) port_int = 210;

    conn = connect_to_server(g_w_active_host, (long)port_int);
    if (!conn) {
        _snprintf(status, sizeof(status), "Cannot connect to %s:%d.",
                  g_w_active_host, port_int);
        wais_status(content, status);
        s_free(req); s_free(resp); free(out_buf);
        return;
    }

    mygethostname(hostname, 80);
    _snprintf(userInfo, sizeof(userInfo),
              "MGT-Unicorn-Suite/0.1.0-mvp %s", hostname);

    wais_status(content, "Initializing Z39.50-1988...");
    init_result = init_connection(req, resp, msglen, conn, userInfo);
    if (init_result <= 0) {
        wais_status(content, "Z39.50-1988 Init failed.");
        close_connection(conn);
        s_free(req); s_free(resp); free(out_buf);
        return;
    }

    wais_status(content, "Searching...");

    reqlen = msglen;
    if (!generate_search_apdu(req + HEADER_LENGTH, &reqlen,
                              query, g_w_active_db, NULL, (long)WAIS_MAX_DOCS)) {
        wais_status(content, "Failed to generate Search APDU.");
        close_connection(conn);
        s_free(req); s_free(resp); free(out_buf);
        return;
    }

    if (interpret_message(req, msglen - reqlen, resp, msglen,
                          conn, (boolean)false) == 0) {
        wais_status(content, "No response from server.");
        close_connection(conn);
        s_free(req); s_free(resp); free(out_buf);
        return;
    }

    readSearchResponseAPDU(&response, resp + HEADER_LENGTH);

    g_w_response = response;
    g_w_info = response
               ? (WAISSearchResponse *)response->DatabaseDiagnosticRecords
               : NULL;
    info = g_w_info;

    /* Compose the formatted results listing. */
    _snprintf(line, sizeof(line),
              "Search Response:\r\n  Number of Records Returned: %d\r\n\r\n",
              response ? response->NumberOfRecordsReturned : 0);
    {
        size_t need = strlen(line) + 1;
        if (need > out_cap) {
            char *nb = (char *)realloc(out_buf, need + 8192);
            if (nb) { out_buf = nb; out_cap = need + 8192; }
        }
        strcpy(out_buf, line);
        out_len = strlen(out_buf);
    }

    g_w_count = 0;
    if (info && info->DocHeaders) {
        i = 0;
        while (info->DocHeaders[i] != 0) {
            char *headline = info->DocHeaders[i]->Headline
                             ? trim_junk(info->DocHeaders[i]->Headline)
                             : NULL;
            long score = info->DocHeaders[i]->Score;
            int bars, j;
            char bar[12];
            size_t need;

            /* Match the standalone's FormatResults clamp exactly: raw
             * score clamped to [0,9]. Real WAIS scores are usually well
             * above 9, so most results render with the full 9-block bar. */
            if (score < 0) score = 0;
            bars = (int)score;
            if (bars > 9) bars = 9;
            for (j = 0; j < bars; j++)  bar[j] = '\x01';
            for (; j < 9; j++)          bar[j] = ' ';
            bar[9] = '\0';

            _snprintf(line, sizeof(line),
                      "%2ld: [%s]  %s\r\n",
                      i + 1, bar,
                      headline && *headline ? headline : "(no title)");

            need = out_len + strlen(line) + 1;
            if (need > out_cap) {
                size_t new_cap = out_cap * 2;
                char *nb;
                while (new_cap < need) new_cap *= 2;
                nb = (char *)realloc(out_buf, new_cap);
                if (!nb) break;
                out_buf = nb; out_cap = new_cap;
            }
            strcpy(out_buf + out_len, line);
            out_len += strlen(line);
            i++;
        }
        g_w_count = (int)i;
    }

    close_connection(conn);
    s_free(req); s_free(resp);

    g_w_results_text = out_buf;
    wais_show_view(1);

    /* Doc# always stays blank; user types the desired document number
     * manually. No auto-fill, even after a successful search. */
    SetWindowTextA(g_w_doc_edit, "");
    if (g_w_count > 0)
        _snprintf(status, sizeof(status), "%d results", g_w_count);
    else
        _snprintf(status, sizeof(status), "0 results");
    wais_status(content, status);

    /* Return focus to the search edit so arrow keys keep working as
     * scroll forwards even when Search was triggered by button click. */
    SetFocus(g_w_search_edit);
}

/* ------------------------------------------------------------------ */
/* View: read Doc # entry, retrieve that document, render in output.  */
/* ------------------------------------------------------------------ */

static void wais_on_view(HWND content)
{
    char       doc_str[16];
    int        doc_num;
    any       *docID;
    long       doc_size;
    char      *type;
    char      *req = NULL, *resp = NULL;
    long       msglen = BUFSZ, reqlen;
    FILE      *conn = NULL;
    char       userInfo[500];
    char       hostname[80];
    long       init_result;
    long       count;
    char      *body = NULL;
    size_t     body_len = 0, body_cap = 0;
    boolean    skipped_separator = (boolean)false;
    SearchResponseAPDU *rr = NULL;
    WAISSearchResponse *ri;
    char       status[96];
    int        port_int;

    if (!g_w_info || !g_w_info->DocHeaders || g_w_count <= 0) {
        wais_status(content, "Run a search first.");
        return;
    }

    GetWindowTextA(g_w_doc_edit, doc_str, sizeof(doc_str));
    doc_num = atoi(doc_str);
    if (doc_num < 1 || doc_num > g_w_count) {
        _snprintf(status, sizeof(status),
                  "Enter a Doc # from 1 to %d.", g_w_count);
        wais_status(content, status);
        return;
    }

    docID    = g_w_info->DocHeaders[doc_num - 1]->DocumentID;
    doc_size = g_w_info->DocHeaders[doc_num - 1]->DocumentLength;
    type     = g_w_info->DocHeaders[doc_num - 1]->Types
               ? g_w_info->DocHeaders[doc_num - 1]->Types[0]
               : "TEXT";

    wais_status(content, "Loading...");

    req  = (char *)s_malloc((size_t)msglen);
    resp = (char *)s_malloc((size_t)msglen);
    if (!req || !resp) {
        wais_status(content, "Out of memory.");
        if (req)  s_free(req);
        if (resp) s_free(resp);
        return;
    }

    port_int = atoi(g_w_active_port);
    if (port_int <= 0) port_int = 210;

    conn = connect_to_server(g_w_active_host, (long)port_int);
    if (!conn) {
        _snprintf(status, sizeof(status),
                  "Cannot connect to %s:%d.",
                  g_w_active_host, port_int);
        wais_status(content, status);
        s_free(req); s_free(resp);
        return;
    }

    mygethostname(hostname, 80);
    _snprintf(userInfo, sizeof(userInfo),
              "MGT-Unicorn-Suite/0.1.0-mvp %s", hostname);

    init_result = init_connection(req, resp, msglen, conn, userInfo);
    if (init_result <= 0) {
        wais_status(content, "Z39.50-1988 Init failed.");
        close_connection(conn);
        s_free(req); s_free(resp);
        return;
    }

    body_cap = (doc_size > 0 && doc_size < WAIS_DOC_MAX_BYTES)
               ? (size_t)doc_size + 1024
               : 16384;
    body = (char *)malloc(body_cap);
    if (!body) {
        wais_status(content, "Out of memory.");
        close_connection(conn);
        s_free(req); s_free(resp);
        return;
    }
    body_len = 0;

    for (count = 0;
         (doc_size == 0) || (count * WAIS_CHARS_PER_PAGE < doc_size);
         count++)
    {
        long end_byte = (doc_size > 0)
                        ? MINIMUM((count + 1) * WAIS_CHARS_PER_PAGE, doc_size)
                        : (count + 1) * WAIS_CHARS_PER_PAGE;

        reqlen = msglen;
        if (!generate_retrieval_apdu(req + HEADER_LENGTH, &reqlen,
                                     docID, CT_byte,
                                     count * WAIS_CHARS_PER_PAGE, end_byte,
                                     type, g_w_active_db))
            break;

        if (interpret_message(req, msglen - reqlen, resp, msglen,
                              conn, (boolean)false) == 0)
            break;

        readSearchResponseAPDU(&rr, resp + HEADER_LENGTH);
        ri = rr ? (WAISSearchResponse *)rr->DatabaseDiagnosticRecords : NULL;

        if (ri && ri->Text) {
            WAISDocumentText *text = ri->Text[0];
            if (text && text->DocumentText) {
                char *bytes = text->DocumentText->bytes;
                long  size  = text->DocumentText->size;
                long  start = 0, j;

                if (count == 0 && !skipped_separator) {
                    /* Find the first "----" record-separator line. Keep it
                     * (start AT the dashes, not after) so the document's
                     * own |title|/----- header box renders as a full
                     * frame. Matches the standalone cli_engine pattern. */
                    for (j = 0; j < size - 4; j++) {
                        if (bytes[j]=='-' && bytes[j+1]=='-' &&
                            bytes[j+2]=='-' && bytes[j+3]=='-') {
                            start = j;
                            break;
                        }
                    }
                    skipped_separator = (boolean)true;
                }

                if (size > start) {
                    size_t add = (size_t)(size - start);
                    if (body_len + add + 1 > body_cap) {
                        size_t new_cap = body_cap * 2;
                        char *nb;
                        while (new_cap < body_len + add + 1) new_cap *= 2;
                        if (new_cap > WAIS_DOC_MAX_BYTES) new_cap = WAIS_DOC_MAX_BYTES;
                        if (body_len + add + 1 > new_cap) add = new_cap - body_len - 1;
                        nb = (char *)realloc(body, new_cap);
                        if (nb) { body = nb; body_cap = new_cap; } else break;
                    }
                    memcpy(body + body_len, bytes + start, add);
                    body_len += add;
                }
            }
        }

        if (rr) {
            freeWAISSearchResponse(rr->DatabaseDiagnosticRecords);
            freeSearchResponseAPDU(rr);
            rr = NULL;
        }

        if (doc_size == 0) break;
        if (body_len >= WAIS_DOC_MAX_BYTES) break;
    }

    if (rr) {
        freeWAISSearchResponse(rr->DatabaseDiagnosticRecords);
        freeSearchResponseAPDU(rr);
    }

    close_connection(conn);
    s_free(req); s_free(resp);

    /* Sanitize body: CRLF normalization, ESC strip. */
    {
        char *clean = (char *)malloc(body_len * 2 + 1);
        size_t ci = 0, k;
        if (clean) {
            for (k = 0; k < body_len; k++) {
                unsigned char c = (unsigned char)body[k];
                if (c == 27) continue;
                if (c == '\n' && (k == 0 || body[k-1] != '\r')) {
                    clean[ci++] = '\r';
                    clean[ci++] = '\n';
                } else {
                    clean[ci++] = (char)c;
                }
            }
            clean[ci] = '\0';
            if (g_w_document_text) free(g_w_document_text);
            g_w_document_text = clean;
            wais_show_view(2);
        }
    }
    free(body);

    _snprintf(status, sizeof(status), "Loaded %lu bytes", (unsigned long)body_len);
    wais_status(content, status);

    /* Move keyboard focus to the output pane so the user can immediately
     * scroll the document with arrow keys (instead of leaving focus on
     * the View button where keys do nothing). The output edit's caret
     * is hidden on focus, so this looks like a viewer, not an editor. */
    SetFocus(g_w_output);
}

/* ------------------------------------------------------------------ */
/* Layout reflow: anchor right-side controls, stretch search edit and  */
/* output pane to fill width. Mirrors LayoutControls in MGTWAIS GUI.   */
/* ------------------------------------------------------------------ */

void wais_module_resize(HWND content, int w, int h)
{
    const int margin = 16;
    const int gap    = 10;
    const int btn_w  = 105;
    const int btn_h  = 30;
    const int row_h  = 26;
    const int doc_w  = 55;
    const int dlbl_w = 55;
    int view_x, doc_edit_x, doc_label_x;

    if (!g_w_output) return;

    /* Row 1: server/port/db fields on the left; Connect + Disconnect
     * anchored right so they stay visible when the window is wider
     * than the fixed field block. Connect first, then search, then
     * view — read top-to-bottom matches the actual workflow. */
    MoveWindow(g_w_server_lbl,  margin,      18, 55, 22, TRUE);
    MoveWindow(g_w_server_edit, margin + 60, 14, 240, row_h, TRUE);
    MoveWindow(g_w_port_lbl,    margin + 310, 18, 40, 22, TRUE);
    MoveWindow(g_w_port_edit,   margin + 350, 14, 50, row_h, TRUE);
    MoveWindow(g_w_db_lbl,      margin + 410, 18, 70, 22, TRUE);
    MoveWindow(g_w_db_edit,     margin + 485, 14, 150, row_h, TRUE);
    {
        const int conn_w   = 85;
        const int disc_w   = 95;
        const int conn_gap = 8;
        int disc_x = w - margin - disc_w;
        int conn_x = disc_x - conn_gap - conn_w;
        MoveWindow(g_w_connect_btn,    conn_x, 12, conn_w, btn_h, TRUE);
        MoveWindow(g_w_disconnect_btn, disc_x, 12, disc_w, btn_h, TRUE);
    }

    /* Row 2: Search edit stretches; Search button anchors right. */
    MoveWindow(g_w_search_lbl,
        margin, 53, 60, row_h, TRUE);
    MoveWindow(g_w_search_edit,
        margin + 60, 49, w - margin - 60 - gap - btn_w - margin, row_h, TRUE);
    MoveWindow(g_w_search_btn,
        w - margin - btn_w, 47, btn_w, btn_h, TRUE);

    /* Row 3: Protocol label + [<] [>] + Doc# + View. View anchors right. */
    MoveWindow(g_w_proto_lbl, margin, 90, 200, 22, TRUE);
    MoveWindow(g_w_back_btn,  margin + 220, 85, 40, btn_h, TRUE);
    MoveWindow(g_w_fwd_btn,   margin + 265, 85, 40, btn_h, TRUE);
    view_x      = w - margin - btn_w;
    doc_edit_x  = view_x - gap - doc_w;
    doc_label_x = doc_edit_x - 5 - dlbl_w;
    MoveWindow(g_w_doc_lbl,  doc_label_x, 90, dlbl_w, 22, TRUE);
    MoveWindow(g_w_doc_edit, doc_edit_x,  86, doc_w,  row_h, TRUE);
    MoveWindow(g_w_view_btn, view_x,      84, btn_w,  btn_h, TRUE);

    /* Output: fills the rest. */
    MoveWindow(g_w_output, margin, 120, w - 2 * margin, h - 120 - margin, TRUE);

    /* Add a small internal padding so the first line isn't clipped by
     * the EDIT control's top edge / client-edge border, and so text
     * doesn't hug the left/right edges either. EM_SETRECTNP sets the
     * formatting rectangle without forcing a paint. Called on every
     * resize because the formatting rect resets when the control moves. */
    {
        RECT fmt;
        GetClientRect(g_w_output, &fmt);
        fmt.top    += 8;
        fmt.left   += 6;
        fmt.right  -= 6;
        SendMessageA(g_w_output, EM_SETRECTNP, 0, (LPARAM)&fmt);
    }
}

/* ------------------------------------------------------------------ */
/* Activate: build all controls inside the content panel.              */
/* ------------------------------------------------------------------ */

void wais_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    /* Re-activation path: controls already exist, just show them and
     * preserve everything (search query, results, doc text, scroll,
     * connected/disconnected state). */
    if (g_w_controls_created) {
        ShowWindow(g_w_search_lbl,     SW_SHOW);
        ShowWindow(g_w_search_edit,    SW_SHOW);
        ShowWindow(g_w_search_btn,     SW_SHOW);
        ShowWindow(g_w_server_lbl,     SW_SHOW);
        ShowWindow(g_w_server_edit,    SW_SHOW);
        ShowWindow(g_w_port_lbl,       SW_SHOW);
        ShowWindow(g_w_port_edit,      SW_SHOW);
        ShowWindow(g_w_db_lbl,         SW_SHOW);
        ShowWindow(g_w_db_edit,        SW_SHOW);
        ShowWindow(g_w_connect_btn,    SW_SHOW);
        ShowWindow(g_w_disconnect_btn, SW_SHOW);
        ShowWindow(g_w_proto_lbl,      SW_SHOW);
        ShowWindow(g_w_back_btn,       SW_SHOW);
        ShowWindow(g_w_fwd_btn,        SW_SHOW);
        ShowWindow(g_w_doc_lbl,        SW_SHOW);
        ShowWindow(g_w_doc_edit,       SW_SHOW);
        ShowWindow(g_w_view_btn,       SW_SHOW);
        ShowWindow(g_w_output,         SW_SHOW);
        SetFocus(g_w_connected ? g_w_search_edit : g_w_server_edit);
        return;
    }

    /* Initialize active bindings on first activate. Preserved across
     * re-activations during a single session (Connect updates them). */
    if (!g_w_active_host[0]) {
        strcpy(g_w_active_host, WAIS_DEFAULT_HOST);
        strcpy(g_w_active_port, WAIS_DEFAULT_PORT);
        strcpy(g_w_active_db,   WAIS_DEFAULT_DB);
    }

    /* Row 1 */
    g_w_search_lbl = CreateWindowA("STATIC", "Search:",
        WS_CHILD | WS_VISIBLE, 0, 0, 60, 22, content, NULL, hInst, NULL);
    g_w_search_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 200, 26, content, (HMENU)(INT_PTR)IDC_WAIS_SEARCH_EDIT, hInst, NULL);
    g_w_search_btn = CreateWindowA("BUTTON", "Search",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 105, 30, content, (HMENU)(INT_PTR)IDC_WAIS_SEARCH_BTN, hInst, NULL);

    /* Row 2 */
    g_w_server_lbl = CreateWindowA("STATIC", "Server:",
        WS_CHILD | WS_VISIBLE, 0, 0, 55, 22, content, NULL, hInst, NULL);
    g_w_server_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_w_active_host,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 240, 26, content, (HMENU)(INT_PTR)IDC_WAIS_SERVER_EDIT, hInst, NULL);
    g_w_port_lbl = CreateWindowA("STATIC", "Port:",
        WS_CHILD | WS_VISIBLE, 0, 0, 40, 22, content, NULL, hInst, NULL);
    g_w_port_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_w_active_port,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        0, 0, 50, 26, content, (HMENU)(INT_PTR)IDC_WAIS_PORT_EDIT, hInst, NULL);
    g_w_db_lbl = CreateWindowA("STATIC", "Database:",
        WS_CHILD | WS_VISIBLE, 0, 0, 70, 22, content, NULL, hInst, NULL);
    g_w_db_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_w_active_db,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 150, 26, content, (HMENU)(INT_PTR)IDC_WAIS_DB_EDIT, hInst, NULL);
    g_w_connect_btn = CreateWindowA("BUTTON", "Connect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 85, 30, content, (HMENU)(INT_PTR)IDC_WAIS_CONNECT_BTN, hInst, NULL);
    g_w_disconnect_btn = CreateWindowA("BUTTON", "Disconnect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 95, 30, content, (HMENU)(INT_PTR)IDC_WAIS_DISCONNECT_BTN, hInst, NULL);

    /* Row 3 */
    g_w_proto_lbl = CreateWindowA("STATIC", "Protocol: Z39.50-1988",
        WS_CHILD | WS_VISIBLE, 0, 0, 200, 22, content, NULL, hInst, NULL);
    g_w_back_btn = CreateWindowA("BUTTON", "<",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 40, 30, content, (HMENU)(INT_PTR)IDC_WAIS_BACK_BTN, hInst, NULL);
    g_w_fwd_btn = CreateWindowA("BUTTON", ">",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 40, 30, content, (HMENU)(INT_PTR)IDC_WAIS_FWD_BTN, hInst, NULL);
    g_w_doc_lbl = CreateWindowA("STATIC", "Doc #:",
        WS_CHILD | WS_VISIBLE, 0, 0, 55, 22, content, NULL, hInst, NULL);
    g_w_doc_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        0, 0, 55, 26, content, (HMENU)(INT_PTR)IDC_WAIS_DOC_EDIT, hInst, NULL);
    g_w_view_btn = CreateWindowA("BUTTON", "View",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 105, 30, content, (HMENU)(INT_PTR)IDC_WAIS_VIEW_BTN, hInst, NULL);

    /* Output pane: multi-line, read-only, horizontal + vertical scroll. */
    g_w_output = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
        0, 0, 200, 200, content, (HMENU)(INT_PTR)IDC_WAIS_OUTPUT, hInst, NULL);
    SendMessageA(g_w_output, EM_SETLIMITTEXT, (WPARAM)262144, 0);

    /* Apply UI font to controls, output font to the output pane. */
    if (g_hFontUI) {
        SendMessageA(g_w_search_lbl,  WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_search_edit, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_search_btn,  WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_server_lbl,  WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_server_edit, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_port_lbl,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_port_edit,   WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_db_lbl,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_db_edit,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_connect_btn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_disconnect_btn, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_proto_lbl,   WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_back_btn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_fwd_btn,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_doc_lbl,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_doc_edit,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_view_btn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }
    if (g_hFontOutput)
        SendMessageA(g_w_output, WM_SETFONT, (WPARAM)g_hFontOutput, TRUE);

    /* Subclass search edit and doc edit for Enter handling, the output
     * edit for caret-hide + arrow-as-scroll, and the server/port/db
     * edits with FieldScrollSub so Up/Down/PgUp/PgDn from any of those
     * fields forwards to the output pane's vertical scroll. */
    g_orig_search_proc = (WNDPROC)SetWindowLongPtrA(
        g_w_search_edit, GWLP_WNDPROC, (LONG_PTR)SearchEditSub);
    g_orig_doc_proc    = (WNDPROC)SetWindowLongPtrA(
        g_w_doc_edit,    GWLP_WNDPROC, (LONG_PTR)DocEditSub);
    g_orig_output_proc = (WNDPROC)SetWindowLongPtrA(
        g_w_output,      GWLP_WNDPROC, (LONG_PTR)OutputSub);
    /* All three field edits share the same default WndProc (Edit class),
     * so capturing it once from the server edit covers all three. */
    g_orig_field_proc = (WNDPROC)SetWindowLongPtrA(
        g_w_server_edit, GWLP_WNDPROC, (LONG_PTR)FieldScrollSub);
    SetWindowLongPtrA(g_w_port_edit, GWLP_WNDPROC, (LONG_PTR)FieldScrollSub);
    SetWindowLongPtrA(g_w_db_edit,   GWLP_WNDPROC, (LONG_PTR)FieldScrollSub);

    GetClientRect(content, &rc);
    wais_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);

    g_w_controls_created = TRUE;

    /* Initial UI state: pre-connect. Only Server/Port/Database/Connect
     * reachable; Search/Doc/View/Back/Forward/Disconnect grayed until
     * Connect succeeds. Mirrors the ARPANET FTP-Mail session gate. */
    wais_set_connected_state(FALSE);
    SetFocus(g_w_server_edit);
}

/* ------------------------------------------------------------------ */
/* Deactivate: tear down. Destructive switch per BLUEPRINT 3.2.       */
/* ------------------------------------------------------------------ */

void wais_module_deactivate(HWND content)
{
    (void)content;

    /* Non-destructive deactivate: hide controls and preserve all state
     * (search query, results, doc text, server/port/db values, scroll
     * position). User can switch to another module and back without
     * losing in-progress work. See [[feedback-module-state-preserved]]. */
    if (!g_w_controls_created) return;

    ShowWindow(g_w_search_lbl,     SW_HIDE);
    ShowWindow(g_w_search_edit,    SW_HIDE);
    ShowWindow(g_w_search_btn,     SW_HIDE);
    ShowWindow(g_w_server_lbl,     SW_HIDE);
    ShowWindow(g_w_server_edit,    SW_HIDE);
    ShowWindow(g_w_port_lbl,       SW_HIDE);
    ShowWindow(g_w_port_edit,      SW_HIDE);
    ShowWindow(g_w_db_lbl,         SW_HIDE);
    ShowWindow(g_w_db_edit,        SW_HIDE);
    ShowWindow(g_w_connect_btn,    SW_HIDE);
    ShowWindow(g_w_disconnect_btn, SW_HIDE);
    ShowWindow(g_w_proto_lbl,      SW_HIDE);
    ShowWindow(g_w_back_btn,       SW_HIDE);
    ShowWindow(g_w_fwd_btn,        SW_HIDE);
    ShowWindow(g_w_doc_lbl,        SW_HIDE);
    ShowWindow(g_w_doc_edit,       SW_HIDE);
    ShowWindow(g_w_view_btn,       SW_HIDE);
    ShowWindow(g_w_output,         SW_HIDE);
}

/* ------------------------------------------------------------------ */
/* on_command dispatch.                                                */
/* ------------------------------------------------------------------ */

BOOL wais_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id    = LOWORD(wParam);
    int notif = HIWORD(wParam);
    (void)lParam;

    switch (id) {
    case IDC_WAIS_SEARCH_BTN:
        if (notif == BN_CLICKED) { wais_on_search(content); return TRUE; }
        break;
    case IDC_WAIS_CONNECT_BTN:
        if (notif == BN_CLICKED) { wais_on_connect(content); return TRUE; }
        break;
    case IDC_WAIS_DISCONNECT_BTN:
        if (notif == BN_CLICKED) { wais_on_disconnect(content); return TRUE; }
        break;
    case IDC_WAIS_VIEW_BTN:
        if (notif == BN_CLICKED) { wais_on_view(content); return TRUE; }
        break;
    case IDC_WAIS_BACK_BTN:
        if (notif == BN_CLICKED) {
            if (g_w_current_view == 2 && g_w_results_text) wais_show_view(1);
            return TRUE;
        }
        break;
    case IDC_WAIS_FWD_BTN:
        if (notif == BN_CLICKED) {
            if (g_w_current_view == 1 && g_w_document_text) wais_show_view(2);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

BOOL wais_module_has_unsaved(void)
{
    return FALSE;
}
