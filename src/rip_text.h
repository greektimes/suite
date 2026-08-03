/*
 * rip_text.h - RIPscrip text rendering: font 0 bitmap and fonts 1-10
 *              stroked, sized and placed per the 1.54 METRIC tables.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from docs/ripscrip-ref/RIPSCRIP-1.54.DOC. No Win32, no GDI.
 */

#ifndef RIP_TEXT_H_INCLUDED
#define RIP_TEXT_H_INCLUDED

#include "rip_ega.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RIP_FONT_STYLE direction values. */
#define RIP_DIR_HORIZ  0
#define RIP_DIR_VERT   1

/* Draw `text` with its cell top-left at (x, y), which is what RIP_TEXT
 * and RIP_TEXT_XY mean by the drawing position. `font` is 0-10, `size`
 * is 1-10 and `dir` is RIP_DIR_*. Returns the advance in pixels along
 * the text direction, so the caller can move the drawing position. */
int rip_text_draw(RipEga *s, int font, int dir, int size,
                  int x, int y, const char *text, int color);

/* Advance in pixels the same text would take, without drawing. */
int rip_text_width(int font, int size, const char *text);

/* Height of one character cell for this font and size, in pixels. */
int rip_text_cell_height(int font, int size);

/* Row offset from the top of the cell at which to draw a hot-key
 * underline. 1.54 says the underline sits below the baseline and lower
 * still under characters with descenders, but does not publish the
 * offset, so this is the spec's own `drop` (the lowest pixel of the
 * cell) plus a two pixel gap. */
int rip_text_underline_offset(int font, int size);

/* Horizontal advance override for the stroked fonts (1 to 10).
 *
 * Glyph SHAPES always come from Hershey. Only the pen step changes. The
 * Hershey faces are a few percent wider than the stroked fonts the
 * service assumes when it computes host-side text placement, so without
 * an override a long centred string overruns the space budgeted for it.
 *
 * The renderer prefers a width table when one is set, then a tracking
 * factor, then the glyph's own Hershey advance.
 *
 *   rip_text_set_tracking(font, factor)
 *       One multiplier per font. Ships no third-party data and is
 *       approximate: a small per-string residual remains.
 *   rip_text_set_advance_table(font, adv)
 *       adv[c - 32] is the advance in Hershey units at natural size for
 *       c in 32 to 126, or NULL to clear. Exact, but it is measured data
 *       rather than anything the 1.54 document publishes.
 *
 * Both are cleared by default, so the untouched renderer is pure
 * Hershey. */
void rip_text_set_tracking(int font, double factor);
void rip_text_set_advance_table(int font, const short *adv);

/* Install the Suite's chosen overrides. Called from rip_parser_init so
 * the tab and the offline harness always agree. Idempotent. */
void rip_text_metrics_init(void);

#ifdef __cplusplus
}
#endif

#endif /* RIP_TEXT_H_INCLUDED */
