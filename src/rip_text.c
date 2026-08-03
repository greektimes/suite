/*
 * rip_text.c - RIPscrip text rendering.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from docs/ripscrip-ref/RIPSCRIP-1.54.DOC. No Win32, no GDI.
 *
 * HOW THE SIZE IS DERIVED, and why no oracle was needed.
 *
 * 1.54 publishes a METRIC table for every font at every size, giving four
 * numbers measured in scan lines from the top of the character cell:
 * top (to the top of the character), bow (to the crest), base (to the
 * baseline) and drop (to the lowermost pixel). Those tables are
 * reproduced below verbatim and are the only sizing input.
 *
 * The capital height of a font at a given size is therefore base - top.
 * Every Hershey face in this set has a natural capital height of exactly
 * 21 units (measured from 'H' at generation time; all nine agree), so
 *
 *     scale = (base - top) / 21
 *
 * Evaluated at size 4, which the BGI size ladder makes the natural size,
 * that expression yields exactly 1.000 for Triplex, Small's parent,
 * Sans Serif, Gothic, Script, Simplex, Triplex Script and Complex, and
 * 2.000 for European. Seven exact unit matches is not a coincidence: the
 * stroked fonts RIPscrip names are the Hershey set, so the spec's own
 * vertical metrics size our Hershey glyphs correctly with no reference
 * to any terminal's font files.
 *
 * The baseline is placed at y + base, again straight from the table.
 *
 * Font 0 is the bitmap font and needs none of this: the table's own
 * numbers (base = 7*size - 1, drop = 8*size - 1) confirm it is a plain
 * pixel replication of an 8x8 cell, which is what the spec's note about
 * enlarged pixels and a jagged look describes.
 */

#include "rip_text.h"
#include "rip_font8x8.h"
#include "rip_hershey.h"

#include <string.h>

/* --------------------------------------------------------------------
 * The 1.54 font METRIC tables, [font 0-10][size 1-10], as { top, base }.
 * bow and drop are not needed for placement, so they are not carried.
 * ------------------------------------------------------------------ */

typedef struct { unsigned char top, base; } RipMetric;

static const RipMetric RIP_METRICS[11][10] = {
    /* 0: Default 8x8 */
    { {0,6},{0,13},{0,20},{0,27},{0,34},{0,41},{0,48},{0,55},{0,62},{0,69} },
    /* 1: Triplex */
    { {6,18},{6,20},{8,23},{10,31},{13,41},{16,51},{20,62},{25,77},{30,93},{40,124} },
    /* 2: Small */
    { {2,5},{2,6},{2,6},{3,9},{4,12},{5,15},{6,13},{7,22},{9,27},{12,36} },
    /* 3: Sans Serif */
    { {7,19},{7,21},{9,24},{11,32},{14,42},{18,53},{22,64},{28,80},{33,96},{74,158} },
    /* 4: Gothic */
    { {7,19},{7,21},{9,24},{11,32},{14,42},{18,53},{22,64},{28,80},{33,96},{44,128} },
    /* 5: Script */
    { {10,22},{10,24},{12,27},{16,37},{21,49},{26,61},{32,74},{40,92},{48,111},{63,147} },
    /* 6: Simplex */
    { {9,21},{9,23},{11,26},{14,35},{18,46},{23,58},{28,70},{35,87},{42,105},{56,140} },
    /* 7: Triplex Script */
    { {5,17},{5,19},{7,22},{9,30},{12,40},{15,50},{19,61},{24,77},{29,92},{39,123} },
    /* 8: Complex */
    { {8,20},{8,22},{10,25},{13,34},{17,45},{22,57},{27,69},{34,86},{41,104},{54,139} },
    /* 9: European */
    { {7,32},{7,35},{9,40},{12,54},{16,72},{20,96},{25,109},{31,136},{38,164},{51,219} },
    /* 10: Bold */
    { {11,35},{13,39},{14,44},{19,59},{19,59},{19,59},{19,59},{19,59},{19,59},{19,59} }
};

/* The 1.54 Bold table prints only sizes 1 to 4; rows 5 to 10 above repeat
 * size 4 rather than invent numbers the document does not publish. The
 * server does not emit font 10, so nothing on screen depends on it. */

/* The drop column of the same METRIC tables: scan lines from the top of
 * the cell to the lowermost pixel. Kept alongside rather than folded
 * into RipMetric so the verified top and base data above is untouched.
 * Only the hot-key underline needs it. */
static const short RIP_DROP[11][10] = {
    {  7, 15, 23, 31, 39, 47, 55, 63, 71,  79 },  /* 0  Default 8x8    */
    { 22, 24, 28, 38, 50, 62, 76, 94,114, 152 },  /* 1  Triplex        */
    {  6,  7,  7, 11, 14, 18, 22, 27, 33,  44 },  /* 2  Small          */
    { 23, 25, 29, 39, 51, 64, 78, 97,117, 186 },  /* 3  Sans Serif     */
    { 23, 25, 29, 39, 51, 64, 78, 97,117, 156 },  /* 4  Gothic         */
    { 29, 32, 36, 49, 65, 80, 98,122,147, 195 },  /* 5  Script         */
    { 25, 27, 31, 42, 56, 69, 84,104,126, 168 },  /* 6  Simplex        */
    { 21, 23, 27, 37, 49, 61, 75, 93,113, 151 },  /* 7  Triplex Script */
    { 24, 26, 30, 41, 54, 68, 83,103,125, 167 },  /* 8  Complex        */
    { 38, 41, 47, 64, 85,106,129,161,194, 259 },  /* 9  European       */
    { 39, 43, 49, 66, 66, 66, 66, 66, 66,  66 }   /* 10 Bold (1-4 only)*/
};

/* ------------------------------------------------------------------ */
/* Advance overrides. See rip_text.h for what these are for.           */
/* ------------------------------------------------------------------ */

static double       g_track[11];   /* 0 = unset, else advance multiplier */
static const short *g_adv[11];     /* NULL = unset, else per-glyph width */

void rip_text_set_tracking(int font, double factor)
{
    if (font >= 1 && font <= 10) g_track[font] = factor;
}

void rip_text_set_advance_table(int font, const short *adv)
{
    if (font >= 1 && font <= 10) g_adv[font] = adv;
}

void rip_text_metrics_init(void)
{
    /* Gothic is the only font on this service whose text collides: the
     * masthead is the widest string on the menu and ran 289 px into the
     * 280 px the service budgeted, pushing "Times" under the Script tag.
     * 280 / 289 = 0.969. The other emitted fonts (1, 2 and 5) show no
     * collision and stay on their natural Hershey advance. */
    rip_text_set_tracking(4, 0.969);
}

static void clampfs(int *font, int *size)
{
    if (*font < 0 || *font > 10) *font = 0;
    if (*size < 1) *size = 1;
    if (*size > 10) *size = 10;
}

int rip_text_cell_height(int font, int size)
{
    clampfs(&font, &size);
    if (font == 0) return 8 * size;
    /* Cell height is not published directly; drop is the lowest pixel and
     * the tables put the cell a little below it. base plus the descender
     * allowance the ladder implies is close enough for callers that only
     * need a line height. */
    return RIP_METRICS[font][size - 1].base +
           (RIP_METRICS[font][size - 1].base -
            RIP_METRICS[font][size - 1].top) / 4 + 1;
}

int rip_text_underline_offset(int font, int size)
{
    clampfs(&font, &size);
    /* Below the lowest pixel of the cell, with a two pixel gap. The
     * document asks for the underline to clear descenders; starting from
     * `drop`, which is already the lowest pixel the font can reach, does
     * that for every character without a per-character rule. */
    return RIP_DROP[font][size - 1] + 2;
}

/* ------------------------------------------------------------------ */
/* Font 0: 8x8 bitmap, pixel replicated                                */
/* ------------------------------------------------------------------ */

static int bitmap_draw(RipEga *s, int dir, int size, int x, int y,
                       const char *text, int color, int draw)
{
    int adv = 0;
    const unsigned char *p = (const unsigned char *)text;

    for (; *p; p++) {
        int c = *p;
        if (c > 127) c = '?';
        if (draw) {
            int row, col, dx, dy;
            for (row = 0; row < 8; row++) {
                unsigned char bits = rip_font8x8[c][row];
                for (col = 0; col < 8; col++) {
                    if (!(bits & (1u << col))) continue;
                    /* Replicate each source pixel into a size x size block. */
                    for (dy = 0; dy < size; dy++) {
                        for (dx = 0; dx < size; dx++) {
                            int px, py;
                            if (dir == RIP_DIR_VERT) {
                                /* Vertical text reads bottom to top. */
                                px = x + row * size + dx;
                                py = y - adv - col * size - dy;
                            } else {
                                px = x + adv + col * size + dx;
                                py = y + row * size + dy;
                            }
                            rip_ega_pixel(s, px, py, color);
                        }
                    }
                }
            }
        }
        adv += 8 * size;
    }
    return adv;
}

/* ------------------------------------------------------------------ */
/* Fonts 1-10: Hershey strokes                                         */
/* ------------------------------------------------------------------ */

/* Scale is a rational (num / den) so the whole path stays in integers:
 * num = base - top (the spec's capital height), den = 21 (Hershey's). */
static int scaled(int v, int num, int den)
{
    /* Round to nearest, symmetric about zero. */
    if (v >= 0) return (v * num + den / 2) / den;
    return -(((-v) * num + den / 2) / den);
}

/* Pen step, in output pixels, after drawing `ch`. `nat` is the glyph's
 * own Hershey advance in Hershey units; the glyph is still drawn from
 * the Hershey outline, only the step changes. */
static int advance_for(int font, int ch, int nat, int num)
{
    if (font >= 1 && font <= 10) {
        if (g_adv[font] && ch >= 32 && ch <= 126)
            nat = g_adv[font][ch - 32];
        else if (g_track[font] > 0.0)
            nat = (int)(nat * g_track[font] + 0.5);
    }
    return scaled(nat, num, RIP_HERSHEY_CAP_UNITS);
}

static int stroke_draw(RipEga *s, int font, int dir, int size,
                       int x, int y, const char *text, int color, int draw)
{
    const RipHersheyFace *face;
    const RipMetric *m;
    int num, adv = 0;
    const unsigned char *p = (const unsigned char *)text;

    face = &rip_hershey_faces[rip_hershey_font_map[font]];
    m = &RIP_METRICS[font][size - 1];
    num = (int)m->base - (int)m->top;
    if (num <= 0) num = 1;

    for (; *p; p++) {
        int c = *p;
        const RipHersheyGlyph *g;
        int i, have_prev = 0, px_prev = 0, py_prev = 0;

        if (c < 0x20 || c > 0x7E) c = '?';
        g = &face->glyphs[c - 0x20];

        if (draw) {
            for (i = 0; i < g->count; i++) {
                int hx = face->points[(g->first + i) * 2];
                int hy = face->points[(g->first + i) * 2 + 1];
                int ox, oy, px, py;

                if (hx == RIP_HERSHEY_PENUP) { have_prev = 0; continue; }

                /* Hershey space to pixels: x from the glyph's left edge,
                 * y from the baseline, then placed at the cell top plus
                 * the spec's base offset. */
                ox = scaled(hx - g->left, num, RIP_HERSHEY_CAP_UNITS);
                oy = scaled(hy - RIP_HERSHEY_BASELINE, num,
                            RIP_HERSHEY_CAP_UNITS);

                if (dir == RIP_DIR_VERT) {
                    px = x + m->base + oy;
                    py = y - adv - ox;
                } else {
                    px = x + adv + ox;
                    py = y + m->base + oy;
                }

                if (have_prev)
                    rip_ega_line(s, px_prev, py_prev, px, py, color, 1);
                else
                    rip_ega_pixel(s, px, py, color);

                px_prev = px;
                py_prev = py;
                have_prev = 1;
            }
        }
        adv += advance_for(font, c, g->right - g->left, num);
    }
    return adv;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

int rip_text_draw(RipEga *s, int font, int dir, int size,
                  int x, int y, const char *text, int color)
{
    if (!s || !text || !*text) return 0;
    clampfs(&font, &size);
    if (dir != RIP_DIR_VERT) dir = RIP_DIR_HORIZ;
    if (font == 0) return bitmap_draw(s, dir, size, x, y, text, color, 1);
    return stroke_draw(s, font, dir, size, x, y, text, color, 1);
}

int rip_text_width(int font, int size, const char *text)
{
    if (!text || !*text) return 0;
    clampfs(&font, &size);
    if (font == 0) return bitmap_draw(0, RIP_DIR_HORIZ, size, 0, 0, text, 0, 0);
    return stroke_draw(0, font, RIP_DIR_HORIZ, size, 0, 0, text, 0, 0);
}
