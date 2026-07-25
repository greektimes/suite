/*
 * archie_module.c - Archie Search module: WSArchie-style GUI over the
 *                   Prospero/ARDP transport in archie_ardp.[ch].
 *
 * Layout mirrors the classic WSArchie / NetManage Chameleon Archie
 * dialog, and (v2, 2026-07-24) is skinned in the light-gray Windows 3.1
 * dialog look: gray face, black text, white editable fields, a standard
 * default push button. That reskin is scoped to THIS tab only: all Archie
 * controls live inside a private gray container window (g_a_panel, class
 * MGTArchiePanel) whose WndProc paints the gray face and answers the
 * WM_CTLCOLOR* messages, so the suite's shared white-on-black content
 * panel is untouched and no other retro tab changes.
 *
 *   Row 1  Search term + Search button
 *   Row 2  Archie Server + Port (default 1525) + static "Protocol: UDP"
 *   Row 3  Search type: Substring (ci) / Substring (cs) / Exact /
 *          Regular Expression, plus an "Exact first" modifier checkbox
 *   Panes  three result lists: Hosts | Directories | Files
 *   (blank line)
 *   Detail Selected file: File Name / Size / Mode / Date / Host Address,
 *          shown as plain text (no field boxes)
 *
 * Search-type wire chars are mapped in archie_ardp (=,S,C,R and the
 * exact-first variants s,c,r). The protocol is Prospero ARDP over UDP
 * only, so "UDP" is a static label, never a TCP option.
 *
 * All network I/O runs on a worker thread (archie_query blocks up to
 * ~16 s with retries) and the UI polls a 100 ms timer for the result, so
 * the tab stays responsive -- the same pattern the WAIS module uses.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>   /* GetSaveFileName for the Download Save As dialog */

#include "archie_module.h"
#include "suite_shell.h"
#include "archie_ardp.h"
#include "ftp_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Module-local control IDs (3000+). */
#define IDC_A_SEARCH_EDIT   3001
#define IDC_A_SEARCH_BTN    3002
#define IDC_A_SERVER_EDIT   3003
#define IDC_A_HOSTS_LIST    3004
#define IDC_A_DIRS_LIST     3005
#define IDC_A_FILES_LIST    3006
#define IDC_A_FNAME_VAL     3007
#define IDC_A_SIZE_VAL      3008
#define IDC_A_MODE_VAL      3009
/* Type radios kept contiguous for CheckRadioButton. */
#define IDC_A_TYPE_SUBSTR   3010
#define IDC_A_TYPE_SUBSTRCS 3011
#define IDC_A_TYPE_EXACT    3012
#define IDC_A_TYPE_REGEX    3013
#define IDC_A_DATE_VAL      3014
#define IDC_A_HOST_VAL      3015
#define IDC_A_PORT_EDIT     3016
#define IDC_A_EXACTFIRST    3017
#define IDC_A_DOWNLOAD_BTN  3018

#define ARCHIE_QUERY_TIMER_ID  0xB001
#define ARCHIE_DL_TIMER_ID     0xB002
#define ARCHIE_PANEL_CLASS     "MGTArchiePanel"

/* Windows 3.1 dialog palette. */
#define ARCHIE_FACE   RGB(192, 192, 192)
#define ARCHIE_WHITE  RGB(255, 255, 255)
#define ARCHIE_BLACK  RGB(0, 0, 0)

/* The private gray container that hosts every Archie control. */
static HWND    g_a_panel = NULL;
static HBRUSH  g_a_face_brush  = NULL;
static HBRUSH  g_a_white_brush = NULL;
static BOOL    g_a_class_registered = FALSE;

/* Live control handles. */
static HWND g_a_search_lbl, g_a_search_edit, g_a_search_btn;
static HWND g_a_server_lbl, g_a_server_edit;
static HWND g_a_port_lbl,   g_a_port_edit;
static HWND g_a_proto_lbl,  g_a_proto_val;
static HWND g_a_type_lbl;
static HWND g_a_type_substr, g_a_type_substrcs, g_a_type_exact, g_a_type_regex;
static HWND g_a_exactfirst_chk;
static HWND g_a_hosts_lbl,  g_a_hosts_list;
static HWND g_a_dirs_lbl,   g_a_dirs_list;
static HWND g_a_files_lbl,  g_a_files_list;
static HWND g_a_detail_lbl;
static HWND g_a_fname_lbl,  g_a_fname_val;
static HWND g_a_size_lbl,   g_a_size_val;
static HWND g_a_mode_lbl,   g_a_mode_val;
static HWND g_a_date_lbl,   g_a_date_val;
static HWND g_a_host_lbl,   g_a_host_val;
static HWND g_a_download_btn;

/* Bold variant of g_hFontUI, used only for the fixed detail labels. */
static HFONT g_a_bold_font = NULL;

/* Index into g_a_result.hits of the currently selected file, or -1 for
 * none. Drives the Download button's enabled state. */
static int g_a_selected_hit = -1;

static BOOL g_a_controls_created = FALSE;

/* Active server + base type + exact-first modifier. */
static char g_a_active_server[256] = ARCHIE_DEFAULT_SERVER;
static archie_search_type_t g_a_type = ARCHIE_SUBSTR_CI;
static BOOL g_a_exact_first = FALSE;

/* Current result set, owned by the module until the next Search. */
static archie_result_t g_a_result;
static BOOL            g_a_have_result = FALSE;

/* Subclass. */
static WNDPROC g_a_orig_search_proc = NULL;

/* Forward declarations. */
static void archie_on_search(void);
static void archie_fill_dirs_for_host(const char *host);
static void archie_fill_files_for_host_dir(const char *host, const char *dir);
static void archie_show_detail(int hit_index);
static void archie_clear_detail(void);
static void archie_clear_panes(void);
static void archie_status(const char *text);
static void archie_update_exactfirst_enable(void);

/* ------------------------------------------------------------------ */
/* Status helper: set and flush so the transition is visible.          */
/* ------------------------------------------------------------------ */

static void archie_status(const char *text)
{
    HWND root;
    suite_set_status(text);
    root = g_a_panel ? GetAncestor(g_a_panel, GA_ROOT) : NULL;
    if (root) UpdateWindow(root);
}

/* Read the selected item's text from a list box ("" if none). */
static void archie_get_sel(HWND list, char *buf, int sz)
{
    LRESULT sel, len;
    if (sz <= 0) return;
    buf[0] = '\0';
    sel = SendMessageA(list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return;
    len = SendMessageA(list, LB_GETTEXTLEN, (WPARAM)sel, 0);
    if (len == LB_ERR || len < 0 || len >= sz) return;
    SendMessageA(list, LB_GETTEXT, (WPARAM)sel, (LPARAM)buf);
}

/* ------------------------------------------------------------------ */
/* Subclass: search edit. Enter triggers Search.                       */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK ArchieSearchSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_GETDLGCODE) {
        MSG *pmsg = (MSG *)l;
        LRESULT base = CallWindowProcA(g_a_orig_search_proc, h, m, w, l);
        if (pmsg && pmsg->message == WM_KEYDOWN && pmsg->wParam == VK_RETURN)
            return base | DLGC_WANTMESSAGE;
        return base;
    }
    if (m == WM_KEYDOWN && w == VK_RETURN) { archie_on_search(); return 0; }
    if (m == WM_CHAR && w == '\r') return 0;   /* swallow the ding */
    return CallWindowProcA(g_a_orig_search_proc, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Detail formatting helpers (graceful on any bad input).              */
/* ------------------------------------------------------------------ */

/* Group a plain decimal digit string with US thousands separators. */
static void archie_group_commas(const char *digits, char *out, int outsz)
{
    int len = (int)strlen(digits), i, oi = 0;
    for (i = 0; i < len && oi < outsz - 2; i++) {
        if (i > 0 && (len - i) % 3 == 0) out[oi++] = ',';
        out[oi++] = digits[i];
    }
    out[oi] = '\0';
}

/* SIZE (a byte count in digits) -> Explorer-style whole KB, rounded up,
 * comma-grouped, " KB" suffix (e.g. 687420 -> "672 KB", 68 -> "1 KB").
 * Non-numeric input falls back to the raw string; empty -> empty. */
static void archie_format_size_kb(const char *bytes_str, char *out, int outsz)
{
    unsigned long long bytes, kb;
    char *end = NULL;
    char digits[32];

    if (outsz > 0) out[0] = '\0';
    if (!bytes_str || !bytes_str[0]) return;

    bytes = strtoull(bytes_str, &end, 10);
    if (!end || end == bytes_str) {          /* not a number: show raw */
        strncpy(out, bytes_str, (size_t)outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }
    /* ceil(bytes / 1024); any non-empty file is at least 1 KB. */
    kb = (bytes + 1023ULL) / 1024ULL;
    if (bytes > 0 && kb == 0) kb = 1;
    _snprintf(digits, sizeof digits, "%llu", kb);
    digits[sizeof digits - 1] = '\0';
    {
        char grouped[48];
        archie_group_commas(digits, grouped, sizeof grouped);
        _snprintf(out, outsz, "%s KB", grouped);
        out[outsz - 1] = '\0';
    }
}

/* Prospero ASN date "YYYYMMDDHHMMSS[Z]" -> "14 April 2026, 12:20 PM"
 * (UTC value shown as-is, no TZ conversion or label). Any parse failure
 * falls back to the raw string. */
static void archie_format_date_human(const char *raw, char *out, int outsz)
{
    static const char *months[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December" };
    int i, year, mon, day, hour, minute, h12;
    const char *ampm;

    if (outsz > 0) out[0] = '\0';
    if (!raw || !raw[0]) return;

    if ((int)strlen(raw) < 12) goto fallback;
    for (i = 0; i < 12; i++)
        if (raw[i] < '0' || raw[i] > '9') goto fallback;

    year = (raw[0]-'0')*1000 + (raw[1]-'0')*100 + (raw[2]-'0')*10 + (raw[3]-'0');
    mon  = (raw[4]-'0')*10 + (raw[5]-'0');
    day  = (raw[6]-'0')*10 + (raw[7]-'0');
    hour = (raw[8]-'0')*10 + (raw[9]-'0');
    minute = (raw[10]-'0')*10 + (raw[11]-'0');
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || minute > 59)
        goto fallback;

    ampm = (hour < 12) ? "AM" : "PM";
    h12  = hour % 12; if (h12 == 0) h12 = 12;
    _snprintf(out, outsz, "%d %s %d, %d:%02d %s",
              day, months[mon - 1], year, h12, minute, ampm);
    out[outsz - 1] = '\0';
    return;

fallback:
    strncpy(out, raw, (size_t)outsz - 1);
    out[outsz - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/* Pane / detail population.                                            */
/* ------------------------------------------------------------------ */

static void archie_clear_detail(void)
{
    SetWindowTextA(g_a_fname_val, "");
    SetWindowTextA(g_a_size_val,  "");
    SetWindowTextA(g_a_mode_val,  "");
    SetWindowTextA(g_a_date_val,  "");
    SetWindowTextA(g_a_host_val,  "");
    /* No file selected: disable Download. */
    g_a_selected_hit = -1;
    if (g_a_download_btn) EnableWindow(g_a_download_btn, FALSE);
}

static void archie_clear_panes(void)
{
    SendMessageA(g_a_hosts_list, LB_RESETCONTENT, 0, 0);
    SendMessageA(g_a_dirs_list,  LB_RESETCONTENT, 0, 0);
    SendMessageA(g_a_files_list, LB_RESETCONTENT, 0, 0);
    archie_clear_detail();
}

static void archie_fill_dirs_for_host(const char *host)
{
    int i;
    SendMessageA(g_a_dirs_list,  LB_RESETCONTENT, 0, 0);
    SendMessageA(g_a_files_list, LB_RESETCONTENT, 0, 0);
    archie_clear_detail();
    if (!g_a_have_result) return;

    for (i = 0; i < g_a_result.hit_count; i++) {
        const char *d = g_a_result.hits[i].dir;
        if (strcmp(g_a_result.hits[i].host, host) != 0) continue;
        if (!d[0]) d = "/";
        if (SendMessageA(g_a_dirs_list, LB_FINDSTRINGEXACT,
                         (WPARAM)-1, (LPARAM)d) == LB_ERR)
            SendMessageA(g_a_dirs_list, LB_ADDSTRING, 0, (LPARAM)d);
    }
    if (SendMessageA(g_a_dirs_list, LB_GETCOUNT, 0, 0) > 0) {
        char dir[512];
        SendMessageA(g_a_dirs_list, LB_SETCURSEL, 0, 0);
        archie_get_sel(g_a_dirs_list, dir, sizeof dir);
        archie_fill_files_for_host_dir(host, dir);
    }
}

static void archie_fill_files_for_host_dir(const char *host, const char *dir)
{
    int i;
    SendMessageA(g_a_files_list, LB_RESETCONTENT, 0, 0);
    archie_clear_detail();
    if (!g_a_have_result) return;

    for (i = 0; i < g_a_result.hit_count; i++) {
        const char *d = g_a_result.hits[i].dir;
        LRESULT idx;
        if (strcmp(g_a_result.hits[i].host, host) != 0) continue;
        if (!d[0]) d = "/";
        if (strcmp(d, dir) != 0) continue;
        idx = SendMessageA(g_a_files_list, LB_ADDSTRING, 0,
                           (LPARAM)g_a_result.hits[i].filename);
        if (idx != LB_ERR)
            SendMessageA(g_a_files_list, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
    }
}

static void archie_show_detail(int hit_index)
{
    const archie_hit_t *hh;
    char sizebuf[48], datebuf[96];
    if (!g_a_have_result || hit_index < 0 ||
        hit_index >= g_a_result.hit_count)
        return;
    hh = &g_a_result.hits[hit_index];
    archie_format_size_kb(hh->size, sizebuf, sizeof sizebuf);
    archie_format_date_human(hh->date, datebuf, sizeof datebuf);
    SetWindowTextA(g_a_fname_val, hh->filename[0] ? hh->filename : hh->name);
    SetWindowTextA(g_a_size_val,  sizebuf);
    SetWindowTextA(g_a_mode_val,  hh->mode);
    SetWindowTextA(g_a_date_val,  datebuf);
    SetWindowTextA(g_a_host_val,  hh->host);
    /* A valid file is selected: remember it and enable Download. */
    g_a_selected_hit = hit_index;
    if (g_a_download_btn) EnableWindow(g_a_download_btn, TRUE);
}

static void archie_apply_result(void)
{
    int i;
    archie_clear_panes();
    if (!g_a_have_result) return;

    for (i = 0; i < g_a_result.hit_count; i++) {
        const char *hst = g_a_result.hits[i].host;
        if (!hst[0]) continue;
        if (SendMessageA(g_a_hosts_list, LB_FINDSTRINGEXACT,
                         (WPARAM)-1, (LPARAM)hst) == LB_ERR)
            SendMessageA(g_a_hosts_list, LB_ADDSTRING, 0, (LPARAM)hst);
    }
    if (SendMessageA(g_a_hosts_list, LB_GETCOUNT, 0, 0) > 0) {
        char host[256];
        SendMessageA(g_a_hosts_list, LB_SETCURSEL, 0, 0);
        archie_get_sel(g_a_hosts_list, host, sizeof host);
        archie_fill_dirs_for_host(host);
    }
}

/* ------------------------------------------------------------------ */
/* Backgrounded query: worker thread + UI-thread polling timer.        */
/* ------------------------------------------------------------------ */

typedef struct ArchieQueryCtx {
    char                 server[256];
    int                  port;
    char                 term[512];
    archie_search_type_t type;
    int                  exact_first;
    int                  maxhits;

    volatile LONG        done;
    archie_result_t      result;
} ArchieQueryCtx;

static ArchieQueryCtx *g_a_query      = NULL;
static UINT_PTR        g_a_query_timer = 0;

static DWORD WINAPI archie_query_worker(LPVOID arg)
{
    ArchieQueryCtx *ctx = (ArchieQueryCtx *)arg;
    archie_query(ctx->server, ctx->port, ctx->term, ctx->type,
                 ctx->exact_first, ctx->maxhits, &ctx->result);
    InterlockedExchange(&ctx->done, 1);
    return 0;
}

static void CALLBACK archie_query_timer_proc(HWND hwnd, UINT msg,
                                             UINT_PTR id, DWORD time)
{
    ArchieQueryCtx *ctx;
    (void)msg; (void)time;

    if (!g_a_query || !g_a_query->done) return;

    KillTimer(hwnd, id);
    g_a_query_timer = 0;
    ctx = g_a_query;
    g_a_query = NULL;

    if (g_a_have_result) { archie_result_free(&g_a_result); g_a_have_result = FALSE; }
    g_a_result = ctx->result;      /* transfer ownership of the hit array */
    g_a_have_result = TRUE;

    archie_apply_result();

    if (ctx->result.status == ARCHIE_OK) {
        if (ctx->result.hit_count > 0)
            archie_status(ctx->result.message);
        else
            archie_status("No matches.");
    } else {
        archie_status(ctx->result.message[0] ? ctx->result.message
                                             : "Archie query failed.");
    }

    EnableWindow(g_a_search_btn, TRUE);
    SetFocus(g_a_search_edit);
    free(ctx);
}

/* ------------------------------------------------------------------ */
/* Search: read the term/server/port/type, launch the worker.          */
/* ------------------------------------------------------------------ */

static void archie_on_search(void)
{
    char server[256], portstr[16], term[512];
    char *colon;
    int   port;
    ArchieQueryCtx *ctx;
    HANDLE thr;

    if (g_a_query) return;   /* one query in flight at a time */

    GetWindowTextA(g_a_search_edit, term, sizeof term);
    {
        char *s = term, *e;
        while (*s == ' ' || *s == '\t') s++;
        if (s != term) memmove(term, s, strlen(s) + 1);
        e = term + strlen(term);
        while (e > term && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    }
    if (!term[0]) {
        archie_status("Enter a search term and press Search.");
        return;
    }

    GetWindowTextA(g_a_server_edit, server, sizeof server);
    if (!server[0]) {
        strncpy(server, ARCHIE_DEFAULT_SERVER, sizeof server - 1);
        server[sizeof server - 1] = '\0';
        SetWindowTextA(g_a_server_edit, server);
    }
    /* Port from the field; a stray :port in the server field also wins. */
    GetWindowTextA(g_a_port_edit, portstr, sizeof portstr);
    port = atoi(portstr);
    if (port <= 0 || port > 65535) {
        port = ARCHIE_DEFAULT_PORT;
        _snprintf(portstr, sizeof portstr, "%d", port);
        SetWindowTextA(g_a_port_edit, portstr);
    }
    colon = strchr(server, ':');
    if (colon) {
        int p = atoi(colon + 1);
        *colon = '\0';
        if (p > 0 && p <= 65535) port = p;
    }
    strncpy(g_a_active_server, server, sizeof g_a_active_server - 1);
    g_a_active_server[sizeof g_a_active_server - 1] = '\0';

    ctx = (ArchieQueryCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) { archie_status("Out of memory."); return; }
    ctx->port        = port;
    ctx->type        = g_a_type;
    /* Exact has no exact-first form; ignore the modifier there. */
    ctx->exact_first = (g_a_type != ARCHIE_EXACT) && g_a_exact_first;
    ctx->maxhits     = ARCHIE_MAXHITS_DEFAULT;
    strncpy(ctx->server, server, sizeof ctx->server - 1);
    strncpy(ctx->term,   term,   sizeof ctx->term   - 1);

    g_a_query = ctx;
    EnableWindow(g_a_search_btn, FALSE);
    archie_status("Searching Archie...");

    thr = CreateThread(NULL, 0, archie_query_worker, ctx, 0, NULL);
    if (!thr) {
        archie_status("Cannot start query thread.");
        EnableWindow(g_a_search_btn, TRUE);
        free(ctx);
        g_a_query = NULL;
        return;
    }
    CloseHandle(thr);

    g_a_query_timer = SetTimer(g_a_panel, ARCHIE_QUERY_TIMER_ID,
                               100, archie_query_timer_proc);
}

/* Exact-first is a no-op when Exact is the base type: gray it out there. */
static void archie_update_exactfirst_enable(void)
{
    if (g_a_exactfirst_chk)
        EnableWindow(g_a_exactfirst_chk, g_a_type != ARCHIE_EXACT);
}

/* ------------------------------------------------------------------ */
/* Download: FTP the selected file to a chosen local path, on a worker   */
/* thread (never the UI thread), polled by a 100 ms timer -- the same    */
/* off-UI pattern the search uses.                                       */
/* ------------------------------------------------------------------ */

typedef struct ArchieDlCtx {
    char          host[256];
    char          remote[512];
    char          local[MAX_PATH];
    volatile LONG done;
    volatile LONG bytes;     /* progress, updated by the worker */
    int           ok;
    long          total;
    char          msg[256];
} ArchieDlCtx;

static ArchieDlCtx *g_a_dl       = NULL;
static UINT_PTR     g_a_dl_timer = 0;

/* Worker-thread progress callback: no UI here, just publish the count. */
static void archie_dl_progress(void *user, long bytes)
{
    ArchieDlCtx *c = (ArchieDlCtx *)user;
    InterlockedExchange(&c->bytes, bytes);
}

static DWORD WINAPI archie_dl_worker(LPVOID arg)
{
    ArchieDlCtx *c = (ArchieDlCtx *)arg;
    char err[256];
    long total = 0;
    err[0] = '\0';
    c->ok = ftp_fetch_file(c->host, FTP_FETCH_DEFAULT_PORT, c->remote, c->local,
                           archie_dl_progress, c, &total, err, sizeof err);
    c->total = total;
    if (c->ok)
        _snprintf(c->msg, sizeof c->msg, "Saved %ld bytes to %s", total, c->local);
    else {
        strncpy(c->msg, err[0] ? err : "Download failed.", sizeof c->msg - 1);
        c->msg[sizeof c->msg - 1] = '\0';
    }
    InterlockedExchange(&c->done, 1);
    return 0;
}

static void CALLBACK archie_dl_timer_proc(HWND hwnd, UINT msg,
                                          UINT_PTR id, DWORD time)
{
    ArchieDlCtx *c;
    (void)msg; (void)time;
    if (!g_a_dl) return;

    if (!g_a_dl->done) {
        char s[96];
        _snprintf(s, sizeof s, "Downloading... %ld bytes", (long)g_a_dl->bytes);
        s[sizeof s - 1] = '\0';
        suite_set_status(s);     /* light-touch progress, no forced repaint */
        return;
    }

    KillTimer(hwnd, id);
    g_a_dl_timer = 0;
    c = g_a_dl;
    g_a_dl = NULL;

    archie_status(c->msg);
    /* Re-enable Download only if a valid file is still selected. */
    if (g_a_download_btn) EnableWindow(g_a_download_btn, g_a_selected_hit >= 0);
    free(c);
}

static void archie_on_download(void)
{
    const archie_hit_t *hh;
    OPENFILENAMEA ofn;
    char path[MAX_PATH];
    ArchieDlCtx *ctx;
    HANDLE thr;
    HWND owner;

    if (g_a_dl) return;              /* one transfer at a time */
    if (!g_a_have_result || g_a_selected_hit < 0 ||
        g_a_selected_hit >= g_a_result.hit_count) {
        archie_status("Select a file to download.");
        return;
    }
    hh = &g_a_result.hits[g_a_selected_hit];
    if (!hh->host[0] || !hh->hsoname[0]) {
        archie_status("Selected file has no download path.");
        return;
    }

    /* Save As dialog, defaulting to the file's own name. */
    memset(path, 0, sizeof path);
    strncpy(path, hh->filename[0] ? hh->filename : hh->name, sizeof path - 1);
    path[sizeof path - 1] = '\0';

    owner = GetAncestor(g_a_panel, GA_ROOT);
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = "All Files\0*.*\0\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = sizeof path;
    ofn.lpstrTitle  = "Download file";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameA(&ofn))
        return;                      /* Cancel: clean no-op */

    ctx = (ArchieDlCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) { archie_status("Out of memory."); return; }
    strncpy(ctx->host,   hh->host,    sizeof ctx->host   - 1);
    strncpy(ctx->remote, hh->hsoname, sizeof ctx->remote - 1);
    strncpy(ctx->local,  path,        sizeof ctx->local  - 1);

    g_a_dl = ctx;
    if (g_a_download_btn) EnableWindow(g_a_download_btn, FALSE);
    archie_status("Downloading...");

    thr = CreateThread(NULL, 0, archie_dl_worker, ctx, 0, NULL);
    if (!thr) {
        archie_status("Cannot start download thread.");
        if (g_a_download_btn) EnableWindow(g_a_download_btn, TRUE);
        free(ctx);
        g_a_dl = NULL;
        return;
    }
    CloseHandle(thr);

    g_a_dl_timer = SetTimer(g_a_panel, ARCHIE_DL_TIMER_ID,
                            100, archie_dl_timer_proc);
}

/* ------------------------------------------------------------------ */
/* on_command dispatch (called from the panel WndProc).                */
/* ------------------------------------------------------------------ */

BOOL archie_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id    = LOWORD(wParam);
    int notif = HIWORD(wParam);
    (void)content; (void)lParam;

    switch (id) {
    case IDC_A_SEARCH_BTN:
        if (notif == BN_CLICKED) { archie_on_search(); return TRUE; }
        break;

    case IDC_A_DOWNLOAD_BTN:
        if (notif == BN_CLICKED) { archie_on_download(); return TRUE; }
        break;

    case IDC_A_TYPE_SUBSTR:
    case IDC_A_TYPE_SUBSTRCS:
    case IDC_A_TYPE_EXACT:
    case IDC_A_TYPE_REGEX:
        if (notif == BN_CLICKED) {
            CheckRadioButton(g_a_panel, IDC_A_TYPE_SUBSTR, IDC_A_TYPE_REGEX, id);
            g_a_type = (id == IDC_A_TYPE_SUBSTRCS) ? ARCHIE_SUBSTR_CS
                     : (id == IDC_A_TYPE_EXACT)    ? ARCHIE_EXACT
                     : (id == IDC_A_TYPE_REGEX)    ? ARCHIE_REGEX
                                                   : ARCHIE_SUBSTR_CI;
            archie_update_exactfirst_enable();
            return TRUE;
        }
        break;

    case IDC_A_EXACTFIRST:
        if (notif == BN_CLICKED) {
            g_a_exact_first =
                (SendMessageA(g_a_exactfirst_chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return TRUE;
        }
        break;

    case IDC_A_HOSTS_LIST:
        if (notif == LBN_SELCHANGE) {
            char host[256];
            archie_get_sel(g_a_hosts_list, host, sizeof host);
            if (host[0]) archie_fill_dirs_for_host(host);
            return TRUE;
        }
        break;

    case IDC_A_DIRS_LIST:
        if (notif == LBN_SELCHANGE) {
            char host[256], dir[512];
            archie_get_sel(g_a_hosts_list, host, sizeof host);
            archie_get_sel(g_a_dirs_list,  dir,  sizeof dir);
            if (host[0] && dir[0]) archie_fill_files_for_host_dir(host, dir);
            return TRUE;
        }
        break;

    case IDC_A_FILES_LIST:
        if (notif == LBN_SELCHANGE) {
            LRESULT sel = SendMessageA(g_a_files_list, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                LRESULT idx = SendMessageA(g_a_files_list, LB_GETITEMDATA,
                                           (WPARAM)sel, 0);
                if (idx != LB_ERR) archie_show_detail((int)idx);
            }
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Gray container WndProc: paints the Windows 3.1 face, colors the      */
/* children (gray statics/buttons, white edits/lists), and routes       */
/* WM_COMMAND from its children to the module dispatch. This is what    */
/* scopes the reskin to the Archie tab without touching the shared      */
/* content panel.                                                       */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK ArchiePanelProc(HWND hwnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC  hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_a_face_brush);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)wParam, ARCHIE_FACE);
        SetTextColor((HDC)wParam, ARCHIE_BLACK);
        return (LRESULT)g_a_face_brush;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wParam, ARCHIE_WHITE);
        SetTextColor((HDC)wParam, ARCHIE_BLACK);
        return (LRESULT)g_a_white_brush;
    case WM_COMMAND:
        if (archie_module_on_command(hwnd, wParam, lParam)) return 0;
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void archie_register_panel_class(HINSTANCE hInst)
{
    WNDCLASSEXA wc;
    if (g_a_class_registered) return;
    if (!g_a_face_brush)  g_a_face_brush  = CreateSolidBrush(ARCHIE_FACE);
    if (!g_a_white_brush) g_a_white_brush = CreateSolidBrush(ARCHIE_WHITE);
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = ArchiePanelProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = g_a_face_brush;
    wc.lpszClassName = ARCHIE_PANEL_CLASS;
    RegisterClassExA(&wc);
    g_a_class_registered = TRUE;
}

/* ------------------------------------------------------------------ */
/* Layout. All coordinates are relative to the panel, which fills the  */
/* content area.                                                        */
/* ------------------------------------------------------------------ */

void archie_module_resize(HWND content, int w, int h)
{
    const int margin = 16;
    const int gap    = 10;
    const int row_h  = 24;
    const int btn_h  = 28;
    const int btn_w  = 100;
    const int lh     = 18;   /* label / value line height */
    int r1_y = 14, r2_y = 48, r3_y = 82;
    int panes_lbl_y, panes_y, panes_bottom, list_h, col_w;
    int hosts_x, dirs_x, files_x;
    int detail_block_h = 108, gap_above_detail = 20, detail_top;
    int rx;

    if (!g_a_controls_created || !g_a_panel) return;

    /* Panel fills the content area. */
    MoveWindow(g_a_panel, 0, 0, w, h, TRUE);

    /* Row 1: Search term + Search button (right-anchored). */
    MoveWindow(g_a_search_lbl,  margin,      r1_y + 3, 56, lh, TRUE);
    MoveWindow(g_a_search_edit, margin + 60, r1_y,
               w - margin - 60 - gap - btn_w - margin, row_h, TRUE);
    MoveWindow(g_a_search_btn,  w - margin - btn_w, r1_y - 2, btn_w, btn_h, TRUE);

    /* Row 2: Archie Server + Port + static Protocol: UDP. */
    MoveWindow(g_a_server_lbl,  margin,       r2_y + 3, 110, lh, TRUE);
    MoveWindow(g_a_server_edit, margin + 114, r2_y, 210, row_h, TRUE);
    MoveWindow(g_a_port_lbl,    margin + 334, r2_y + 3, 36, lh, TRUE);
    MoveWindow(g_a_port_edit,   margin + 372, r2_y, 60, row_h, TRUE);
    MoveWindow(g_a_proto_lbl,   margin + 446, r2_y + 3, 74, lh, TRUE);
    MoveWindow(g_a_proto_val,   margin + 522, r2_y + 3, 60, lh, TRUE);

    /* Row 3: search type radios + Exact-first checkbox. */
    MoveWindow(g_a_type_lbl, margin, r3_y + 3, 92, lh, TRUE);
    rx = margin + 96;
    MoveWindow(g_a_type_substr,   rx, r3_y, 92,  22, TRUE); rx += 96;
    MoveWindow(g_a_type_substrcs, rx, r3_y, 240, 22, TRUE); rx += 244;
    MoveWindow(g_a_type_exact,    rx, r3_y, 62,  22, TRUE); rx += 66;
    MoveWindow(g_a_type_regex,    rx, r3_y, 162, 22, TRUE); rx += 172;
    MoveWindow(g_a_exactfirst_chk,rx, r3_y, 104, 22, TRUE);

    /* Detail block anchored to the bottom, with a blank line above it. */
    detail_top = h - margin - detail_block_h;
    if (detail_top < r3_y + 130) detail_top = r3_y + 130;

    /* Three result panes between row 3 and the detail block. */
    panes_lbl_y  = r3_y + 34;
    panes_y      = panes_lbl_y + 20;
    panes_bottom = detail_top - gap_above_detail;
    list_h = panes_bottom - panes_y;
    if (list_h < 60) list_h = 60;
    col_w = (w - 2 * margin - 2 * gap) / 3;
    if (col_w < 60) col_w = 60;
    hosts_x = margin;
    dirs_x  = margin + col_w + gap;
    files_x = margin + 2 * (col_w + gap);

    MoveWindow(g_a_hosts_lbl, hosts_x, panes_lbl_y, col_w, lh, TRUE);
    MoveWindow(g_a_dirs_lbl,  dirs_x,  panes_lbl_y, col_w, lh, TRUE);
    MoveWindow(g_a_files_lbl, files_x, panes_lbl_y, col_w, lh, TRUE);
    MoveWindow(g_a_hosts_list, hosts_x, panes_y, col_w, list_h, TRUE);
    MoveWindow(g_a_dirs_list,  dirs_x,  panes_y, col_w, list_h, TRUE);
    MoveWindow(g_a_files_list, files_x, panes_y, col_w, list_h, TRUE);

    /* Detail: plain text values (no boxes). The bottom-right corner holds
     * the Download button (aligned under Search: same X column and width);
     * the Host Address value is kept clear of that column. */
    {
        int dl_y = detail_top;
        int d0   = detail_top + 22;
        int d1   = d0 + 28;
        int d2   = d1 + 28;
        int btn_x    = w - margin - btn_w;      /* shared with Search */
        int host_r   = btn_x - gap;             /* host value right edge  */
        MoveWindow(g_a_detail_lbl, margin, dl_y, 200, lh, TRUE);

        MoveWindow(g_a_fname_lbl, margin,      d0, 80, lh, TRUE);
        MoveWindow(g_a_fname_val, margin + 88, d0, w - margin - (margin + 88), lh, TRUE);

        MoveWindow(g_a_size_lbl,  margin,       d1, 40,  lh, TRUE);
        MoveWindow(g_a_size_val,  margin + 44,  d1, 96,  lh, TRUE);
        MoveWindow(g_a_mode_lbl,  margin + 150, d1, 44,  lh, TRUE);
        MoveWindow(g_a_mode_val,  margin + 196, d1, 120, lh, TRUE);
        MoveWindow(g_a_date_lbl,  margin + 330, d1, 44,  lh, TRUE);
        MoveWindow(g_a_date_val,  margin + 376, d1, w - margin - (margin + 376), lh, TRUE);

        MoveWindow(g_a_host_lbl,  margin,       d2, 106, lh, TRUE);
        MoveWindow(g_a_host_val,  margin + 112, d2, host_r - (margin + 112), lh, TRUE);

        MoveWindow(g_a_download_btn, btn_x, h - margin - btn_h, btn_w, btn_h, TRUE);
    }

    /* Repaint the whole face so a resize never leaves stale text behind. */
    InvalidateRect(g_a_panel, NULL, TRUE);
    (void)content;
}

/* ------------------------------------------------------------------ */
/* Activate: create the panel + controls (once), else show them.       */
/* ------------------------------------------------------------------ */

void archie_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    if (g_a_controls_created) {
        ShowWindow(g_a_panel, SW_SHOW);
        SetFocus(g_a_search_edit);
        return;
    }

    archie_register_panel_class(hInst);

    /* No WS_EX_CONTROLPARENT: this panel is not a dialog-navigation
     * container, and having it under the non-control-parent content panel
     * created a WS_EX_CONTROLPARENT inconsistency that hung IsDialogMessage
     * in GetNextDlgTabItem. The shell skips IsDialogMessage for content
     * focus, so Archie controls are dispatched to directly (mouse, plus the
     * search-edit Enter subclass) exactly like the WAIS tab. */
    GetClientRect(content, &rc);
    g_a_panel = CreateWindowExA(0, ARCHIE_PANEL_CLASS, "",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        content, NULL, hInst, NULL);

    /* Row 1 */
    g_a_search_lbl = CreateWindowA("STATIC", "Search:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 56, 18, g_a_panel, NULL, hInst, NULL);
    g_a_search_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 200, 24, g_a_panel, (HMENU)(INT_PTR)IDC_A_SEARCH_EDIT, hInst, NULL);
    g_a_search_btn = CreateWindowA("BUTTON", "Search",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        0, 0, 100, 28, g_a_panel, (HMENU)(INT_PTR)IDC_A_SEARCH_BTN, hInst, NULL);

    /* Row 2 */
    g_a_server_lbl = CreateWindowA("STATIC", "Archie Server:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 96, 18, g_a_panel, NULL, hInst, NULL);
    g_a_server_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_a_active_server,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 210, 24, g_a_panel, (HMENU)(INT_PTR)IDC_A_SERVER_EDIT, hInst, NULL);
    g_a_port_lbl = CreateWindowA("STATIC", "Port:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 36, 18, g_a_panel, NULL, hInst, NULL);
    {
        char pbuf[16];
        _snprintf(pbuf, sizeof pbuf, "%d", ARCHIE_DEFAULT_PORT);
        g_a_port_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", pbuf,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
            0, 0, 60, 24, g_a_panel, (HMENU)(INT_PTR)IDC_A_PORT_EDIT, hInst, NULL);
    }
    g_a_proto_lbl = CreateWindowA("STATIC", "Protocol:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 66, 18, g_a_panel, NULL, hInst, NULL);
    g_a_proto_val = CreateWindowA("STATIC", "UDP",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 60, 18, g_a_panel, NULL, hInst, NULL);

    /* Row 3: search type + Exact-first. Regular Expression is a real,
     * enabled option now (the server answers R with ed(1)/BSD regex). */
    g_a_type_lbl = CreateWindowA("STATIC", "Search Type:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 92, 18, g_a_panel, NULL, hInst, NULL);
    g_a_type_substr = CreateWindowA("BUTTON", "Substring",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
        0, 0, 92, 22, g_a_panel, (HMENU)(INT_PTR)IDC_A_TYPE_SUBSTR, hInst, NULL);
    g_a_type_substrcs = CreateWindowA("BUTTON", "Substring (case sensitive)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
        0, 0, 210, 22, g_a_panel, (HMENU)(INT_PTR)IDC_A_TYPE_SUBSTRCS, hInst, NULL);
    g_a_type_exact = CreateWindowA("BUTTON", "Exact",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
        0, 0, 62, 22, g_a_panel, (HMENU)(INT_PTR)IDC_A_TYPE_EXACT, hInst, NULL);
    g_a_type_regex = CreateWindowA("BUTTON", "Regular Expression",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
        0, 0, 162, 22, g_a_panel, (HMENU)(INT_PTR)IDC_A_TYPE_REGEX, hInst, NULL);
    g_a_exactfirst_chk = CreateWindowA("BUTTON", "Exact first",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX,
        0, 0, 104, 22, g_a_panel, (HMENU)(INT_PTR)IDC_A_EXACTFIRST, hInst, NULL);

    /* Result panes. */
    g_a_hosts_lbl = CreateWindowA("STATIC", "Hosts",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 100, 18, g_a_panel, NULL, hInst, NULL);
    g_a_dirs_lbl = CreateWindowA("STATIC", "Directories",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 100, 18, g_a_panel, NULL, hInst, NULL);
    g_a_files_lbl = CreateWindowA("STATIC", "Files",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 100, 18, g_a_panel, NULL, hInst, NULL);
    g_a_hosts_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        0, 0, 100, 100, g_a_panel, (HMENU)(INT_PTR)IDC_A_HOSTS_LIST, hInst, NULL);
    g_a_dirs_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        0, 0, 100, 100, g_a_panel, (HMENU)(INT_PTR)IDC_A_DIRS_LIST, hInst, NULL);
    g_a_files_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        0, 0, 100, 100, g_a_panel, (HMENU)(INT_PTR)IDC_A_FILES_LIST, hInst, NULL);

    /* Detail block: plain-text values (no field boxes). */
    g_a_detail_lbl = CreateWindowA("STATIC", "Selected file:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 200, 18, g_a_panel, NULL, hInst, NULL);
    g_a_fname_lbl = CreateWindowA("STATIC", "File Name:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 80, 18, g_a_panel, NULL, hInst, NULL);
    g_a_fname_val = CreateWindowA("STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 200, 18, g_a_panel, (HMENU)(INT_PTR)IDC_A_FNAME_VAL, hInst, NULL);
    g_a_size_lbl = CreateWindowA("STATIC", "Size:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 40, 18, g_a_panel, NULL, hInst, NULL);
    g_a_size_val = CreateWindowA("STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 96, 18, g_a_panel, (HMENU)(INT_PTR)IDC_A_SIZE_VAL, hInst, NULL);
    g_a_mode_lbl = CreateWindowA("STATIC", "Mode:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 44, 18, g_a_panel, NULL, hInst, NULL);
    g_a_mode_val = CreateWindowA("STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 120, 18, g_a_panel, (HMENU)(INT_PTR)IDC_A_MODE_VAL, hInst, NULL);
    g_a_date_lbl = CreateWindowA("STATIC", "Date:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 44, 18, g_a_panel, NULL, hInst, NULL);
    g_a_date_val = CreateWindowA("STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 200, 18, g_a_panel, (HMENU)(INT_PTR)IDC_A_DATE_VAL, hInst, NULL);
    g_a_host_lbl = CreateWindowA("STATIC", "Host Address:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 96, 18, g_a_panel, NULL, hInst, NULL);
    g_a_host_val = CreateWindowA("STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 200, 18, g_a_panel, (HMENU)(INT_PTR)IDC_A_HOST_VAL, hInst, NULL);

    /* Download button: bottom-right column, aligned under Search. Standard
     * 3.1 push button (Search stays the default). Disabled until a file is
     * selected. */
    g_a_download_btn = CreateWindowA("BUTTON", "Download",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
        0, 0, 100, 28, g_a_panel, (HMENU)(INT_PTR)IDC_A_DOWNLOAD_BTN, hInst, NULL);

    /* One UI font for the whole 3.1-style dialog. */
    if (g_hFontUI) {
        HWND all[] = {
            g_a_search_lbl, g_a_search_edit, g_a_search_btn,
            g_a_server_lbl, g_a_server_edit, g_a_port_lbl, g_a_port_edit,
            g_a_proto_lbl, g_a_proto_val, g_a_type_lbl,
            g_a_type_substr, g_a_type_substrcs, g_a_type_exact, g_a_type_regex,
            g_a_exactfirst_chk, g_a_hosts_lbl, g_a_dirs_lbl, g_a_files_lbl,
            g_a_hosts_list, g_a_dirs_list, g_a_files_list, g_a_detail_lbl,
            g_a_fname_val, g_a_size_val, g_a_mode_val, g_a_date_val,
            g_a_host_val, g_a_download_btn };
        int k;
        for (k = 0; k < (int)(sizeof all / sizeof all[0]); k++)
            SendMessageA(all[k], WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }

    /* The five fixed detail labels render bold (values stay regular). Bold
     * variant derived from g_hFontUI so it matches the face/size exactly. */
    if (g_hFontUI && !g_a_bold_font) {
        LOGFONTA lf;
        if (GetObjectA(g_hFontUI, sizeof lf, &lf) == sizeof lf) {
            lf.lfWeight = FW_BOLD;
            g_a_bold_font = CreateFontIndirectA(&lf);
        }
    }
    {
        HFONT lblfont = g_a_bold_font ? g_a_bold_font : g_hFontUI;
        HWND labels[] = { g_a_fname_lbl, g_a_size_lbl, g_a_mode_lbl,
                          g_a_date_lbl, g_a_host_lbl };
        int k;
        for (k = 0; k < (int)(sizeof labels / sizeof labels[0]); k++)
            SendMessageA(labels[k], WM_SETFONT, (WPARAM)lblfont, TRUE);
    }

    g_a_orig_search_proc = (WNDPROC)SetWindowLongPtrA(
        g_a_search_edit, GWLP_WNDPROC, (LONG_PTR)ArchieSearchSub);

    CheckRadioButton(g_a_panel, IDC_A_TYPE_SUBSTR, IDC_A_TYPE_REGEX,
                     IDC_A_TYPE_SUBSTR);
    g_a_type = ARCHIE_SUBSTR_CI;
    g_a_exact_first = FALSE;
    archie_update_exactfirst_enable();

    GetClientRect(content, &rc);
    archie_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);

    g_a_controls_created = TRUE;
    SetFocus(g_a_search_edit);
}

/* ------------------------------------------------------------------ */
/* Deactivate: hide the panel (preserves all state).                   */
/* ------------------------------------------------------------------ */

void archie_module_deactivate(HWND content)
{
    (void)content;
    if (g_a_panel) ShowWindow(g_a_panel, SW_HIDE);
}

BOOL archie_module_has_unsaved(void)
{
    return FALSE;
}
