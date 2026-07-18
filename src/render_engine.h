/*
 * render_engine.h - Shared internal rendering engine (Phase 6R).
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Phase 6R promotes the renderer that previously lived as a single
 * Retro-Web-bound singleton in src/web_module.c into a reusable
 * engine through one structural change: every former per-view
 * singleton global moves into a WebRenderView struct that engine
 * entry points accept as a parameter. The function bodies stay in
 * src/web_module.c (no physical relocation); the engine no longer
 * reads or writes file-scope singletons for per-view state.
 *
 * The Retro Web module declares exactly one WebRenderView instance
 * and continues to behave bit-identically. Phase 6b's Active
 * Desktop module will declare its own WebRenderView for the ticker
 * band and call the same engine entry points.
 *
 * Phase 6a CDF seam (intact): the CDF parser, web_url_is_cdf, the
 * is_cdf branches of web_fetch_worker and web_fetch_timer_proc,
 * and web_navigate_from_cdf live in web_module.c. This header does
 * not touch them.
 */

#ifndef RENDER_ENGINE_H
#define RENDER_ENGINE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>

/* HTextStatus (used inside WebDoc) is defined by libwww. The engine
 * unit (src/web_module.c) already includes the libwww headers; a
 * second consumer compiled with -Isrc\libwww (the Suite's
 * CFLAGS_WEB profile) will also pick HText.h up cleanly. */
#include "HText.h"

/* ------------------------------------------------------------------ */
/* Engine timers and intervals.                                        */
/* ------------------------------------------------------------------ */

/* Animation tick: 50 ms, 20 Hz. Distinct from the 0xB001 fetch timer
 * and the 0xB002 selection auto-scroll timer. */
#define WEB_ANIM_TIMER_ID    0xB003
#define WEB_ANIM_INTERVAL_MS 50

/* ------------------------------------------------------------------ */
/* Render model. Types copied verbatim from the pre-6R source so the  */
/* on-disk struct layout is unchanged. The Retro Web shell continues  */
/* to walk WebBlock and WebDoc fields directly (notably for the       */
/* WM_TIMER animation tick).                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    BLK_PARA,
    BLK_H1, BLK_H2, BLK_H3, BLK_H4, BLK_H5, BLK_H6,
    BLK_HR,
    BLK_PRE,
    BLK_LI,        /* list item; bullet_text + indent */
    BLK_DT,
    BLK_DD,        /* indented one level */
    BLK_IMG,       /* image (src/alt fields hold info) */
    BLK_TABLE      /* runs and inner blocks ignored; table_data holds cells */
} BlockType;

/* Inline style bits. */
#define STY_BOLD        (1u << 0)
#define STY_ITALIC      (1u << 1)
#define STY_UNDERLINE   (1u << 2)
#define STY_LINK        (1u << 3)
#define STY_MONO        (1u << 4)
#define STY_SUB         (1u << 5)
#define STY_SUP         (1u << 6)
#define STY_SMALL       (1u << 7)

#define WEB_NOCOLOR     0xFFFFFFFFu

typedef struct WebRun {
    char    *text;
    size_t   len;
    unsigned style;
    COLORREF color;        /* WEB_NOCOLOR = use block default */
    int      size_delta;   /* font size delta from block default (e.g. -2..+4) */
    char    *href;
    /* Inline image: non-NULL when this run is an <img> that followed text
     * in an inline context (e.g. "101.6 kPa <arrow>"). It points to a
     * heap BLK_IMG container so the existing image fetch/decode/blit code
     * can be reused unchanged; the run flows in the text line instead of
     * forcing a block break. NULL for ordinary text runs. */
    struct WebBlock *img_blk;
} WebRun;

/* Forward decl for table cells. */
struct WebTable;

typedef struct WebBlock {
    BlockType         type;
    int               indent;       /* indentation units (each = ~24 px) */
    int               align;        /* 0=left, 1=center, 2=right */
    char             *bullet_text;  /* for BLK_LI prefix; NULL otherwise */

    /* Text content (most block types). */
    WebRun           *runs;
    size_t            runs_n;
    size_t            runs_cap;

    /* BLK_IMG payload. */
    char             *img_src;
    char             *img_alt;
    char             *img_href;     /* enclosing <a href=...>, NULL if none */
    int               img_w_hint;   /* from <img width=...>, 0 if unspecified */
    int               img_h_hint;   /* from <img height=...> */
    HBITMAP           img_hbm;      /* current frame; aliases img_frames[img_current_frame] when present */
    int               img_w_actual;
    int               img_h_actual;
    /* Pass 10B / IMG-05: animated-GIF playback. img_frames[] is the
     * decode-time array of HBITMAPs (length 1 for static images);
     * img_frame_delays_ms[] is the parallel per-frame delay in
     * milliseconds; img_hbm is kept and updated by the animation
     * tick to alias the active frame so web_render_image's blit
     * path is unchanged. Ownership of every HBITMAP lives in
     * img_frames[]; img_hbm is a non-owning pointer into it. */
    HBITMAP          *img_frames;
    unsigned         *img_frame_delays_ms;
    int               img_frame_count;
    int               img_current_frame;
    int               img_ms_until_next;   /* ms remaining for current frame */
    int               img_loop_count;      /* 0 = infinite (per Netscape ext) */
    int               img_loops_done;
    int               img_anim_done;       /* set once a finite loop has ended */
    RECT              img_last_rect;       /* last painted client rect; drives per-image InvalidateRect */

    /* BLK_TABLE payload. */
    struct WebTable  *table_data;
} WebBlock;

typedef struct WebCell {
    BlockType   ttype;     /* TD or TH (we model TH as bold TD) */
    int         is_header;
    WebBlock   *inner;     /* list of inner blocks (cell contents) */
    size_t      inner_n;
    size_t      inner_cap;
    int         colspan;
    int         align;     /* 0=left, 1=center, 2=right (TD/TH align attr) */
    /* Table-layout phase 1+ fields. bgcolor defaults to WEB_NOCOLOR
     * (no fill). width_px / width_pct hold an explicit <td width=...>:
     * exactly one is nonzero, or both zero when unspecified. valign:
     * 0=top (default), 1=middle, 2=bottom (wired in phase 2). rowspan is
     * parsed but layout treats it as 1 (out of scope by operator decision). */
    COLORREF    bgcolor;
    int         width_px;
    int         width_pct;
    int         valign;
    int         rowspan;
} WebCell;

typedef struct WebRow {
    WebCell *cells;
    size_t   cells_n;
    size_t   cells_cap;
    int      align;        /* TR-level default align for its cells */
    /* Row-level defaults. valign wired in phase 2. bgcolor is present for
     * completeness but note the vendored libwww DTD group for TR
     * (_HTML_TELE_Attributes) has no BGCOLOR, so <tr bgcolor> is dropped by
     * the parser and this normally stays WEB_NOCOLOR. */
    COLORREF bgcolor;
    int      valign;
} WebRow;

typedef struct WebTable {
    WebRow  *rows;
    size_t   rows_n;
    size_t   rows_cap;
    char    *caption;
    int      border;
    int      align;        /* table-block horizontal placement: 0/1/2 */
    /* Table-layout phase 1+ fields. bgcolor defaults to WEB_NOCOLOR.
     * width_px / width_pct hold an explicit <table width=...>. cellpadding
     * / cellspacing are -1 when unspecified (engine applies HTML defaults
     * of 1 and 2 px respectively). */
    COLORREF bgcolor;
    int      width_px;
    int      width_pct;
    int      cellpadding;
    int      cellspacing;
} WebTable;

/* ------------------------------------------------------------------ */
/* Style + parse state.                                                */
/* ------------------------------------------------------------------ */

#define WEB_STACK 16

typedef struct StyleState {
    int       bold_depth;
    int       italic_depth;
    int       underline_depth;
    int       mono_depth;
    int       link_depth;
    int       sub_depth;
    int       sup_depth;
    int       small_depth;
    char     *current_href;

    COLORREF  color_stack[WEB_STACK];
    int       color_depth;

    int       size_stack[WEB_STACK];
    int       size_depth;

    int       indent_depth;    /* blockquote + dl + list */
    int       center_depth;

    int       list_kind_stack[WEB_STACK];   /* 'U' or 'O' */
    int       list_counter_stack[WEB_STACK];
    int       list_depth;
} StyleState;

typedef enum {
    PARSE_NORMAL,
    PARSE_IN_TABLE,
    PARSE_IN_CELL
} ParseMode;

typedef struct WebDoc {
    WebBlock   *blocks;
    size_t      blocks_n;
    size_t      blocks_cap;

    BlockType   current_type;
    int         current_indent;
    int         current_align;
    char       *current_bullet;
    WebRun     *current_runs;
    size_t      current_runs_n;
    size_t      current_runs_cap;

    ParseMode   parse_mode;
    WebTable   *open_table;
    WebTable   *table_stack[WEB_STACK];
    int         table_stack_depth;
    WebCell    *open_cell;
    /* Parallel to table_stack: the cell (if any) that each open table is
     * nested inside. On a nested </table> the completed BLK_TABLE is routed
     * back into this parent cell instead of being hoisted to the document
     * top level. NULL entry means the table is at document top level. */
    WebCell    *table_parent_cell[WEB_STACK];

    int         in_style;
    int         in_script;
    char       *style_buf;
    size_t      style_buf_len;
    size_t      style_buf_cap;

    int         in_title;

    COLORREF    body_bg;
    COLORREF    body_text;
    COLORREF    body_link;
    COLORREF    body_vlink;

    UINT        source_codepage;

    StyleState  style;
    HTextStatus status;
} WebDoc;

/* ------------------------------------------------------------------ */
/* Selection token (one entry per painted text token, indexed by      */
/* document order).                                                   */
/* ------------------------------------------------------------------ */

typedef struct WebSelToken {
    int         x, y, w, h;
    const char *bytes;          /* points into a WebRun.text (UTF-8) */
    int         len;
    HFONT       font;
    int         doc_idx;
    int         block_idx;
    int         line_idx;
    unsigned char is_pre;
} WebSelToken;

/* ------------------------------------------------------------------ */
/* Link hit-test rect emitted by paint.                                */
/* ------------------------------------------------------------------ */

typedef struct WebLinkRect {
    RECT        rc;
    const char *href;
} WebLinkRect;

/* ------------------------------------------------------------------ */
/* WebRenderView: per-consumer render state. The Phase 6R structural   */
/* change. Every former per-view singleton global lives in here. The   */
/* Retro Web module owns one instance; Phase 6b's Active Desktop will  */
/* own a second. Engine entry points accept a WebRenderView *view and  */
/* operate on view->* fields instead of any file-scope singleton.      */
/*                                                                     */
/* Initial-state invariants: sel_anchor_idx and sel_extent_idx must    */
/* start at -1 (no selection). Everything else can be zero-init. The   */
/* Retro Web module uses a designated initializer; a heap-allocated    */
/* view (e.g. for the Active Desktop ticker host) must call            */
/* web_render_view_init to apply the sentinels.                        */
/* ------------------------------------------------------------------ */

typedef struct WebRenderView {
    WebDoc      *view_doc;
    int          scroll_y;
    int          content_height;

    /* Selection map regenerated on every paint. */
    WebSelToken *sel_map;
    int          sel_map_n;
    int          sel_map_cap;

    /* Paint-time selection counters (private to web_paint_to and the
     * web_emit_line dispatch the engine drives during one paint). */
    int          sel_cur_block_idx;
    int          sel_cur_line_idx;
    int          sel_cur_is_pre;

    /* Selection state: anchor + extent as (doc_idx, byte_off). idx -1
     * means no selection. The pair is unordered; the copy/highlight
     * passes normalize. */
    int          sel_anchor_idx;
    int          sel_anchor_off;
    int          sel_extent_idx;
    int          sel_extent_off;
    int          sel_active;     /* TRUE once a drag exceeded threshold */

    /* Link hit-test rects produced during paint. */
    WebLinkRect *link_rects;
    size_t       link_rects_n;
    size_t       link_rects_cap;

    /* Animation timer arm guard. The 0xB003 timer fires on render_hwnd
     * (the consumer's render area). The HWND is passed at arm time;
     * the arm-active flag lives here so a consumer can ask "am I
     * currently animating?" without polling the timer manager. */
    int          anim_timer_active;
} WebRenderView;

/* Initialize a heap-allocated WebRenderView (or reset an existing one
 * to a fresh empty state without freeing buffers). The selection
 * sentinels (-1) are applied; all other fields zeroed. Owned buffers
 * (sel_map, link_rects) are not freed by this call -- callers managing
 * a long-lived view should call this only on a fresh zeroed struct. */
void web_render_view_init(WebRenderView *view);

/* ------------------------------------------------------------------ */
/* Process-global storage (NOT per-view). Defined in src/web_module.c. */
/* Re-introduced in Phase 6R-B alongside the matching `static`-strip   */
/* in the source.                                                      */
/* ------------------------------------------------------------------ */

extern BOOL      g_b_libwww_inited;
extern BOOL      g_b_ole_inited;
extern BOOL      g_b_gdiplus_inited;
extern ULONG_PTR g_b_gdiplus_token;

/* Headless token-dump recorder: one writer at a time by contract. */
extern FILE *g_b_token_recorder;
extern int   g_b_token_line_index;

/* Font cache. Array size is the private FONT_CACHE_SIZE macro inside
 * src/web_module.c; the header declares an unsized extern so a peer
 * compilation unit can read the symbol without dragging in the
 * cache-size constant. */
extern HFONT g_b_font_cache[];

/* ------------------------------------------------------------------ */
/* Engine API. Each entry point takes WebRenderView *view as its       */
/* first parameter; the Retro Web shell passes &g_b_view (its file-    */
/* scope singleton) and Phase 6b's Active Desktop module will pass a   */
/* second instance. Function bodies live in src/web_module.c; symbols  */
/* have external linkage so a peer compilation unit can call them.     */
/* ------------------------------------------------------------------ */

/* WebDoc lifecycle (free side; peer modules construct WebDoc via the
 * fetch helpers in web_module.h and free with this). */
void        webdoc_free(WebDoc *doc);

void        web_paint_to(WebRenderView *view, HDC hdc, RECT *client,
                         BOOL measure_only, int *out_height);
void        web_update_scroll(WebRenderView *view, HWND hwnd);

void        web_anim_arm(WebRenderView *view, HWND hwnd);
void        web_anim_disarm(WebRenderView *view, HWND hwnd);

void        web_clear_link_rects(WebRenderView *view);
const char *web_hit_test_link(WebRenderView *view, int x, int y);

void        web_sel_clear(WebRenderView *view, HWND hwnd);
void        web_sel_select_all(WebRenderView *view, HWND hwnd);
void        web_sel_copy_to_clipboard(WebRenderView *view, HWND hwnd);
int         web_sel_hit_test(WebRenderView *view, int mx, int my,
                             int *out_idx, int *out_off);

#endif /* RENDER_ENGINE_H */
