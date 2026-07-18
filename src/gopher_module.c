/*
 * gopher_module.c - F3 Gopher client. Plain RFC 1436 over TCP.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Layout mirrors the F2 polish-pass shape: URL row at y=14, nav buttons
 * (Back/Forward/Home/Stop/Refresh) at y=50, render area at y=88.
 * The render area is a custom child window (class "MGTGopherRenderV1")
 * which paints rows of [icon glyph][display] for the current gophermap,
 * or a plain monospace view for type-0 text replies.
 *
 * Click dispatch:
 *   '1' -> push back, fetch as MENU, re-render
 *   '0' -> push back, fetch as TEXT, switch to text-view mode
 *   'g' -> fetch as BINARY, open side image viewer (gdi+)
 *   '9' -> selector .mp3/.au -> audio_service_play_bytes; else SaveAs
 *   's' -> fetch as BINARY, audio_service_play_bytes
 *   'i' -> no-op
 *
 * Fetch happens on a worker thread; results return via PostMessage.
 */

#include "gopher_module.h"
#include "gopher_protocol.h"
#include "gophermap.h"
#include "gopher_search_dialog.h"
#include "audio_service.h"
#include "suite_clipboard.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <objbase.h>
#include <ole2.h>
#include <shlwapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- gdi+ flat C API (matches web_module's vendored declarations) ---------- */

typedef struct MgtGdiplusStartupInput_ {
    UINT32  GdiplusVersion;
    void   *DebugEventCallback;
    BOOL    SuppressBackgroundThread;
    BOOL    SuppressExternalCodecs;
} MgtGdiplusStartupInput;

extern int  WINAPI GdiplusStartup(ULONG_PTR *token,
                                  const MgtGdiplusStartupInput *input,
                                  void *output);
extern void WINAPI GdiplusShutdown(ULONG_PTR token);
extern int  WINAPI GdipCreateBitmapFromStream(IStream *stream, void **bitmap);
extern int  WINAPI GdipCreateHBITMAPFromBitmap(void *bitmap,
                                               HBITMAP *hbmReturn,
                                               DWORD background);
extern int  WINAPI GdipDisposeImage(void *image);
extern int  WINAPI GdipGetImageWidth(void *image, UINT *width);
extern int  WINAPI GdipGetImageHeight(void *image, UINT *height);

/* ---------- constants ---------- */

#define GOPHER_DEFAULT_HOST   "gopher.greektimes.ca"
#define GOPHER_DEFAULT_PORT_  70
#define GOPHER_HOME_URL       "gopher://gopher.greektimes.ca/"

/* Bright white on dark for maximum legibility. Kept local; no shared
 * color header in this project. Numbers are locked here so a future
 * theme switch is a one-place change. */
#define GOPHER_COL_BG       RGB(0x1b, 0x1b, 0x1b)  /* view background        */
#define GOPHER_COL_FG       RGB(0xff, 0xff, 0xff)  /* main text (pure white) */
#define GOPHER_COL_LINK     RGB(0xff, 0xff, 0xff)  /* clickable item (white) */
#define GOPHER_COL_INFO     RGB(0xc0, 0xc0, 0xc0)  /* 'i' lines (dim silver) */
#define GOPHER_COL_HINT     RGB(0x80, 0x80, 0x80)  /* empty-state hint       */
#define GOPHER_COL_GUTTER   RGB(0x45, 0x45, 0x45)  /* future line-number gutter */
#define GOPHER_COL_SEL      RGB(0xcf, 0x9a, 0x0c)  /* reserved selection hi  */

#define IDC_GOPH_URL_LBL      4001
#define IDC_GOPH_URL_EDIT     4002
#define IDC_GOPH_GO_BTN       4003
#define IDC_GOPH_BACK_BTN     4004
#define IDC_GOPH_FWD_BTN      4005
#define IDC_GOPH_HOME_BTN     4006
#define IDC_GOPH_STOP_BTN     4007
#define IDC_GOPH_REFRESH_BTN  4008
#define IDC_GOPH_RENDER       4009

#define GOPHER_RENDER_CLASS   "MGTGopherRenderV1"
#define GOPHER_IMAGE_CLASS    "MGTGopherImageV1"

#define WM_GOPHER_FETCH_DONE  (WM_USER + 21)
/* wParam: 1=success, 0=failure
 * lParam: pointer to GopherFetchResult (caller owns; UI thread frees) */

/* F3 selection / clipboard (polish, 2026-05-22). Shares the menu IDs
 * with F6 because each module compiles its own copy and the IDs only
 * have to be unique within their own popup menu's WM_COMMAND. */
#define ID_CTX_COPY              2001
#define ID_CTX_PASTE             2002
#define ID_CTX_SELECT_ALL        2003
#define ID_TIMER_F3_SEL_SCROLL   91
#define F3_DRAG_THRESHOLD_PX     3

typedef struct GopherFetchResult {
    char            *url;             /* canonical fetched URL */
    char            *host;
    int              port;
    char             type;
    char            *selector;
    char            *display;         /* label of the clicked item, may be NULL */
    GopherFetchKind  kind;
    char            *bytes;           /* malloc'd; UI thread frees */
    size_t           size;
    int              push_history;    /* 1 to push current url onto back stack */
    int              err_code;        /* 0 on success */
    char             err_msg[256];
} GopherFetchResult;

/* ---------- module state ---------- */

static HWND     g_hContent       = NULL;
static HWND     g_hUrlLbl        = NULL;
static HWND     g_hUrlEdit       = NULL;
static HWND     g_hGoBtn         = NULL;
static HWND     g_hBackBtn       = NULL;
static HWND     g_hFwdBtn        = NULL;
static HWND     g_hHomeBtn       = NULL;
static HWND     g_hStopBtn       = NULL;
static HWND     g_hRefreshBtn    = NULL;
static HWND     g_hRender        = NULL;

static BOOL     g_controls_created = FALSE;
static BOOL     g_render_class_reg = FALSE;
static BOOL     g_image_class_reg  = FALSE;
static BOOL     g_ole_inited       = FALSE;
static BOOL     g_gdiplus_inited   = FALSE;
static ULONG_PTR g_gdiplus_token   = 0;
static HFONT    g_hFontMono        = NULL;   /* Ubuntu Mono Regular for body / 'i' lines / text view */
static HFONT    g_hFontIcon        = NULL;   /* Segoe UI Emoji for icons (fallback chain) */
static HBRUSH   g_hBrushGopherBg   = NULL;   /* GOPHER_COL_BG, used by render proc */

/* Current view state: either a menu of items, or a text view. */
typedef enum {
    GVIEW_MENU = 0,
    GVIEW_TEXT = 1
} GopherViewMode;

static GopherViewMode g_view_mode = GVIEW_MENU;
static GopherItem    *g_items     = NULL;
static int            g_items_n   = 0;
static char          *g_text_body = NULL;     /* type-0 text body, NUL-terminated */
static size_t         g_text_n    = 0;

static char          *g_current_url = NULL;

/* Per-module history stacks (LIFO arrays of malloc'd URL strings). */
#define GOPHER_HISTORY_MAX 64
static char *g_back_stack[GOPHER_HISTORY_MAX];
static int   g_back_n = 0;
static char *g_fwd_stack[GOPHER_HISTORY_MAX];
static int   g_fwd_n = 0;

/* Worker thread state. */
static HANDLE g_fetch_thread = NULL;
static volatile LONG g_fetch_in_flight = 0;

/* Row metric cache. Computed in WM_PAINT once per redraw via GetTextMetrics. */
static int g_row_h    = 22;
static int g_icon_w   = 28;
static int g_indent_i = 28;

/* Vertical scroll. */
static int g_scroll_y = 0;

/* Selection state (polish, 2026-05-22). Cols are UTF-8 byte indices
 * into the row's displayable text. Selection follows the same line-
 * flow rules as vt_term (xterm-style, half-open at end_col). */
static int  g_sel_active        = 0;
static int  g_sel_anchor_line   = 0;
static int  g_sel_anchor_col    = 0;
static int  g_sel_lead_line     = 0;
static int  g_sel_lead_col      = 0;

/* Click-vs-drag disambiguation. WM_LBUTTONDOWN does NOT navigate; the
 * caller defers to WM_LBUTTONUP, which navigates only if no drag
 * crossed F3_DRAG_THRESHOLD_PX. */
static BOOL g_f3_lbutton_down   = FALSE;
static BOOL g_f3_drag_active    = FALSE;
static int  g_f3_down_x_px      = 0;
static int  g_f3_down_y_px      = 0;
static BOOL g_f3_scroll_armed   = FALSE;
static int  g_f3_scroll_dir     = 0;

/* Cached brush for selection background. Created lazily, destroyed
 * in gopher_module_shutdown. */
static HBRUSH g_f3_sel_brush    = NULL;

/* Image viewer chain: a freshly opened viewer prepends itself; on shutdown
 * we walk and destroy. */
typedef struct GopherImageWin {
    HWND                    hwnd;
    HBITMAP                 hbm;
    int                     bw;
    int                     bh;
    struct GopherImageWin  *next;
} GopherImageWin;
static GopherImageWin *g_image_chain = NULL;

/* ---------- forward decls ---------- */

static void gopher_navigate(const char *url, int push_history,
                            const char *display_for_status);
static void gopher_render_set_menu(GopherItem *items, int n);
static void gopher_render_set_text(char *body, size_t n);
static void gopher_render_clear(void);
static void gopher_update_url_bar(const char *url);
static void gopher_update_nav_buttons(void);
static void gopher_open_image_viewer(const char *title,
                                     const char *bytes, size_t n);
static int  gopher_save_binary_dialog(HWND owner, const char *suggested,
                                      const char *bytes, size_t n);
static void gopher_dispatch_item(int idx);
static char *gopher_dup(const char *s);
static char *gopher_normalize_url(const char *raw);

/* Selection (polish, 2026-05-22). */
static int   gopher_total_lines(void);
static int   gopher_line_text(int line_idx, const char **out_ptr, size_t *out_len);
static void  gopher_select_clear(void);
static void  gopher_select_set(int a_line, int a_col, int l_line, int l_col);
static void  gopher_select_all(void);
static int   gopher_cell_selected(int line, int col);
static char *gopher_select_extract(void);
static void  gopher_module_copy_selection(void);
static void  pixel_to_line_col(HWND hwnd, int x_px, int y_px,
                               int *out_line, int *out_col);
static void  arm_f3_sel_scroll_timer(HWND hwnd, int dir);
static void  kill_f3_sel_scroll_timer(HWND hwnd);
static void  f3_scroll_by_lines(HWND hwnd, int dlines);
static void  f3_handle_click_navigate(int x_px, int y_px);

/* ---------- small utilities ---------- */

static char *gopher_dup(const char *s)
{
    size_t n;
    char  *p;
    if (!s) return NULL;
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t sl, suf;
    size_t i;
    if (!s || !suffix) return 0;
    sl  = strlen(s);
    suf = strlen(suffix);
    if (suf > sl) return 0;
    for (i = 0; i < suf; i++) {
        char a = s[sl - suf + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Accept "gopher://...", bare "host", "host/path", "host:port/...". */
static char *gopher_normalize_url(const char *raw)
{
    if (!raw || !raw[0]) return gopher_dup(GOPHER_HOME_URL);
    /* Already a gopher URL? */
    if (strncmp(raw, "gopher://", 9) == 0 ||
        strncmp(raw, "GOPHER://", 9) == 0)
        return gopher_dup(raw);
    /* Otherwise prepend the scheme. */
    {
        size_t n = strlen(raw) + 9 + 1;
        char  *out = (char *)malloc(n);
        if (!out) return NULL;
        _snprintf(out, n, "gopher://%s", raw);
        return out;
    }
}

/* ---------- status + URL bar ---------- */

static void gopher_set_status(const char *text)
{
    suite_set_status(text);
}

static void gopher_update_url_bar(const char *url)
{
    if (g_hUrlEdit && url) SetWindowTextA(g_hUrlEdit, url);
}

/* TRUE when a selector has loaded and there is something on the canvas
 * (a menu or a text body). Single source of truth shared by the empty-
 * state hint in WM_PAINT and the Refresh button enable logic below. */
static BOOL gopher_has_document(void)
{
    return (g_view_mode == GVIEW_TEXT && g_text_body) ||
           (g_view_mode == GVIEW_MENU && g_items_n > 0);
}

static void gopher_update_nav_buttons(void)
{
    if (g_hBackBtn) EnableWindow(g_hBackBtn, g_back_n > 0);
    if (g_hFwdBtn)  EnableWindow(g_hFwdBtn,  g_fwd_n  > 0);
    if (g_hStopBtn) EnableWindow(g_hStopBtn, g_fetch_in_flight != 0);
    /* Refresh re-loads the current selector, so it is only meaningful
     * once a document is on the canvas. Disabled in the blank state and
     * whenever navigation (e.g. Back) lands back on it. */
    if (g_hRefreshBtn) EnableWindow(g_hRefreshBtn, gopher_has_document());
}

/* ---------- history ---------- */

static void history_push_back(char *url)
{
    int i;
    if (!url) return;
    if (g_back_n >= GOPHER_HISTORY_MAX) {
        /* Drop oldest. */
        free(g_back_stack[0]);
        for (i = 1; i < GOPHER_HISTORY_MAX; i++)
            g_back_stack[i - 1] = g_back_stack[i];
        g_back_n--;
    }
    g_back_stack[g_back_n++] = url;
}

static void history_clear_fwd(void)
{
    int i;
    for (i = 0; i < g_fwd_n; i++) free(g_fwd_stack[i]);
    g_fwd_n = 0;
}

/* ---------- selection model (polish, 2026-05-22) ---------- */

/* Total renderable lines in the current view. Selection coordinates
 * use line indices in this space (NOT raw entry indices, which would
 * confuse text-view line splitting). */
static int gopher_total_lines(void)
{
    if (g_view_mode == GVIEW_MENU) {
        return g_items_n;
    }
    if (g_view_mode == GVIEW_TEXT && g_text_body) {
        int count = 1;
        size_t i;
        for (i = 0; i < g_text_n; i++)
            if (g_text_body[i] == '\n') count++;
        /* Trailing newline does not create an extra empty visible line
         * the user can select from -- snip it. */
        if (g_text_n > 0 && g_text_body[g_text_n - 1] == '\n') count--;
        if (count < 0) count = 0;
        return count;
    }
    return 0;
}

/* Look up the displayable text for a given line. For GVIEW_MENU the
 * text is the entry's `display` (icon glyph is render-only, not part
 * of the selectable text). For GVIEW_TEXT the text is the byte range
 * between two consecutive \n's. The CR before an LF is trimmed.
 *
 * Returns 1 on success; *out_ptr / *out_len point into existing
 * caller-owned storage and stay valid as long as the view does not
 * mutate. Returns 0 on out-of-range line_idx. */
static int gopher_line_text(int line_idx, const char **out_ptr, size_t *out_len)
{
    if (g_view_mode == GVIEW_MENU) {
        const char *s;
        if (line_idx < 0 || line_idx >= g_items_n) return 0;
        s = g_items[line_idx].display ? g_items[line_idx].display : "";
        *out_ptr = s;
        *out_len = strlen(s);
        return 1;
    }
    if (g_view_mode == GVIEW_TEXT && g_text_body) {
        int    cur = 0;
        size_t line_start = 0;
        size_t i;
        for (i = 0; i <= g_text_n; i++) {
            if (i == g_text_n || g_text_body[i] == '\n') {
                if (cur == line_idx) {
                    size_t end = i;
                    if (end > line_start && g_text_body[end - 1] == '\r') end--;
                    *out_ptr = g_text_body + line_start;
                    *out_len = end - line_start;
                    return 1;
                }
                cur++;
                line_start = i + 1;
            }
        }
    }
    return 0;
}

static void gopher_select_clear(void)
{
    g_sel_active      = 0;
    g_sel_anchor_line = 0;
    g_sel_anchor_col  = 0;
    g_sel_lead_line   = 0;
    g_sel_lead_col    = 0;
}

static void gopher_select_set(int a_line, int a_col, int l_line, int l_col)
{
    g_sel_anchor_line = a_line;
    g_sel_anchor_col  = a_col;
    g_sel_lead_line   = l_line;
    g_sel_lead_col    = l_col;
    g_sel_active      = 1;
}

static void gopher_select_all(void)
{
    int total = gopher_total_lines();
    const char *txt;
    size_t len = 0;
    if (total <= 0) return;
    g_sel_anchor_line = 0;
    g_sel_anchor_col  = 0;
    g_sel_lead_line   = total - 1;
    if (gopher_line_text(total - 1, &txt, &len)) {
        g_sel_lead_col = (int)len;     /* half-open end-of-line */
    } else {
        g_sel_lead_col = 0;
    }
    g_sel_active = 1;
}

/* Normalize anchor / lead into (start, end) in reading order. */
static void sel_normalize(int *sr, int *sc, int *er, int *ec)
{
    int ar = g_sel_anchor_line, ac = g_sel_anchor_col;
    int lr = g_sel_lead_line,   lc = g_sel_lead_col;
    if (ar < lr || (ar == lr && ac <= lc)) {
        *sr = ar; *sc = ac; *er = lr; *ec = lc;
    } else {
        *sr = lr; *sc = lc; *er = ar; *ec = ac;
    }
}

static int gopher_cell_selected(int line, int col)
{
    int sr, sc, er, ec;
    if (!g_sel_active) return 0;
    sel_normalize(&sr, &sc, &er, &ec);
    if (line < sr || line > er) return 0;
    if (sr == er)        return (col >= sc && col < ec);
    if (line == sr)      return (col >= sc);
    if (line == er)      return (col <  ec);
    return 1;                          /* middle line: full row */
}

/* Snap a UTF-8 byte index forward to the next codepoint boundary.
 * UTF-8 continuation bytes have the high two bits set to 10. */
static int utf8_snap_forward(const char *s, size_t len, int idx)
{
    if (idx < 0) return 0;
    if ((size_t)idx >= len) return (int)len;
    while ((size_t)idx < len && ((unsigned char)s[idx] & 0xC0) == 0x80) idx++;
    return idx;
}

static char *gopher_select_extract(void)
{
    int sr, sc, er, ec;
    char *buf;
    size_t cap, used;
    int line;

    if (!g_sel_active) return NULL;
    sel_normalize(&sr, &sc, &er, &ec);
    if (sr == er && sc == ec) return NULL;

    cap  = 256;
    used = 0;
    buf  = (char *)malloc(cap);
    if (!buf) return NULL;

    for (line = sr; line <= er; line++) {
        const char *txt = NULL;
        size_t      tlen = 0;
        int         col_lo, col_hi;
        size_t      line_end;

        if (!gopher_line_text(line, &txt, &tlen)) {
            txt  = "";
            tlen = 0;
        }
        col_lo = (line == sr) ? sc : 0;
        col_hi = (line == er) ? ec : (int)tlen;
        if (col_lo < 0) col_lo = 0;
        if (col_hi > (int)tlen) col_hi = (int)tlen;
        col_lo = utf8_snap_forward(txt, tlen, col_lo);
        col_hi = utf8_snap_forward(txt, tlen, col_hi);
        if (col_hi < col_lo) col_hi = col_lo;

        line_end = used;
        if (col_hi > col_lo) {
            size_t span = (size_t)(col_hi - col_lo);
            if (used + span + 4 >= cap) {
                size_t ncap = cap;
                while (ncap < used + span + 4) ncap *= 2;
                {
                    char *nbuf = (char *)realloc(buf, ncap);
                    if (!nbuf) { free(buf); return NULL; }
                    buf = nbuf; cap = ncap;
                }
            }
            memcpy(buf + used, txt + col_lo, span);
            used += span;
        }
        /* Trim trailing spaces on this line. */
        while (used > line_end && buf[used - 1] == ' ') used--;

        if (line < er) {
            if (used + 4 >= cap) {
                size_t ncap = cap * 2;
                char *nbuf = (char *)realloc(buf, ncap);
                if (!nbuf) { free(buf); return NULL; }
                buf = nbuf; cap = ncap;
            }
            buf[used++] = '\r';
            buf[used++] = '\n';
        }
    }

    if (used + 1 >= cap) {
        char *nbuf = (char *)realloc(buf, used + 1);
        if (!nbuf) { free(buf); return NULL; }
        buf = nbuf;
    }
    buf[used] = '\0';
    return buf;
}

/* ---------- render area: model swap ---------- */

static void gopher_render_clear(void)
{
    if (g_items)     { gophermap_free(g_items, g_items_n); g_items = NULL; g_items_n = 0; }
    if (g_text_body) { free(g_text_body); g_text_body = NULL; g_text_n = 0; }
    /* Selection points at content that is about to be discarded. */
    gopher_select_clear();
}

static void gopher_render_set_menu(GopherItem *items, int n)
{
    gopher_render_clear();
    g_items     = items;
    g_items_n   = n;
    g_view_mode = GVIEW_MENU;
    g_scroll_y  = 0;
    if (g_hRender) {
        SCROLLINFO si;
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask  = SIF_POS;
        si.nPos   = 0;
        SetScrollInfo(g_hRender, SB_VERT, &si, TRUE);
        InvalidateRect(g_hRender, NULL, TRUE);
    }
}

static void gopher_render_set_text(char *body, size_t n)
{
    gopher_render_clear();
    g_text_body = body;
    g_text_n    = n;
    g_view_mode = GVIEW_TEXT;
    g_scroll_y  = 0;
    if (g_hRender) {
        SCROLLINFO si;
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask  = SIF_POS;
        si.nPos   = 0;
        SetScrollInfo(g_hRender, SB_VERT, &si, TRUE);
        InvalidateRect(g_hRender, NULL, TRUE);
    }
}

/* ---------- icon glyphs ---------- */

static const wchar_t *gopher_icon_for(char type)
{
    switch (type) {
    case '1': return L"\xD83D\xDCC1";  /* U+1F4C1 folder */
    case '0': return L"\xD83D\xDCC4";  /* U+1F4C4 document */
    case '7': return L"\xD83D\xDD0D";  /* U+1F50D magnifying glass (search) */
    case 'g': return L"\xD83D\xDDBC";  /* U+1F5BC frame */
    case '9': return L"\xD83D\xDCBE";  /* U+1F4BE floppy */
    case 's': return L"\xD83D\xDD0A";  /* U+1F50A speaker */
    case 'I': return L"\xD83D\xDDBC";  /* same as 'g' */
    case 'h': return L"\xD83D\xDD17";  /* U+1F517 link */
    default:  return L"";              /* informational / unknown */
    }
}

static int gopher_type_is_clickable(char t)
{
    return t == '1' || t == '0' || t == '7' || t == 'g' || t == '9' ||
           t == 's' || t == 'I' || t == 'h';
}

/* Render a UTF-8 span at (x, y) with the given text color. Returns the
 * pixel width consumed so callers can advance x for the next piece. */
static int gopher_text_emit_utf8(HDC hdc, int x, int y,
                                 const char *utf8, size_t n,
                                 COLORREF text_color, BOOL transparent)
{
    int wn;
    SIZE sz;
    sz.cx = 0;
    if (!utf8 || n == 0) return 0;
    wn = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n, NULL, 0);
    if (wn <= 0) return 0;
    {
        wchar_t *wbuf = (wchar_t *)malloc((size_t)(wn + 1) * sizeof(wchar_t));
        if (!wbuf) return 0;
        MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n, wbuf, wn);
        wbuf[wn] = 0;
        SetTextColor(hdc, text_color);
        if (transparent) SetBkMode(hdc, TRANSPARENT);
        TextOutW(hdc, x, y, wbuf, wn);
        GetTextExtentPoint32W(hdc, wbuf, wn, &sz);
        free(wbuf);
    }
    return sz.cx;
}

/* Render a single rendered-line's text starting at (x, y), splitting
 * into pre-sel / sel / post-sel runs based on the active selection.
 * normal_color is used for the unselected portions; selected portions
 * always render dark on light (Gophie palette swap) so the inversion
 * is uniform regardless of underlying link / info color. */
static void gopher_paint_line_with_selection(HDC hdc, int x, int y,
                                             const char *utf8, size_t n,
                                             int line_idx,
                                             COLORREF normal_color)
{
    int sel_start = 0, sel_end = 0;
    int has_sel = 0;
    int cx = x;

    if (g_sel_active) {
        int sr, sc, er, ec;
        sel_normalize(&sr, &sc, &er, &ec);
        if (line_idx >= sr && line_idx <= er) {
            int lo = (line_idx == sr) ? sc : 0;
            int hi = (line_idx == er) ? ec : (int)n;
            if (lo < 0) lo = 0;
            if (hi > (int)n) hi = (int)n;
            lo = utf8_snap_forward(utf8, n, lo);
            hi = utf8_snap_forward(utf8, n, hi);
            if (hi > lo) {
                sel_start = lo;
                sel_end   = hi;
                has_sel   = 1;
            }
        }
    }

    if (!has_sel) {
        gopher_text_emit_utf8(hdc, cx, y, utf8, n, normal_color, TRUE);
        return;
    }

    if (sel_start > 0) {
        cx += gopher_text_emit_utf8(hdc, cx, y, utf8, sel_start,
                                    normal_color, TRUE);
    }
    if (sel_end > sel_start) {
        size_t span = (size_t)(sel_end - sel_start);
        int wn = MultiByteToWideChar(CP_UTF8, 0, utf8 + sel_start, (int)span, NULL, 0);
        int span_px = 0;
        wchar_t *wbuf = NULL;
        if (wn > 0) {
            wbuf = (wchar_t *)malloc((size_t)(wn + 1) * sizeof(wchar_t));
            if (wbuf) {
                SIZE sz;
                MultiByteToWideChar(CP_UTF8, 0, utf8 + sel_start, (int)span, wbuf, wn);
                wbuf[wn] = 0;
                GetTextExtentPoint32W(hdc, wbuf, wn, &sz);
                span_px = sz.cx;
            }
        }
        if (span_px > 0 && g_f3_sel_brush) {
            RECT rs;
            rs.left   = cx;
            rs.top    = y;
            rs.right  = cx + span_px;
            rs.bottom = y + g_row_h;
            FillRect(hdc, &rs, g_f3_sel_brush);
        }
        if (wbuf) {
            SetTextColor(hdc, GOPHER_COL_BG);
            SetBkMode   (hdc, TRANSPARENT);
            TextOutW(hdc, cx, y, wbuf, wn);
            free(wbuf);
        }
        cx += span_px;
    }
    if (sel_end < (int)n) {
        gopher_text_emit_utf8(hdc, cx, y, utf8 + sel_end, n - sel_end,
                              normal_color, TRUE);
    }
}

/* ---------- render area: WM_PAINT ---------- */

static int gopher_total_height(void)
{
    if (g_view_mode == GVIEW_TEXT) {
        /* Count lines in text. */
        int lines = 1;
        size_t i;
        if (!g_text_body) return g_row_h;
        for (i = 0; i < g_text_n; i++) if (g_text_body[i] == '\n') lines++;
        return lines * g_row_h + 16;
    } else {
        if (g_items_n <= 0) return g_row_h;
        return g_items_n * g_row_h + 16;
    }
}

static void gopher_update_scroll(HWND hwnd)
{
    SCROLLINFO si;
    RECT       rc;
    int        client_h, total_h;

    GetClientRect(hwnd, &rc);
    client_h = rc.bottom - rc.top;
    total_h  = gopher_total_height();

    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin   = 0;
    si.nMax   = total_h > 0 ? total_h - 1 : 0;
    si.nPage  = (UINT)client_h;
    si.nPos   = g_scroll_y;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

static void gopher_paint_menu(HDC hdc, RECT *rc)
{
    int  y_base;
    int  i;
    HFONT old_font;
    TEXTMETRIC tm;
    int  line_y;

    old_font = (HFONT)SelectObject(hdc, g_hFontMono ? g_hFontMono : g_hFontUI);
    GetTextMetrics(hdc, &tm);
    g_row_h    = tm.tmHeight + 4;
    g_icon_w   = tm.tmAveCharWidth * 3;
    g_indent_i = tm.tmAveCharWidth * 3;

    SetBkMode(hdc, TRANSPARENT);

    y_base = -g_scroll_y + 8;

    for (i = 0; i < g_items_n; i++) {
        line_y = y_base + i * g_row_h;
        if (line_y + g_row_h < 0) continue;
        if (line_y > rc->bottom) break;
        {
            GopherItem *it = &g_items[i];
            COLORREF text_col;
            const char *disp = it->display ? it->display : "";
            size_t      dl   = strlen(disp);

            switch (it->type) {
            case 'i': text_col = GOPHER_COL_INFO; break;
            case '1':
            case '0':
            case '7':
            case 'g':
            case 'I':
            case '9':
            case 's':
            case 'h': text_col = GOPHER_COL_LINK; break;
            default:  text_col = GOPHER_COL_FG;   break;
            }
            SetTextColor(hdc, text_col);

            if (it->type != 'i' && gopher_type_is_clickable(it->type)) {
                const wchar_t *icon = gopher_icon_for(it->type);
                int icon_len = (int)wcslen(icon);
                HFONT prev = NULL;
                if (icon_len > 0 && g_hFontIcon) {
                    prev = (HFONT)SelectObject(hdc, g_hFontIcon);
                    TextOutW(hdc, 8, line_y, icon, icon_len);
                    SelectObject(hdc, prev);
                }
                /* Display label via the selection-aware emitter so any
                 * active drag-select renders inverted runs in place.
                 * The icon glyph is NOT part of the selectable text. */
                gopher_paint_line_with_selection(hdc,
                    8 + g_icon_w, line_y, disp, dl, i, text_col);
            } else {
                /* 'i' info line: indented, no icon. */
                gopher_paint_line_with_selection(hdc,
                    8 + g_indent_i, line_y, disp, dl, i, text_col);
            }
        }
    }

    SelectObject(hdc, old_font);
}

static void gopher_paint_text(HDC hdc, RECT *rc)
{
    HFONT       old_font;
    TEXTMETRIC  tm;
    int         y_base;
    size_t      i;
    size_t      line_start;
    int         line_idx = 0;

    if (!g_text_body) return;

    old_font = (HFONT)SelectObject(hdc, g_hFontMono ? g_hFontMono : g_hFontUI);
    GetTextMetrics(hdc, &tm);
    g_row_h = tm.tmHeight + 2;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GOPHER_COL_FG);

    y_base = -g_scroll_y + 8;
    line_start = 0;
    for (i = 0; i <= g_text_n; i++) {
        if (i == g_text_n || g_text_body[i] == '\n') {
            size_t line_len = i - line_start;
            int    line_y   = y_base + line_idx * g_row_h;
            if (line_len > 0 && g_text_body[line_start + line_len - 1] == '\r')
                line_len--;
            if (line_y + g_row_h >= 0 && line_y <= rc->bottom) {
                /* Selection-aware emit (no-op fast path when there is no
                 * active selection on this line). */
                gopher_paint_line_with_selection(hdc, 8, line_y,
                    g_text_body + line_start, line_len, line_idx,
                    GOPHER_COL_FG);
            }
            line_idx++;
            line_start = i + 1;
        }
    }
    SelectObject(hdc, old_font);
}

/* ---------- click + hover ---------- */

static int gopher_item_at_point(int x, int y)
{
    int row;
    (void)x;
    if (g_view_mode != GVIEW_MENU || g_items_n <= 0 || g_row_h <= 0) return -1;
    row = (y + g_scroll_y - 8) / g_row_h;
    if (row < 0 || row >= g_items_n) return -1;
    return row;
}

/* Translate pixel coordinates inside the render hwnd to (line, col),
 * where col is a UTF-8 byte index into that line's displayable text.
 * Used by the polish-selection drag path. Out-of-range pixel y clamps
 * to the first / last line; out-of-range x clamps to [0, line_len]. */
static void pixel_to_line_col(HWND hwnd, int x_px, int y_px,
                              int *out_line, int *out_col)
{
    int total_lines = gopher_total_lines();
    int line;
    const char *txt = NULL;
    size_t tlen = 0;
    int avg_w, x_text;
    int col;

    (void)hwnd;
    if (total_lines <= 0 || g_row_h <= 0) { *out_line = 0; *out_col = 0; return; }

    line = (y_px + g_scroll_y - 8) / g_row_h;
    if (line < 0)             line = 0;
    if (line >= total_lines)  line = total_lines - 1;
    *out_line = line;

    if (!gopher_line_text(line, &txt, &tlen)) { *out_col = 0; return; }

    /* Approximate column by average char width. Text view renders at
     * x=8; menu mode indents past the icon for clickable items or
     * past g_indent_i for 'i' lines. */
    if (g_view_mode == GVIEW_MENU && line >= 0 && line < g_items_n) {
        char t = g_items[line].type;
        if (t == 'i') x_text = 8 + g_indent_i;
        else          x_text = 8 + g_icon_w;
    } else {
        x_text = 8;
    }
    avg_w = (g_icon_w > 0) ? (g_icon_w / 3) : 9;
    if (avg_w <= 0) avg_w = 9;

    col = (x_px - x_text) / avg_w;
    if (col < 0)            col = 0;
    if (col > (int)tlen)    col = (int)tlen;
    col = utf8_snap_forward(txt, tlen, col);
    *out_col = col;
}

static void arm_f3_sel_scroll_timer(HWND hwnd, int dir)
{
    g_f3_scroll_dir = dir;
    if (!g_f3_scroll_armed && hwnd) {
        SetTimer(hwnd, ID_TIMER_F3_SEL_SCROLL, 80, NULL);
        g_f3_scroll_armed = TRUE;
    }
}

static void kill_f3_sel_scroll_timer(HWND hwnd)
{
    if (g_f3_scroll_armed && hwnd) {
        KillTimer(hwnd, ID_TIMER_F3_SEL_SCROLL);
        g_f3_scroll_armed = FALSE;
    }
    g_f3_scroll_dir = 0;
}

static void f3_scroll_by_lines(HWND hwnd, int dlines)
{
    int total_h, page;
    RECT rc;
    int new_pos;
    GetClientRect(hwnd, &rc);
    page    = rc.bottom - rc.top;
    total_h = gopher_total_height();
    new_pos = g_scroll_y + dlines * g_row_h;
    if (new_pos < 0) new_pos = 0;
    if (new_pos > total_h - page) new_pos = total_h - page;
    if (new_pos < 0) new_pos = 0;
    g_scroll_y = new_pos;
    SetScrollPos(hwnd, SB_VERT, g_scroll_y, TRUE);
}

/* Existing inline navigation extracted so WM_LBUTTONUP can defer to
 * it when the click never crossed the drag threshold. */
static void f3_handle_click_navigate(int x_px, int y_px)
{
    int idx;
    if (g_view_mode != GVIEW_MENU) return;
    idx = gopher_item_at_point(x_px, y_px);
    if (idx >= 0) gopher_dispatch_item(idx);
}

static void gopher_module_copy_selection(void)
{
    char *text;
    if (!g_sel_active) return;
    text = gopher_select_extract();
    if (!text) return;
    suite_clipboard_put_text(g_hRender, text);
    free(text);
}

/* ---------- worker thread ---------- */

static DWORD WINAPI gopher_fetch_thread(LPVOID arg)
{
    GopherFetchResult *r = (GopherFetchResult *)arg;
    int rc;

    rc = gopher_fetch_sync(r->host, r->port, r->selector,
                           r->kind, &r->bytes, &r->size);
    if (rc != 0) {
        r->err_code = rc;
        _snprintf(r->err_msg, sizeof(r->err_msg),
                  "gopher_fetch_sync failed (code %d)", rc);
    }
    /* Route to the render window: suite_shell's ContentProc only
     * forwards WM_COMMAND, not arbitrary WM_USER messages, so any
     * non-command notifications must reach a HWND we own. */
    PostMessageA(g_hRender, WM_GOPHER_FETCH_DONE,
                 rc == 0 ? 1 : 0, (LPARAM)r);
    return 0;
}

static void gopher_kick_fetch(GopherFetchResult *r)
{
    if (InterlockedExchange(&g_fetch_in_flight, 1) != 0) {
        /* Already in flight: refuse. The worker thread will Post DONE
         * for the prior one and reset the flag. */
        free(r->url); free(r->host); free(r->selector); free(r->display);
        free(r);
        gopher_set_status("Gopher: a fetch is already in flight; press Stop first.");
        return;
    }
    gopher_update_nav_buttons();
    gopher_set_status("Connecting...");
    g_fetch_thread = CreateThread(NULL, 0, gopher_fetch_thread, r, 0, NULL);
    if (!g_fetch_thread) {
        InterlockedExchange(&g_fetch_in_flight, 0);
        free(r->url); free(r->host); free(r->selector); free(r->display);
        free(r);
        gopher_set_status("Gopher: failed to spawn fetch thread.");
        gopher_update_nav_buttons();
    }
}

/* Compose a GopherFetchResult and start the worker. push_history=1 if
 * the current url should be pushed onto the back stack on success. */
static void gopher_start_fetch(const char *host, int port, char type,
                               const char *selector, const char *display,
                               GopherFetchKind kind, int push_history,
                               const char *full_url)
{
    GopherFetchResult *r = (GopherFetchResult *)calloc(1, sizeof(GopherFetchResult));
    if (!r) return;
    r->url          = gopher_dup(full_url);
    r->host         = gopher_dup(host);
    r->port         = port;
    r->type         = type;
    r->selector     = gopher_dup(selector ? selector : "");
    r->display      = display ? gopher_dup(display) : NULL;
    r->kind         = kind;
    r->push_history = push_history;
    gopher_kick_fetch(r);
}

/* Navigate by URL string: parse + dispatch the right fetch kind by
 * looking at the type char. push_history controls whether to record
 * the previous URL on the back stack on success. */
static void gopher_navigate(const char *raw_url, int push_history,
                            const char *display_for_status)
{
    char *url, *host = NULL, *selector = NULL;
    int   port = 70;
    char  type = '1';
    GopherFetchKind kind;

    (void)display_for_status;
    url = gopher_normalize_url(raw_url);
    if (!url) return;
    if (gopher_parse_url(url, &host, &port, &type, &selector) != 0) {
        gopher_set_status("Gopher: malformed URL.");
        free(url);
        return;
    }

    switch (type) {
    case '0': kind = GOPHER_KIND_TEXT;   break;
    case '1': kind = GOPHER_KIND_MENU;   break;
    case 'g':
    case 'I':
    case '9':
    case 's': kind = GOPHER_KIND_BINARY; break;
    default:
        /* Unknown / out-of-scope types: graceful degrade. */
        gopher_set_status("Gopher: item type not supported in v0.1.0-mvp.");
        free(url); free(host); free(selector);
        return;
    }

    gopher_start_fetch(host, port, type, selector, NULL,
                       kind, push_history, url);
    free(url); free(host); free(selector);
}

/* ---------- DONE handler on the UI thread ---------- */

static void gopher_on_fetch_done(int ok, GopherFetchResult *r)
{
    g_fetch_thread = NULL;
    InterlockedExchange(&g_fetch_in_flight, 0);

    if (!ok || r->err_code != 0) {
        char status[320];
        _snprintf(status, sizeof(status), "Gopher: %s", r->err_msg);
        gopher_set_status(status);
        if (r->bytes) free(r->bytes);
        free(r->url); free(r->host); free(r->selector); free(r->display);
        free(r);
        gopher_update_nav_buttons();
        return;
    }

    /* Dispatch by kind. */
    switch (r->kind) {
    case GOPHER_KIND_MENU: {
        GopherItem *items;
        int n = gophermap_parse(r->bytes, r->size, &items);
        if (r->push_history && g_current_url)
            history_push_back(gopher_dup(g_current_url));
        if (r->push_history)
            history_clear_fwd();
        if (g_current_url) free(g_current_url);
        g_current_url = gopher_dup(r->url);
        gopher_render_set_menu(items, n);
        gopher_update_url_bar(g_current_url);
        gopher_set_status("Done");
        break;
    }
    case GOPHER_KIND_TEXT: {
        /* Steal the bytes into the text view. */
        if (r->push_history && g_current_url)
            history_push_back(gopher_dup(g_current_url));
        if (r->push_history)
            history_clear_fwd();
        if (g_current_url) free(g_current_url);
        g_current_url = gopher_dup(r->url);
        gopher_render_set_text(r->bytes, r->size);
        r->bytes = NULL;   /* owned by the view now */
        gopher_update_url_bar(g_current_url);
        gopher_set_status("Done");
        break;
    }
    case GOPHER_KIND_BINARY: {
        /* Type-specific dispatch. */
        switch (r->type) {
        case 'g':
        case 'I': {
            const char *title = r->display ? r->display
                              : (r->selector ? r->selector : "image");
            gopher_open_image_viewer(title, r->bytes, r->size);
            gopher_set_status("Done");
            break;
        }
        case '9': {
            int played = 0;
            if (ends_with_ci(r->selector, ".mp3") ||
                ends_with_ci(r->selector, ".au")) {
                /* Pass the selector so the player shows "Filename:". */
                if (audio_service_play_bytes_named(GetParent(g_hContent),
                                             r->bytes, r->size,
                                             r->selector) == 0) {
                    played = 1;
                    gopher_set_status("Done (playing audio)");
                }
            }
            if (!played) {
                const char *suggest = r->selector ? r->selector : "download.bin";
                /* Trim to the basename for the suggested filename. */
                const char *slash = suggest;
                const char *p;
                for (p = suggest; *p; p++) if (*p == '/') slash = p + 1;
                if (gopher_save_binary_dialog(GetParent(g_hContent),
                                              slash, r->bytes, r->size) == 0)
                    gopher_set_status("Saved");
                else
                    gopher_set_status("Save cancelled");
            }
            break;
        }
        case 's': {
            /* Pass the selector so the player shows "Filename:". */
            if (audio_service_play_bytes_named(GetParent(g_hContent),
                                         r->bytes, r->size, r->selector) == 0)
                gopher_set_status("Done (playing audio)");
            else
                gopher_set_status("Audio service busy (Stop first).");
            break;
        }
        default:
            gopher_set_status("Done (binary; no handler).");
            break;
        }
        break;
    }
    }

    if (r->bytes) free(r->bytes);
    free(r->url); free(r->host); free(r->selector); free(r->display);
    free(r);
    gopher_update_nav_buttons();
}

/* ---------- click dispatch ---------- */

static void gopher_dispatch_item(int idx)
{
    GopherItem *it;
    char       *url;

    if (idx < 0 || idx >= g_items_n) return;
    it = &g_items[idx];

    if (!gopher_type_is_clickable(it->type)) return;
    if (it->type == 'h') {
        /* TODO: cross-module dispatch hook for 'h' type (URL: prefix
         * to web_module). Out of scope for v0.1.0-mvp. */
        gopher_set_status("Gopher: 'h' URL items are not yet supported.");
        return;
    }
    if (it->type == '7') {
        /* RFC 1436 §3.4: type 7 sends the selector + TAB + query on
         * the wire. Prompt the user; on confirm, build the TAB-joined
         * selector and call gopher_start_fetch directly. The canonical
         * URL we hand to the fetch (which the DONE handler writes to
         * the URL bar and history) encodes the TAB as %09 for display;
         * the wire byte is the raw 0x09 inside r->selector. */
        char *query = NULL;
        if (!gopher_search_dialog_show(
                GetAncestor(g_hContent, GA_ROOT),
                it->display, it->selector, &query) ||
            !query || !query[0]) {
            if (query) free(query);
            return;
        }
        {
            const char *base_sel = it->selector ? it->selector : "";
            size_t      bn       = strlen(base_sel);
            size_t      qn       = strlen(query);
            char       *joined;
            char       *disp_sel;
            char       *full_url;
            const char *host = (it->host && it->host[0]) ? it->host : "";
            int         port = it->port > 0 ? it->port : 70;
            size_t      i;

            joined   = (char *)malloc(bn + 1 + qn + 1);
            disp_sel = (char *)malloc(bn + 3 + qn + 1);
            if (!joined || !disp_sel) {
                free(joined); free(disp_sel); free(query);
                return;
            }
            memcpy(joined, base_sel, bn);
            joined[bn] = '\t';
            memcpy(joined + bn + 1, query, qn + 1);

            memcpy(disp_sel, base_sel, bn);
            disp_sel[bn]     = '%';
            disp_sel[bn + 1] = '0';
            disp_sel[bn + 2] = '9';
            for (i = 0; i < qn; i++) disp_sel[bn + 3 + i] = query[i];
            disp_sel[bn + 3 + qn] = '\0';

            full_url = gopher_build_url(host, port, '7', disp_sel);
            if (full_url) {
                gopher_start_fetch(host, port, '7',
                                   joined, it->display,
                                   GOPHER_KIND_MENU,
                                   /*push_history=*/1, full_url);
                free(full_url);
            }
            free(joined);
            free(disp_sel);
            free(query);
        }
        return;
    }

    url = gopher_build_url(it->host && it->host[0] ? it->host : "",
                           it->port > 0 ? it->port : 70,
                           it->type,
                           it->selector ? it->selector : "");
    if (!url) return;
    gopher_navigate(url, /*push_history=*/1, it->display);
    free(url);
}

/* ---------- image viewer ---------- */

static LRESULT CALLBACK GopherImageProc(HWND hwnd, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        GopherImageWin *v = (GopherImageWin *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_hBrushBlack);
        if (v && v->hbm) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP old = (HBITMAP)SelectObject(mem, v->hbm);
            BitBlt(hdc, 0, 0, v->bw, v->bh, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        GopherImageWin *v = (GopherImageWin *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        if (v) {
            /* Unlink. */
            GopherImageWin **pp = &g_image_chain;
            while (*pp) {
                if (*pp == v) { *pp = v->next; break; }
                pp = &(*pp)->next;
            }
            if (v->hbm) DeleteObject(v->hbm);
            free(v);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    }
    return DefWindowProcA(hwnd, m, w, l);
}

static void gopher_register_image_class_once(HINSTANCE hInst)
{
    WNDCLASSA wc;
    if (g_image_class_reg) return;
    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = GopherImageProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_hBrushBlack;
    wc.lpszClassName = GOPHER_IMAGE_CLASS;
    RegisterClassA(&wc);
    g_image_class_reg = TRUE;
}

static void gopher_ensure_gdiplus(void)
{
    MgtGdiplusStartupInput in;
    if (g_gdiplus_inited) return;
    in.GdiplusVersion           = 1;
    in.DebugEventCallback       = NULL;
    in.SuppressBackgroundThread = FALSE;
    in.SuppressExternalCodecs   = FALSE;
    if (GdiplusStartup(&g_gdiplus_token, &in, NULL) == 0)
        g_gdiplus_inited = TRUE;
}

static void gopher_ensure_ole(void)
{
    if (g_ole_inited) return;
    OleInitialize(NULL);
    g_ole_inited = TRUE;
}

static void gopher_open_image_viewer(const char *title,
                                     const char *bytes, size_t n)
{
    HINSTANCE       hInst;
    HGLOBAL         hg;
    LPVOID          p;
    IStream        *stream = NULL;
    HRESULT         hr;
    void           *bitmap = NULL;
    HBITMAP         hbm    = NULL;
    UINT            bw = 0, bh = 0;
    GopherImageWin *v;
    HWND            hwnd;
    int             win_w, win_h;
    int             sx, sy, x, y;

    if (!bytes || n == 0) return;
    hInst = (HINSTANCE)GetModuleHandleA(NULL);
    gopher_ensure_ole();
    gopher_ensure_gdiplus();
    gopher_register_image_class_once(hInst);
    if (!g_gdiplus_inited) {
        gopher_set_status("Gopher: GDI+ unavailable; image not decoded.");
        return;
    }

    hg = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!hg) return;
    p = GlobalLock(hg);
    if (!p) { GlobalFree(hg); return; }
    memcpy(p, bytes, n);
    GlobalUnlock(hg);
    hr = CreateStreamOnHGlobal(hg, TRUE, &stream);
    if (FAILED(hr)) { GlobalFree(hg); return; }

    if (GdipCreateBitmapFromStream(stream, &bitmap) != 0 || !bitmap) {
        stream->lpVtbl->Release(stream);
        gopher_set_status("Gopher: image decode failed.");
        return;
    }
    GdipGetImageWidth (bitmap, &bw);
    GdipGetImageHeight(bitmap, &bh);
    if (bw == 0 || bh == 0 ||
        GdipCreateHBITMAPFromBitmap(bitmap, &hbm, 0) != 0) {
        GdipDisposeImage(bitmap);
        stream->lpVtbl->Release(stream);
        gopher_set_status("Gopher: image conversion failed.");
        return;
    }
    GdipDisposeImage(bitmap);
    stream->lpVtbl->Release(stream);

    v = (GopherImageWin *)calloc(1, sizeof(*v));
    if (!v) { DeleteObject(hbm); return; }
    v->hbm = hbm;
    v->bw  = (int)bw;
    v->bh  = (int)bh;

    win_w = (int)bw + 16;
    win_h = (int)bh + 40;
    if (win_w < 200) win_w = 200;
    if (win_h < 120) win_h = 120;
    /* Center on the Suite's top-level window via the shared helper so
     * the viewer opens on the same monitor as the Suite, not the
     * primary monitor. (void)sx/sy: kept for the fallback shape; the
     * helper handles the primary-screen fallback internally. */
    (void)sx; (void)sy;
    suite_center_popup(g_hContent, win_w, win_h, &x, &y);

    hwnd = CreateWindowA(GOPHER_IMAGE_CLASS,
                         title ? title : "Gopher image",
                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                         x, y, win_w, win_h,
                         NULL, NULL, hInst, NULL);
    if (!hwnd) {
        DeleteObject(hbm);
        free(v);
        return;
    }
    v->hwnd = hwnd;
    v->next = g_image_chain;
    g_image_chain = v;
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)v);
    UpdateWindow(hwnd);
}

/* ---------- save binary dialog ---------- */

static int gopher_save_binary_dialog(HWND owner, const char *suggested,
                                     const char *bytes, size_t n)
{
    OPENFILENAMEA ofn;
    char          path[MAX_PATH];
    HANDLE        h;
    DWORD         written;
    BOOL          ok;

    memset(path, 0, sizeof(path));
    if (suggested && suggested[0]) {
        size_t len = strlen(suggested);
        if (len >= sizeof(path)) len = sizeof(path) - 1;
        memcpy(path, suggested, len);
        path[len] = '\0';
    }
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = "All files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = sizeof(path);
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameA(&ofn)) return 1;

    h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 2;
    ok = WriteFile(h, bytes, (DWORD)n, &written, NULL);
    CloseHandle(h);
    if (!ok || written != (DWORD)n) return 3;
    return 0;
}

/* ---------- render area window proc ---------- */

static LRESULT CALLBACK GopherRenderProc(HWND hwnd, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)w;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_hBrushGopherBg ? g_hBrushGopherBg : g_hBrushBlack);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT        rc;
        HDC         hdc = BeginPaint(hwnd, &ps);
        HDC         mem_dc  = NULL;
        HBITMAP     mem_bmp = NULL;
        HBITMAP     old_bmp = NULL;
        int         client_w, client_h;
        int         has_content = gopher_has_document();
        GetClientRect(hwnd, &rc);
        client_w = rc.right  - rc.left;
        client_h = rc.bottom - rc.top;
        if (client_w <= 0 || client_h <= 0) {
            EndPaint(hwnd, &ps);
            return 0;
        }
        /* Polish fix (2026-05-22): mem-DC double-buffer pattern (matches
         * F6 polish 1). Two coupled rules in play here:
         *   1. Any custom-painted window using TRANSPARENT-mode text
         *      emit MUST own its full-rect background fill at WM_PAINT
         *      entry. Our paint helpers (gopher_text_emit_utf8 +
         *      gopher_paint_line_with_selection from Dispatch B) all
         *      use TRANSPARENT mode, and the selection-clear path uses
         *      InvalidateRect(hwnd, NULL, FALSE) which suppresses
         *      WM_ERASEBKGND, so without a self-fill the prior
         *      selection's light highlight pixels survive into the
         *      next frame.
         *   2. That full-rect fill MUST land on a memory DC, not the
         *      screen DC, otherwise rapid drag-invalidate cycles let
         *      the user see the bg-fill frame on screen before the
         *      text gets composited on top of it. The screen receives
         *      one atomic BitBlt of the composited frame. */
        mem_dc  = CreateCompatibleDC(hdc);
        mem_bmp = CreateCompatibleBitmap(hdc, client_w, client_h);
        if (!mem_dc || !mem_bmp) {
            if (mem_bmp) DeleteObject(mem_bmp);
            if (mem_dc)  DeleteDC(mem_dc);
            /* Fall back to direct paint with at least the bg fill so
             * the visible frame stays consistent. */
            if (g_hBrushGopherBg) FillRect(hdc, &ps.rcPaint, g_hBrushGopherBg);
            EndPaint(hwnd, &ps);
            return 0;
        }
        old_bmp = (HBITMAP)SelectObject(mem_dc, mem_bmp);

        if (g_hBrushGopherBg) FillRect(mem_dc, &rc, g_hBrushGopherBg);

        if (!has_content) {
            /* Empty state: muted gray hint in Gopher terms (selector +
             * Home), distinct from F2's web-browser wording. */
            const char *msg = "(no selector loaded - Press Home, or enter a Selector and press Go)";
            HFONT  old_font = (HFONT)SelectObject(mem_dc, g_hFontMono ? g_hFontMono : g_hFontUI);
            SIZE   sz;
            int    x, y;
            SetBkMode(mem_dc, TRANSPARENT);
            SetTextColor(mem_dc, GOPHER_COL_HINT);
            GetTextExtentPoint32A(mem_dc, msg, (int)strlen(msg), &sz);
            x = ((rc.right - rc.left) - sz.cx) / 2;
            y = ((rc.bottom - rc.top) - sz.cy) / 2;
            if (x < 8) x = 8;
            if (y < 8) y = 8;
            TextOutA(mem_dc, x, y, msg, (int)strlen(msg));
            SelectObject(mem_dc, old_font);
        } else if (g_view_mode == GVIEW_TEXT) {
            gopher_paint_text(mem_dc, &rc);
        } else {
            gopher_paint_menu(mem_dc, &rc);
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
        gopher_update_scroll(hwnd);
        return 0;
    }

    /* Polish (2026-05-22): the F6 render uses the same WM_GETDLGCODE
     * trick (polish 4). F3 had no symptoms yet because its only
     * keyboard consumers were arrow keys, but adding Ctrl-A/C/V brings
     * the same exposure -- declare our keyboard appetite explicitly so
     * IsDialogMessage in the main loop cannot run mnemonic-search on
     * our letter keystrokes. */
    case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTALLKEYS;

    case WM_LBUTTONDOWN: {
        /* Defer navigation to WM_LBUTTONUP. WM_MOUSEMOVE may promote
         * this to a drag-select, in which case we do NOT navigate. */
        g_f3_lbutton_down = TRUE;
        g_f3_drag_active  = FALSE;
        g_f3_down_x_px    = GET_X_LPARAM(l);
        g_f3_down_y_px    = GET_Y_LPARAM(l);
        SetCapture(hwnd);
        SetFocus(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(l);
        int y = GET_Y_LPARAM(l);
        if (g_f3_lbutton_down) {
            /* Drag path -- selection. */
            if (!g_f3_drag_active) {
                int dx = x - g_f3_down_x_px; if (dx < 0) dx = -dx;
                int dy = y - g_f3_down_y_px; if (dy < 0) dy = -dy;
                if (dx > F3_DRAG_THRESHOLD_PX || dy > F3_DRAG_THRESHOLD_PX) {
                    int a_line, a_col;
                    pixel_to_line_col(hwnd, g_f3_down_x_px, g_f3_down_y_px,
                                      &a_line, &a_col);
                    g_sel_anchor_line = a_line;
                    g_sel_anchor_col  = a_col;
                    gopher_select_set(a_line, a_col, a_line, a_col);
                    g_f3_drag_active = TRUE;
                }
            }
            if (g_f3_drag_active) {
                RECT rc;
                int l_line, l_col;
                GetClientRect(hwnd, &rc);
                if (y < 0)              arm_f3_sel_scroll_timer(hwnd, -1);
                else if (y >= rc.bottom) arm_f3_sel_scroll_timer(hwnd, +1);
                else                     kill_f3_sel_scroll_timer(hwnd);
                pixel_to_line_col(hwnd, x, y, &l_line, &l_col);
                gopher_select_set(g_sel_anchor_line, g_sel_anchor_col,
                                  l_line, l_col);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        /* Hover path -- cursor only (hand over clickable items). */
        {
            int idx = gopher_item_at_point(x, y);
            if (idx >= 0 && gopher_type_is_clickable(g_items[idx].type)) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
            } else {
                SetCursor(LoadCursor(NULL, IDC_ARROW));
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!g_f3_lbutton_down) return 0;
        ReleaseCapture();
        kill_f3_sel_scroll_timer(hwnd);
        if (g_f3_drag_active) {
            /* Selection finalized; suppress navigation. */
            g_f3_drag_active = FALSE;
        } else {
            /* Click without drag: clear any prior selection, then run
             * the normal navigation path with the down-click coords. */
            gopher_select_clear();
            InvalidateRect(hwnd, NULL, FALSE);
            f3_handle_click_navigate(g_f3_down_x_px, g_f3_down_y_px);
        }
        g_f3_lbutton_down = FALSE;
        return 0;
    }
    case WM_RBUTTONDOWN: {
        HMENU hm = CreatePopupMenu();
        UINT  fc = g_sel_active ? MF_ENABLED : MF_GRAYED;
        POINT pt;
        if (!hm) return 0;
        AppendMenuA(hm, MF_STRING | fc,    ID_CTX_COPY,        "Copy\tCtrl+C");
        AppendMenuA(hm, MF_STRING | MF_GRAYED, ID_CTX_PASTE,   "Paste");
        AppendMenuA(hm, MF_SEPARATOR,       0,                  NULL);
        AppendMenuA(hm, MF_STRING,          ID_CTX_SELECT_ALL, "Select All\tCtrl+A");
        pt.x = GET_X_LPARAM(l);
        pt.y = GET_Y_LPARAM(l);
        ClientToScreen(hwnd, &pt);
        SetFocus(hwnd);
        TrackPopupMenu(hm, TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                       pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(hm);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_CTX_COPY) {
            gopher_module_copy_selection();
            return 0;
        }
        if (id == ID_CTX_PASTE) {
            /* F3 render is read-only; ignore. */
            return 0;
        }
        if (id == ID_CTX_SELECT_ALL) {
            gopher_select_all();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    }
    case WM_TIMER: {
        if (w == ID_TIMER_F3_SEL_SCROLL) {
            POINT pt;
            int   l_line, l_col;
            if (!g_f3_drag_active) {
                kill_f3_sel_scroll_timer(hwnd);
                return 0;
            }
            if (g_f3_scroll_dir > 0)      f3_scroll_by_lines(hwnd, +1);
            else if (g_f3_scroll_dir < 0) f3_scroll_by_lines(hwnd, -1);
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            pixel_to_line_col(hwnd, pt.x, pt.y, &l_line, &l_col);
            gopher_select_set(g_sel_anchor_line, g_sel_anchor_col,
                              l_line, l_col);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        int lines = delta / WHEEL_DELTA * 3;
        int total_h = gopher_total_height();
        RECT rc; int page;
        GetClientRect(hwnd, &rc);
        page = rc.bottom - rc.top;
        g_scroll_y -= lines * g_row_h;
        if (g_scroll_y < 0) g_scroll_y = 0;
        if (g_scroll_y > total_h - page) g_scroll_y = total_h - page;
        if (g_scroll_y < 0) g_scroll_y = 0;
        SetScrollPos(hwnd, SB_VERT, g_scroll_y, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_VSCROLL: {
        int  code = LOWORD(w);
        int  pos  = HIWORD(w);
        RECT rc;  int page, total_h, new_pos;
        SCROLLINFO si;
        GetClientRect(hwnd, &rc);
        page    = rc.bottom - rc.top;
        total_h = gopher_total_height();
        new_pos = g_scroll_y;
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask  = SIF_TRACKPOS;
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (code) {
        case SB_LINEUP:        new_pos -= g_row_h; break;
        case SB_LINEDOWN:      new_pos += g_row_h; break;
        case SB_PAGEUP:        new_pos -= page;    break;
        case SB_PAGEDOWN:      new_pos += page;    break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: new_pos  = si.nTrackPos; (void)pos; break;
        case SB_TOP:           new_pos  = 0;       break;
        case SB_BOTTOM:        new_pos  = total_h; break;
        }
        if (new_pos < 0) new_pos = 0;
        if (new_pos > total_h - page) new_pos = total_h - page;
        if (new_pos < 0) new_pos = 0;
        g_scroll_y = new_pos;
        SetScrollPos(hwnd, SB_VERT, g_scroll_y, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_KEYDOWN: {
        RECT rc; int page, total_h, new_pos = g_scroll_y;
        /* Polish (2026-05-22): Ctrl-A / Ctrl-C / Ctrl-V handled first
         * so a Ctrl chord cannot fall through to the arrow scroll. */
        BOOL ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl) {
            if (w == 'A') {
                gopher_select_all();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (w == 'C') {
                if (g_sel_active) {
                    gopher_module_copy_selection();
                    gopher_select_clear();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                /* No selection: no-op. F3 has no remote session to
                 * interrupt with an SIGINT-equivalent. */
                return 0;
            }
            if (w == 'V') {
                /* F3 render is read-only; the URL bar handles its own
                 * Ctrl-V natively. */
                return 0;
            }
        }
        GetClientRect(hwnd, &rc);
        page    = rc.bottom - rc.top;
        total_h = gopher_total_height();
        switch (w) {
        case VK_UP:    new_pos -= g_row_h; break;
        case VK_DOWN:  new_pos += g_row_h; break;
        case VK_PRIOR: new_pos -= page;    break;
        case VK_NEXT:  new_pos += page;    break;
        case VK_HOME:  new_pos  = 0;       break;
        case VK_END:   new_pos  = total_h; break;
        default: return 0;
        }
        if (new_pos < 0) new_pos = 0;
        if (new_pos > total_h - page) new_pos = total_h - page;
        if (new_pos < 0) new_pos = 0;
        g_scroll_y = new_pos;
        SetScrollPos(hwnd, SB_VERT, g_scroll_y, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_SIZE:
        gopher_update_scroll(hwnd);
        return 0;
    case WM_GOPHER_FETCH_DONE: {
        GopherFetchResult *r = (GopherFetchResult *)l;
        if (r) gopher_on_fetch_done((int)w, r);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, m, w, l);
}

static void gopher_register_render_class_once(HINSTANCE hInst)
{
    WNDCLASSA wc;
    if (g_render_class_reg) return;
    if (!g_hBrushGopherBg)
        g_hBrushGopherBg = CreateSolidBrush(GOPHER_COL_BG);
    if (!g_f3_sel_brush)
        g_f3_sel_brush = CreateSolidBrush(GOPHER_COL_FG);  /* light = selection bg */
    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = GopherRenderProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_hBrushGopherBg;
    wc.lpszClassName = GOPHER_RENDER_CLASS;
    RegisterClassA(&wc);
    g_render_class_reg = TRUE;
}

/* ---------- URL edit subclass (Enter -> Go) ---------- */

static WNDPROC g_orig_url_proc = NULL;

static LRESULT CALLBACK GopherUrlEditSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_GETDLGCODE) {
        MSG *pmsg = (MSG *)l;
        LRESULT base = CallWindowProcA(g_orig_url_proc, h, m, w, l);
        if (pmsg && pmsg->message == WM_KEYDOWN && pmsg->wParam == VK_RETURN)
            return base | DLGC_WANTMESSAGE;
        return base;
    }
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        char buf[1024];
        GetWindowTextA(h, buf, sizeof(buf));
        gopher_navigate(buf, /*push_history=*/1, NULL);
        return 0;
    }
    if (m == WM_CHAR && w == '\r') return 0;
    return CallWindowProcA(g_orig_url_proc, h, m, w, l);
}

/* ---------- public entry points ---------- */

void gopher_module_init(void)
{
    /* Idempotent: real allocation happens on first activate. The font
     * handles are created lazily there once the GUI fonts are ready. */
}

void gopher_module_shutdown(void)
{
    /* Free history, current view, and any cached strings. The image
     * viewer chain is walked and torn down too so HBITMAPs do not
     * leak across re-launch in interactive runs. */
    int i;
    while (g_image_chain) {
        HWND h = g_image_chain->hwnd;
        if (h && IsWindow(h)) DestroyWindow(h);
        else { /* destroy handler already cleaned up */ }
        /* DestroyWindow re-enters our proc which unlinks the head. If
         * the window had already been destroyed, break to avoid a
         * loop on a stale chain. */
        if (g_image_chain && g_image_chain->hwnd == h) {
            GopherImageWin *next = g_image_chain->next;
            if (g_image_chain->hbm) DeleteObject(g_image_chain->hbm);
            free(g_image_chain);
            g_image_chain = next;
        }
    }
    for (i = 0; i < g_back_n; i++) { free(g_back_stack[i]); g_back_stack[i] = NULL; }
    g_back_n = 0;
    for (i = 0; i < g_fwd_n; i++)  { free(g_fwd_stack[i]);  g_fwd_stack[i]  = NULL; }
    g_fwd_n = 0;
    gopher_render_clear();
    if (g_current_url) { free(g_current_url); g_current_url = NULL; }
    if (g_hFontMono) { DeleteObject(g_hFontMono); g_hFontMono = NULL; }
    if (g_hFontIcon) { DeleteObject(g_hFontIcon); g_hFontIcon = NULL; }
    if (g_hBrushGopherBg) { DeleteObject(g_hBrushGopherBg); g_hBrushGopherBg = NULL; }
    if (g_f3_sel_brush)   { DeleteObject(g_f3_sel_brush);   g_f3_sel_brush   = NULL; }
    if (g_gdiplus_inited) {
        GdiplusShutdown(g_gdiplus_token);
        g_gdiplus_inited = FALSE;
        g_gdiplus_token  = 0;
    }
}

void gopher_module_resize(HWND content, int w, int h)
{
    const int margin   = 16;
    const int btn_h    = 30;
    const int row_h    = 26;
    const int tb_btn_w = 80;
    const int tb_gap   = 4;
    const int go_w     = 70;
    int x, y;

    (void)content;
    if (!g_controls_created) return;

    y = 14;
    MoveWindow(g_hUrlLbl,  margin,        y + 2, 80, 22, TRUE);
    MoveWindow(g_hUrlEdit, margin + 84,   y,
               w - margin - 84 - 8 - go_w - margin, row_h, TRUE);
    MoveWindow(g_hGoBtn,   w - margin - go_w, y - 2, go_w, btn_h, TRUE);

    y = 50;  x = margin;
    MoveWindow(g_hBackBtn,    x, y - 2, tb_btn_w, btn_h, TRUE); x += tb_btn_w + tb_gap;
    MoveWindow(g_hFwdBtn,     x, y - 2, tb_btn_w, btn_h, TRUE); x += tb_btn_w + tb_gap;
    MoveWindow(g_hHomeBtn,    x, y - 2, tb_btn_w, btn_h, TRUE); x += tb_btn_w + tb_gap;
    MoveWindow(g_hStopBtn,    x, y - 2, tb_btn_w, btn_h, TRUE); x += tb_btn_w + tb_gap;
    MoveWindow(g_hRefreshBtn, x, y - 2, tb_btn_w, btn_h, TRUE);

    y = 88;
    MoveWindow(g_hRender, margin, y, w - 2 * margin, h - y - margin, TRUE);
}

void gopher_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    g_hContent = content;

    if (g_controls_created) {
        ShowWindow(g_hBackBtn,    SW_SHOW);
        ShowWindow(g_hFwdBtn,     SW_SHOW);
        ShowWindow(g_hHomeBtn,    SW_SHOW);
        ShowWindow(g_hStopBtn,    SW_SHOW);
        ShowWindow(g_hRefreshBtn, SW_SHOW);
        ShowWindow(g_hUrlLbl,     SW_SHOW);
        ShowWindow(g_hUrlEdit,    SW_SHOW);
        ShowWindow(g_hGoBtn,      SW_SHOW);
        ShowWindow(g_hRender,     SW_SHOW);
        SetFocus(g_hUrlEdit);
        return;
    }

    gopher_register_render_class_once(hInst);

    if (!g_hFontMono) {
        /* Ubuntu Mono is loaded into the process by suite_fonts_init
         * via AddFontMemResourceEx, so it's findable by face name here
         * without being installed on the host. Negative height = 18 px
         * cell height (visually tuned). DEFAULT_CHARSET keeps Greek
         * fallback alive; CLEARTYPE_QUALITY makes the small body face
         * legible at this size. */
        g_hFontMono = CreateFontA(
            -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Ubuntu Mono");
        if (!g_hFontMono) {
            /* Fallback: original Consolas face, in case the embedded
             * font failed to register. F3 stays usable either way. */
            g_hFontMono = CreateFontA(
                16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Consolas");
        }
    }
    if (!g_hFontIcon) {
        g_hFontIcon = CreateFontA(18, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, 0, DEFAULT_PITCH, "Segoe UI Emoji");
        if (!g_hFontIcon)
            g_hFontIcon = CreateFontA(18, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                DEFAULT_CHARSET, 0, 0, 0, DEFAULT_PITCH, "Segoe UI Symbol");
    }

    g_hBackBtn = CreateWindowA("BUTTON", "< Back",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 80, 30, content, (HMENU)(INT_PTR)IDC_GOPH_BACK_BTN, hInst, NULL);
    g_hFwdBtn = CreateWindowA("BUTTON", "Forward >",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 80, 30, content, (HMENU)(INT_PTR)IDC_GOPH_FWD_BTN, hInst, NULL);
    g_hHomeBtn = CreateWindowA("BUTTON", "Home",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 80, 30, content, (HMENU)(INT_PTR)IDC_GOPH_HOME_BTN, hInst, NULL);
    g_hStopBtn = CreateWindowA("BUTTON", "Stop",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 80, 30, content, (HMENU)(INT_PTR)IDC_GOPH_STOP_BTN, hInst, NULL);
    g_hRefreshBtn = CreateWindowA("BUTTON", "Refresh",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 80, 30, content, (HMENU)(INT_PTR)IDC_GOPH_REFRESH_BTN, hInst, NULL);

    g_hUrlLbl = CreateWindowA("STATIC", "Selector:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 80, 22, content, (HMENU)(INT_PTR)IDC_GOPH_URL_LBL, hInst, NULL);
    g_hUrlEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", GOPHER_HOME_URL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 400, 26, content, (HMENU)(INT_PTR)IDC_GOPH_URL_EDIT,
        hInst, NULL);
    g_hGoBtn = CreateWindowA("BUTTON", "Go",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 70, 30, content, (HMENU)(INT_PTR)IDC_GOPH_GO_BTN, hInst, NULL);

    g_hRender = CreateWindowExA(WS_EX_CLIENTEDGE, GOPHER_RENDER_CLASS, "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL,
        0, 0, 200, 200, content, (HMENU)(INT_PTR)IDC_GOPH_RENDER, hInst, NULL);

    if (g_hFontUI) {
        SendMessageA(g_hBackBtn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hFwdBtn,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hHomeBtn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hStopBtn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hRefreshBtn, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hUrlLbl,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hUrlEdit,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_hGoBtn,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }

    g_orig_url_proc = (WNDPROC)SetWindowLongPtrA(
        g_hUrlEdit, GWLP_WNDPROC, (LONG_PTR)GopherUrlEditSub);

    EnableWindow(g_hBackBtn,    FALSE);
    EnableWindow(g_hFwdBtn,     FALSE);
    EnableWindow(g_hStopBtn,    FALSE);
    EnableWindow(g_hRefreshBtn, FALSE);  /* nothing loaded yet */

    GetClientRect(content, &rc);
    gopher_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);

    g_controls_created = TRUE;

    /* No auto-fetch on activation: show the empty-state hint until
     * the user navigates. First-activation only, prefill the URL bar
     * with GOPHER_HOME_URL so a single Enter press lands the user on
     * the home menu (the most-likely first action). On every later
     * activation, the URL bar shows whatever the user last loaded
     * because g_current_url has been written by the fetch path. */
    if (!g_current_url) {
        SetWindowTextA(g_hUrlEdit, GOPHER_HOME_URL);
    }
    SetFocus(g_hUrlEdit);
}

void gopher_module_deactivate(HWND content)
{
    (void)content;
    if (!g_controls_created) return;
    ShowWindow(g_hBackBtn,    SW_HIDE);
    ShowWindow(g_hFwdBtn,     SW_HIDE);
    ShowWindow(g_hHomeBtn,    SW_HIDE);
    ShowWindow(g_hStopBtn,    SW_HIDE);
    ShowWindow(g_hRefreshBtn, SW_HIDE);
    ShowWindow(g_hUrlLbl,     SW_HIDE);
    ShowWindow(g_hUrlEdit,    SW_HIDE);
    ShowWindow(g_hGoBtn,      SW_HIDE);
    ShowWindow(g_hRender,     SW_HIDE);
}

BOOL gopher_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id    = LOWORD(wParam);
    int notif = HIWORD(wParam);
    (void)content; (void)lParam;

    /* The fetch DONE message is delivered to the content panel by the
     * worker thread; pick it up here from the WM_COMMAND siblings. */
    switch (id) {
    case IDC_GOPH_GO_BTN:
        if (notif == BN_CLICKED) {
            char buf[1024];
            GetWindowTextA(g_hUrlEdit, buf, sizeof(buf));
            gopher_navigate(buf, /*push_history=*/1, NULL);
            return TRUE;
        }
        break;
    case IDC_GOPH_HOME_BTN:
        if (notif == BN_CLICKED) {
            gopher_navigate(GOPHER_HOME_URL, /*push_history=*/1, NULL);
            return TRUE;
        }
        break;
    case IDC_GOPH_REFRESH_BTN:
        if (notif == BN_CLICKED) {
            if (g_current_url)
                gopher_navigate(g_current_url, /*push_history=*/0, NULL);
            return TRUE;
        }
        break;
    case IDC_GOPH_STOP_BTN:
        if (notif == BN_CLICKED) {
            /* Worker has no cancel hook in v0.1.0-mvp; the recv loop
             * has a 15s timeout. Surface intent through the status
             * line and let the timeout fire. */
            gopher_set_status("Gopher: stop requested (waiting on recv timeout)...");
            return TRUE;
        }
        break;
    case IDC_GOPH_BACK_BTN:
        if (notif == BN_CLICKED) {
            if (g_back_n > 0) {
                char *prev = g_back_stack[--g_back_n];
                if (g_current_url) {
                    if (g_fwd_n < GOPHER_HISTORY_MAX)
                        g_fwd_stack[g_fwd_n++] = g_current_url;
                    else
                        free(g_current_url);
                    g_current_url = NULL;
                }
                gopher_navigate(prev, /*push_history=*/0, NULL);
                free(prev);
                gopher_update_nav_buttons();
            }
            return TRUE;
        }
        break;
    case IDC_GOPH_FWD_BTN:
        if (notif == BN_CLICKED) {
            if (g_fwd_n > 0) {
                char *next = g_fwd_stack[--g_fwd_n];
                if (g_current_url) {
                    if (g_back_n < GOPHER_HISTORY_MAX)
                        g_back_stack[g_back_n++] = g_current_url;
                    else
                        free(g_current_url);
                    g_current_url = NULL;
                }
                gopher_navigate(next, /*push_history=*/0, NULL);
                free(next);
                gopher_update_nav_buttons();
            }
            return TRUE;
        }
        break;
    }
    return FALSE;
}

BOOL gopher_module_has_unsaved(void)
{
    return FALSE;
}
