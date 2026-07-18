/*
 * vt_term.h - Minimal VT220 terminal emulator core.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Line-buffer model: a ring of rows, each of which is a flat array of
 * Cell. The visible region is the last `rows` lines of the ring; the
 * scrollback is everything older. Cursor coordinates are in the
 * visible region (0-indexed). Decoupled from any specific transport
 * (Telnet, SSH, serial) so the same core can back any of them.
 *
 * Subset implemented (matches what telnet.greektimes.ca sends):
 *   ESC [ 2 J         clear entire screen
 *   ESC [ H / [ ; H   cursor home (1;1)
 *   ESC [ r ; c H     cursor to (row, col) (1-indexed VT)
 *   ESC [ r ; c f     same as H
 *   ESC [ n A/B/C/D   cursor up/down/forward/back
 *   ESC [ n K         erase in line (0/1/2)
 *   ESC [ ... m       SGR -- consumed and ignored (monochrome)
 *   Anything else     consumed and ignored, no crash, no corruption
 *
 * C0 controls:
 *   LF (0x0A)         next row + scroll-up at bottom
 *   CR (0x0D)         column = 0
 *   BS (0x08)         column -= 1 (clamped)
 *   TAB (0x09)        next column multiple of 8
 *   BEL, NUL          ignored
 */

#ifndef VT_TERM_H
#define VT_TERM_H

#include <stddef.h>
#include <stdint.h>

#define VT_FLAG_NONE  0x00

typedef struct VTCell {
    char    ch;
    uint8_t flags;     /* reserved for future bold/underline */
} VTCell;

typedef enum VTParse {
    VTP_NORMAL = 0,
    VTP_ESC_SEEN,
    VTP_CSI_COLLECTING
} VTParse;

typedef struct VTTerm {
    int       cols;
    int       rows;
    int       cursor_x;
    int       cursor_y;

    int       scrollback_max;       /* maximum number of scrollback rows kept */

    /* Ring of rows. `lines` holds (scrollback_count + rows) rows, each
     * `cols` cells wide. New rows append at the bottom; once we hit
     * (scrollback_max + rows) the oldest row gets dropped (memmove). */
    VTCell   *lines;
    int       lines_total;          /* current row count in `lines` (>= rows) */
    int       lines_capacity;       /* allocated row capacity */

    int       scroll_offset;        /* 0 = at bottom; positive = lines back */

    VTParse   pstate;
    char      csi_buf[64];
    int       csi_len;

    /* When non-zero, an incoming LF (0x0A) also resets cursor_x to 0
     * before advancing the row. This matches the cooked-mode line
     * discipline that older Unix services (Finger, QOTD, many BBSes)
     * implicitly rely on: they send bare `\n` between rows and expect
     * the client to behave as if the kernel's tty driver had inserted
     * the CR for them. Default 0 (off) keeps vt_term semantically
     * clean for any future raw-mode use; toggle on per-protocol via
     * vt_term_set_lf_to_crlf. */
    int       lf_to_crlf;

    /* Polish 3 diagnostic counters (2026-05-22). Incremented when the
     * NORMAL-state byte dispatch sees each control byte; surfaced via
     * the F6 status line to confirm wire-level CR vs LF accounting. */
    unsigned long stat_cr;
    unsigned long stat_lf;

    /* Selection state (polish 6, 2026-05-22).
     *
     * Anchor and lead are stored in LOGICAL row coordinates: an int
     * counting every row ever written to the terminal, visible and
     * scrolled-out alike. Logical row = (lines_total - 1) refers to
     * the most recent row, decreasing into history. When scroll_offset
     * changes during a drag, the cell-on-screen position changes but
     * the logical anchor / lead stay put, so the selection sticks to
     * the content rather than to the viewport.
     *
     * The selection is half-open at end_col (cell at end_col is NOT
     * selected) to match xterm semantics. */
    int  sel_active;
    int  sel_anchor_row;   /* logical row */
    int  sel_anchor_col;   /* cell col within row, [0, cols) */
    int  sel_lead_row;
    int  sel_lead_col;
} VTTerm;

void   vt_term_init(VTTerm *t, int cols, int rows, int scrollback);
void   vt_term_free(VTTerm *t);

/* Resize to (cols, rows). cols=0 or rows=0 is a no-op. The cursor is
 * clamped into the new visible region; scrollback is preserved at the
 * new width by truncation/padding per row. */
void   vt_term_resize(VTTerm *t, int cols, int rows);

void   vt_term_feed_bytes(VTTerm *t, const unsigned char *buf, size_t len);

/* Positive lines = scroll up into history; negative = back toward
 * bottom. Clamps at [0, scrollback_count]. */
void   vt_term_scroll_by(VTTerm *t, int lines);
void   vt_term_scroll_to_bottom(VTTerm *t);
int    vt_term_scrollback_count(const VTTerm *t);
int    vt_term_scroll_offset(const VTTerm *t);

/* Returns a pointer to the row of `cols` Cells that should be drawn
 * at visible-row `row` (0..rows-1), honoring scroll_offset. May
 * return NULL if `row` is out of range. The buffer is owned by the
 * VTTerm; valid until the next vt_term_feed_bytes / vt_term_resize. */
const VTCell *vt_term_get_visible(const VTTerm *t, int row);

/* Blank the visible region; cursor home; scrollback preserved.
 * This is the right entry point for ESC [ 2 J -- a server-side
 * "clear screen" should not nuke the user's scrollback history. */
void   vt_term_clear_visible(VTTerm *t);

/* Blank the visible region AND drop all scrollback rows. After this
 * call, vt_term_scrollback_count() returns 0 and Page Up shows
 * nothing. Intended for the local shell `clear` command, which
 * should erase history along with the visible screen. */
void   vt_term_clear_all(VTTerm *t);

/* Toggle the LF-implies-CR behavior. See VTTerm::lf_to_crlf for why. */
void   vt_term_set_lf_to_crlf(VTTerm *t, int enable);

/* Selection model (polish 6, 2026-05-22). All coordinates are LOGICAL
 * (lines_total-relative); see VTTerm::sel_* for the model. */
void   vt_term_select_clear(VTTerm *t);
void   vt_term_select_set(VTTerm *t,
                          int anchor_row, int anchor_col,
                          int lead_row,   int lead_col);
void   vt_term_select_all(VTTerm *t);

/* True if the cell at (logical_row, col) lies within the active
 * selection in line-flow order. Returns 0 if !sel_active. */
int    vt_term_cell_selected(const VTTerm *t,
                             int logical_row, int col);

/* Extract the selected text. Lines joined with "\r\n". Trailing
 * spaces on each line stripped. Returns a malloc'd null-terminated
 * string the caller frees, or NULL if no active selection / empty. */
char  *vt_term_select_extract(const VTTerm *t);

/* Coordinate conversions used by mouse handlers.
 * visible_to_logical returns the logical row index for a given
 * on-screen row. logical_to_visible returns -1 if the logical row
 * is outside the currently visible window. */
int    vt_term_visible_to_logical(const VTTerm *t, int visible_row);
int    vt_term_logical_to_visible(const VTTerm *t, int logical_row);

#endif /* VT_TERM_H */
