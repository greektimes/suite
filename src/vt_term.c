/*
 * vt_term.c - VT220 terminal emulator core. See vt_term.h.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#include "vt_term.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* Row storage helpers.                                                   */
/* ---------------------------------------------------------------------- */

static void row_blank(VTCell *row, int cols)
{
    int i;
    for (i = 0; i < cols; i++) {
        row[i].ch    = ' ';
        row[i].flags = VT_FLAG_NONE;
    }
}

static VTCell *row_ptr(const VTTerm *t, int row_index)
{
    return &t->lines[(size_t)row_index * (size_t)t->cols];
}

static int total_min(const VTTerm *t)
{
    return t->rows;
}

static int total_cap(const VTTerm *t)
{
    return t->scrollback_max + t->rows;
}

static void grow_to(VTTerm *t, int new_capacity_rows)
{
    int cap = t->lines_capacity;
    VTCell *p;
    if (new_capacity_rows <= cap) return;
    while (cap < new_capacity_rows) cap = cap ? cap * 2 : 32;
    if (cap > total_cap(t)) cap = total_cap(t);
    p = (VTCell *)realloc(t->lines,
                          (size_t)cap * (size_t)t->cols * sizeof(VTCell));
    if (!p) return;
    t->lines          = p;
    t->lines_capacity = cap;
}

/* Drop the oldest row if we are at the cap and need room for one more. */
static void drop_oldest_if_full(VTTerm *t)
{
    int cap = total_cap(t);
    if (t->lines_total < cap) return;
    memmove(t->lines, &t->lines[t->cols],
            (size_t)(t->lines_total - 1) * (size_t)t->cols * sizeof(VTCell));
    t->lines_total -= 1;
    /* Polish 6 (2026-05-22): the eviction shifted every logical row
     * index down by 1. The selection's anchor / lead point at row
     * indices, so we shift them too. If either falls below 0 the
     * content they pointed at is gone -- clear the selection. */
    if (t->sel_active) {
        t->sel_anchor_row -= 1;
        t->sel_lead_row   -= 1;
        if (t->sel_anchor_row < 0 || t->sel_lead_row < 0) {
            t->sel_active     = 0;
            t->sel_anchor_row = 0;
            t->sel_anchor_col = 0;
            t->sel_lead_row   = 0;
            t->sel_lead_col   = 0;
        }
    }
}

/* Append one blank row to the bottom. Returns the new visible-region
 * top index (i.e. the index of visible row 0 inside `lines`). */
static int append_blank_row(VTTerm *t)
{
    int idx;
    drop_oldest_if_full(t);
    grow_to(t, t->lines_total + 1);
    idx = t->lines_total;
    t->lines_total += 1;
    row_blank(row_ptr(t, idx), t->cols);
    return idx;
}

static int visible_top_index(const VTTerm *t)
{
    return t->lines_total - t->rows;
}

/* ---------------------------------------------------------------------- */
/* Init / free / resize / clear.                                          */
/* ---------------------------------------------------------------------- */

void vt_term_init(VTTerm *t, int cols, int rows, int scrollback)
{
    int i;
    if (!t) return;
    memset(t, 0, sizeof(*t));
    if (cols < 1) cols = 80;
    if (rows < 1) rows = 24;
    if (scrollback < 0) scrollback = 0;

    t->cols           = cols;
    t->rows           = rows;
    t->scrollback_max = scrollback;
    t->cursor_x       = 0;
    t->cursor_y       = 0;
    t->scroll_offset  = 0;
    t->pstate         = VTP_NORMAL;
    t->csi_len        = 0;
    t->lines_total    = 0;
    t->lines_capacity = 0;
    t->lines          = NULL;

    grow_to(t, rows);
    for (i = 0; i < rows; i++) append_blank_row(t);
}

void vt_term_free(VTTerm *t)
{
    if (!t) return;
    if (t->lines) free(t->lines);
    t->lines = NULL;
    t->lines_capacity = 0;
    t->lines_total = 0;
}

void vt_term_clear_visible(VTTerm *t)
{
    int i;
    int top;
    if (!t) return;
    top = visible_top_index(t);
    for (i = top; i < t->lines_total; i++) row_blank(row_ptr(t, i), t->cols);
    t->cursor_x = 0;
    t->cursor_y = 0;
    t->scroll_offset = 0;
}

void vt_term_set_lf_to_crlf(VTTerm *t, int enable)
{
    if (!t) return;
    t->lf_to_crlf = enable ? 1 : 0;
}

void vt_term_clear_all(VTTerm *t)
{
    int i;
    if (!t) return;
    /* Collapse to just the visible region. The ring of rows in `lines`
     * has its newest `rows` entries at the bottom; by lowering
     * lines_total to t->rows we drop everything that came before. The
     * underlying capacity stays allocated for reuse. */
    if (t->lines_total > t->rows) t->lines_total = t->rows;
    for (i = 0; i < t->rows; i++) row_blank(row_ptr(t, i), t->cols);
    t->cursor_x = 0;
    t->cursor_y = 0;
    t->scroll_offset = 0;
}

void vt_term_resize(VTTerm *t, int cols, int rows)
{
    VTCell *newbuf;
    int    new_total;
    int    new_cap;
    int    i, j;
    int    copy_cols;

    if (!t || cols < 1 || rows < 1) return;
    if (cols == t->cols && rows == t->rows) return;

    /* Build a fresh buffer at the new cols and copy row-by-row.
     * Visible region stays the *last* min(rows, lines_total) rows. */
    new_cap = t->scrollback_max + rows;
    if (new_cap < rows) new_cap = rows;

    newbuf = (VTCell *)calloc((size_t)new_cap * (size_t)cols, sizeof(VTCell));
    if (!newbuf) return;

    new_total = t->lines_total;
    if (new_total > new_cap) {
        /* keep newest `new_cap` rows */
        new_total = new_cap;
    }

    copy_cols = (cols < t->cols) ? cols : t->cols;
    for (i = 0; i < new_total; i++) {
        int src_idx = (t->lines_total - new_total) + i;
        VTCell *src = row_ptr(t, src_idx);
        VTCell *dst = &newbuf[(size_t)i * (size_t)cols];
        for (j = 0; j < cols; j++) {
            if (j < copy_cols) dst[j] = src[j];
            else { dst[j].ch = ' '; dst[j].flags = VT_FLAG_NONE; }
        }
    }
    if (new_total < rows) {
        /* pad with blank rows so visible region is filled */
        int pad = rows - new_total;
        int k;
        for (k = 0; k < pad; k++) {
            VTCell *dst = &newbuf[(size_t)(new_total + k) * (size_t)cols];
            int x;
            for (x = 0; x < cols; x++) { dst[x].ch = ' '; dst[x].flags = 0; }
        }
        new_total = rows;
    }

    free(t->lines);
    t->lines = newbuf;
    t->lines_capacity = new_cap;
    t->lines_total = new_total;
    t->cols = cols;
    t->rows = rows;
    if (t->cursor_x >= cols) t->cursor_x = cols - 1;
    if (t->cursor_y >= rows) t->cursor_y = rows - 1;
    if (t->scroll_offset > (t->lines_total - t->rows))
        t->scroll_offset = t->lines_total - t->rows;
    if (t->scroll_offset < 0) t->scroll_offset = 0;
    (void)total_min;
}

/* ---------------------------------------------------------------------- */
/* Cursor + writing.                                                      */
/* ---------------------------------------------------------------------- */

static void newline_with_scroll(VTTerm *t)
{
    if (t->cursor_y + 1 < t->rows) {
        t->cursor_y += 1;
    } else {
        /* scroll up: append a blank row at the bottom; visible region
         * naturally shifts up because `lines_total` grew. */
        append_blank_row(t);
        /* cursor stays at last visible row */
    }
}

static void put_printable(VTTerm *t, unsigned char b)
{
    VTCell *row;
    if (t->cursor_x >= t->cols) {
        t->cursor_x = 0;
        newline_with_scroll(t);
    }
    row = row_ptr(t, visible_top_index(t) + t->cursor_y);
    row[t->cursor_x].ch    = (char)b;
    row[t->cursor_x].flags = VT_FLAG_NONE;
    t->cursor_x += 1;
}

static void handle_c0(VTTerm *t, unsigned char b)
{
    switch (b) {
    case 0x0A:               /* LF - advance row */
        t->stat_lf += 1;
        if (t->lf_to_crlf) t->cursor_x = 0;
        newline_with_scroll(t);
        break;
    case 0x0D:               /* CR - reset col, do NOT touch row */
        t->stat_cr += 1;
        t->cursor_x = 0;
        break;
    case 0x08:               /* BS */
        if (t->cursor_x > 0) t->cursor_x -= 1;
        break;
    case 0x09: {             /* HT - next multiple of 8 */
        int nx = (t->cursor_x / 8) * 8 + 8;
        if (nx >= t->cols) nx = t->cols - 1;
        t->cursor_x = nx;
        break;
    }
    case 0x07:               /* BEL */
    case 0x00:               /* NUL */
    default:
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* CSI dispatch.                                                          */
/* ---------------------------------------------------------------------- */

/* Parse up to 8 numeric params separated by ';' from the CSI buffer.
 * Returns the number of params written. Param positions with no
 * digits are filled with -1 so the caller can apply per-CSI defaults. */
static int parse_csi_params(const char *buf, int buf_len, int *out, int max_out)
{
    int i = 0;
    int n = 0;
    int seen_digit;
    int val;

    while (i < buf_len && n < max_out) {
        seen_digit = 0;
        val = 0;
        while (i < buf_len && buf[i] >= '0' && buf[i] <= '9') {
            val = val * 10 + (buf[i] - '0');
            seen_digit = 1;
            i++;
        }
        out[n++] = seen_digit ? val : -1;
        if (i < buf_len && buf[i] == ';') i++;
        else break;
    }
    return n;
}

static int default_param(const int *params, int nparams, int idx, int dflt)
{
    if (idx >= nparams)        return dflt;
    if (params[idx] < 0)       return dflt;
    return params[idx];
}

static void dispatch_csi(VTTerm *t, char final)
{
    int params[8];
    int nparams = parse_csi_params(t->csi_buf, t->csi_len, params, 8);
    int i, j;
    int top;

    switch (final) {
    case 'J': {  /* erase in display */
        int mode = default_param(params, nparams, 0, 0);
        top = visible_top_index(t);
        if (mode == 2) {
            for (i = 0; i < t->rows; i++)
                row_blank(row_ptr(t, top + i), t->cols);
        } else if (mode == 0) {
            VTCell *row = row_ptr(t, top + t->cursor_y);
            for (j = t->cursor_x; j < t->cols; j++) {
                row[j].ch = ' '; row[j].flags = 0;
            }
            for (i = t->cursor_y + 1; i < t->rows; i++)
                row_blank(row_ptr(t, top + i), t->cols);
        } else if (mode == 1) {
            VTCell *row;
            for (i = 0; i < t->cursor_y; i++)
                row_blank(row_ptr(t, top + i), t->cols);
            row = row_ptr(t, top + t->cursor_y);
            for (j = 0; j <= t->cursor_x && j < t->cols; j++) {
                row[j].ch = ' '; row[j].flags = 0;
            }
        }
        break;
    }
    case 'H':
    case 'f': {
        int r = default_param(params, nparams, 0, 1);
        int c = default_param(params, nparams, 1, 1);
        if (r < 1) r = 1;
        if (c < 1) c = 1;
        t->cursor_y = r - 1;
        t->cursor_x = c - 1;
        if (t->cursor_y >= t->rows) t->cursor_y = t->rows - 1;
        if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
        break;
    }
    case 'A': {
        int n = default_param(params, nparams, 0, 1);
        if (n < 1) n = 1;
        t->cursor_y -= n;
        if (t->cursor_y < 0) t->cursor_y = 0;
        break;
    }
    case 'B': {
        int n = default_param(params, nparams, 0, 1);
        if (n < 1) n = 1;
        t->cursor_y += n;
        if (t->cursor_y >= t->rows) t->cursor_y = t->rows - 1;
        break;
    }
    case 'C': {
        int n = default_param(params, nparams, 0, 1);
        if (n < 1) n = 1;
        t->cursor_x += n;
        if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
        break;
    }
    case 'D': {
        int n = default_param(params, nparams, 0, 1);
        if (n < 1) n = 1;
        t->cursor_x -= n;
        if (t->cursor_x < 0) t->cursor_x = 0;
        break;
    }
    case 'K': {
        int mode = default_param(params, nparams, 0, 0);
        VTCell *row;
        top = visible_top_index(t);
        row = row_ptr(t, top + t->cursor_y);
        if (mode == 0) {
            for (j = t->cursor_x; j < t->cols; j++) {
                row[j].ch = ' '; row[j].flags = 0;
            }
        } else if (mode == 1) {
            for (j = 0; j <= t->cursor_x && j < t->cols; j++) {
                row[j].ch = ' '; row[j].flags = 0;
            }
        } else if (mode == 2) {
            row_blank(row, t->cols);
        }
        break;
    }
    case 'm':
        /* SGR - monochrome. Consumed. */
        break;
    default:
        /* Unknown final - consumed. */
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* feed_bytes.                                                            */
/* ---------------------------------------------------------------------- */

void vt_term_feed_bytes(VTTerm *t, const unsigned char *buf, size_t len)
{
    size_t i;
    if (!t || !buf || len == 0) return;

    /* Receiving new data slides back to the bottom, matching most terms. */
    if (t->scroll_offset != 0) t->scroll_offset = 0;

    for (i = 0; i < len; i++) {
        unsigned char b = buf[i];
        switch (t->pstate) {
        case VTP_NORMAL:
            if (b == 0x1B) {
                t->pstate = VTP_ESC_SEEN;
            } else if (b < 0x20) {
                handle_c0(t, b);
            } else if (b == 0x7F) {
                /* DEL ignored */
            } else {
                put_printable(t, b);
            }
            break;

        case VTP_ESC_SEEN:
            if (b == '[') {
                t->pstate = VTP_CSI_COLLECTING;
                t->csi_len = 0;
            } else {
                /* Unknown ESC sequence; discard ESC + this byte. */
                t->pstate = VTP_NORMAL;
            }
            break;

        case VTP_CSI_COLLECTING:
            if (b >= 0x40 && b <= 0x7E) {
                dispatch_csi(t, (char)b);
                t->pstate = VTP_NORMAL;
                t->csi_len = 0;
            } else if (t->csi_len < (int)sizeof(t->csi_buf) - 1) {
                t->csi_buf[t->csi_len++] = (char)b;
            } else {
                /* Buffer overflow - abandon sequence to keep going. */
                t->pstate = VTP_NORMAL;
                t->csi_len = 0;
            }
            break;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Scrolling and view access.                                             */
/* ---------------------------------------------------------------------- */

int vt_term_scrollback_count(const VTTerm *t)
{
    if (!t) return 0;
    return t->lines_total - t->rows;
}

int vt_term_scroll_offset(const VTTerm *t)
{
    if (!t) return 0;
    return t->scroll_offset;
}

void vt_term_scroll_by(VTTerm *t, int lines)
{
    int sb;
    if (!t) return;
    sb = vt_term_scrollback_count(t);
    t->scroll_offset += lines;
    if (t->scroll_offset > sb) t->scroll_offset = sb;
    if (t->scroll_offset < 0)  t->scroll_offset = 0;
}

void vt_term_scroll_to_bottom(VTTerm *t)
{
    if (!t) return;
    t->scroll_offset = 0;
}

const VTCell *vt_term_get_visible(const VTTerm *t, int row)
{
    int top;
    int actual_row;
    if (!t || row < 0 || row >= t->rows) return NULL;
    top = visible_top_index(t) - t->scroll_offset;
    if (top < 0) top = 0;
    actual_row = top + row;
    if (actual_row < 0 || actual_row >= t->lines_total) return NULL;
    return row_ptr(t, actual_row);
}

/* ---------------------------------------------------------------------- */
/* Selection (polish 6, 2026-05-22).                                      */
/* ---------------------------------------------------------------------- */

void vt_term_select_clear(VTTerm *t)
{
    if (!t) return;
    t->sel_active     = 0;
    t->sel_anchor_row = 0;
    t->sel_anchor_col = 0;
    t->sel_lead_row   = 0;
    t->sel_lead_col   = 0;
}

void vt_term_select_set(VTTerm *t,
                        int anchor_row, int anchor_col,
                        int lead_row,   int lead_col)
{
    if (!t) return;
    t->sel_anchor_row = anchor_row;
    t->sel_anchor_col = anchor_col;
    t->sel_lead_row   = lead_row;
    t->sel_lead_col   = lead_col;
    t->sel_active     = 1;
}

void vt_term_select_all(VTTerm *t)
{
    if (!t || t->lines_total <= 0) return;
    t->sel_anchor_row = 0;
    t->sel_anchor_col = 0;
    t->sel_lead_row   = t->lines_total - 1;
    t->sel_lead_col   = t->cols;       /* half-open: includes whole row */
    t->sel_active     = 1;
}

int vt_term_visible_to_logical(const VTTerm *t, int visible_row)
{
    int top_logical;
    if (!t) return -1;
    top_logical = t->lines_total - t->rows - t->scroll_offset;
    return top_logical + visible_row;
}

int vt_term_logical_to_visible(const VTTerm *t, int logical_row)
{
    int top_logical, vis;
    if (!t) return -1;
    top_logical = t->lines_total - t->rows - t->scroll_offset;
    vis = logical_row - top_logical;
    if (vis < 0 || vis >= t->rows) return -1;
    return vis;
}

/* Normalize (anchor, lead) into (start, end) in reading order. */
static void sel_normalize(const VTTerm *t,
                          int *sr, int *sc, int *er, int *ec)
{
    int ar = t->sel_anchor_row, ac = t->sel_anchor_col;
    int lr = t->sel_lead_row,   lc = t->sel_lead_col;
    if (ar < lr || (ar == lr && ac <= lc)) {
        *sr = ar; *sc = ac; *er = lr; *ec = lc;
    } else {
        *sr = lr; *sc = lc; *er = ar; *ec = ac;
    }
}

int vt_term_cell_selected(const VTTerm *t, int logical_row, int col)
{
    int sr, sc, er, ec;
    if (!t || !t->sel_active) return 0;
    sel_normalize(t, &sr, &sc, &er, &ec);
    if (logical_row < sr || logical_row > er) return 0;
    if (sr == er)              return (col >= sc && col < ec);
    if (logical_row == sr)     return (col >= sc);
    if (logical_row == er)     return (col <  ec);
    return 1;
}

char *vt_term_select_extract(const VTTerm *t)
{
    int sr, sc, er, ec;
    char *buf;
    size_t cap, used;
    int row;

    if (!t || !t->sel_active) return NULL;
    sel_normalize(t, &sr, &sc, &er, &ec);
    if (sr == er && sc == ec) return NULL;  /* zero-width */

    cap  = 256;
    used = 0;
    buf  = (char *)malloc(cap);
    if (!buf) return NULL;

    for (row = sr; row <= er; row++) {
        int col_lo, col_hi, i, line_end;
        const VTCell *cells;

        col_lo = (row == sr) ? sc : 0;
        col_hi = (row == er) ? ec : t->cols;
        if (col_hi > t->cols) col_hi = t->cols;
        if (col_lo < 0)       col_lo = 0;

        /* The selection is stored in logical (ring-index) coords. A
         * row outside the current ring (row < 0 or >= lines_total)
         * has been evicted; skip it gracefully. */
        if (row < 0 || row >= t->lines_total) {
            cells = NULL;
        } else {
            cells = row_ptr((VTTerm *)t, row);
        }

        /* Copy the visible portion of the row into buf. */
        line_end = used;
        for (i = col_lo; i < col_hi; i++) {
            char ch = ' ';
            if (cells) {
                unsigned char raw = (unsigned char)cells[i].ch;
                ch = (raw >= 0x20 && raw < 0x7F) ? (char)raw : ' ';
            }
            if (used + 4 >= cap) {
                size_t ncap = cap * 2;
                char *nbuf = (char *)realloc(buf, ncap);
                if (!nbuf) { free(buf); return NULL; }
                buf = nbuf; cap = ncap;
            }
            buf[used++] = ch;
        }

        /* Trim trailing spaces on this line (but not the leading
         * whitespace -- the user may have selected indented text). */
        while (used > (size_t)line_end && buf[used - 1] == ' ') used--;

        if (row < er) {
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
