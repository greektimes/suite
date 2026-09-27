/*
 * minitel_screen.c - render Minitel page memory to a 320 x 250 raster.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from STUM1B; provenance in minitel_decode.h. Printed page
 * numbers are cited.
 *
 * This translation unit is deliberately free of Win32 and GDI calls.
 */

#include "minitel_screen.h"
#include "minitel_font.h"
#include "naplps_font.h"

#include <string.h>

/*
 * The colours. In colour, the eight pure hues of the C1 table's order
 * (schema 2.4). In monochrome, STUM1B's own grey table (p32), "le tube
 * cathodique du Minitel etant en noir et blanc":
 *
 *     black 0%   blue 40%   red 50%   magenta 60%
 *     green 70%  cyan 80%   yellow 90%  white 100%
 *
 * The percentages are of luminance amplitude and "donnes a titre
 * indicatif". They are applied linearly to 0..255 here; a real tube's
 * gamma is a property of the tube, which this raster does not model.
 */
static const unsigned long rgb_colour[8] = {
    0x000000UL, 0xFF0000UL, 0x00FF00UL, 0xFFFF00UL,
    0x0000FFUL, 0xFF00FFUL, 0x00FFFFUL, 0xFFFFFFUL
};
static const int grey_pct[8] = {
    /* black red green yellow blue magenta cyan white */
       0,    50,  70,   90,    40,  60,     80,  100
};

unsigned long minitel_rgb(int c, int mono)
{
    c &= 7;
    if (mono) {
        unsigned long v = (unsigned long)((grey_pct[c] * 255 + 50) / 100);
        return (v << 16) | (v << 8) | v;
    }
    return rgb_colour[c];
}

/*
 * CRT scan lines, reproduced by OBSERVATION ONLY.
 *
 * MiniPavi's hosted emulator was captured on the same page with its
 * Scanlines switch off and then on (throwaway browser, monitor 2), and
 * the "on" pixels were fitted against the "off" pixels as on = m * off +
 * c, separately for each screen-row phase, screen-column phase and
 * channel, by least squares over the whole 640 x 500 screen. The fit
 * explains all 319,680 pixels that did not change between the captures
 * to within 0.8 of 255 (mean 0.06). What it found, at the emulator's 2x
 * scale:
 *
 *   - rows alternate: a LIT row keeps 243/255 of the colour, a DARK row
 *     133/255 (the "green" column below: 250/255 and 137/255);
 *   - columns repeat every 3 screen pixels as an RGB triad: an additive
 *     +10 red, +5 green, +10 blue in successive columns on a lit row
 *     (+1 green beside the red and blue ones), about half that on a dark
 *     row, which is what lifts black to a faint coloured grain;
 *   - NO GLOW: a black pixel beside a lit one is no brighter than an
 *     isolated one (7.01 against 6.96 summed, measured).
 *
 * The numbers below are those measurements, rounded. No emulator source
 * was read (it has no licence and is on the estate's DO-NOT-READ list).
 *
 * At other scales, which the emulator was not observed at: each Minitel
 * pixel row is split so its top half (rounded up) is lit and the rest
 * dark, which at 2x is exactly the observed pattern and keeps every row
 * alike at 3x; the triad stays a 3-pixel screen pattern, as a shadow
 * mask is a property of the tube, not of the picture. At 1x there is no
 * dark row. A MONOCHROME tube has no RGB triad, so in the monochrome
 * view only the rows are applied, with the triad's gains and offsets
 * averaged into one grey figure (lit 245/255 + 3, dark 134/255 + 2).
 */
static const int sl_gain[2][3] = {      /* [dark][column phase], /255 */
    { 243, 250, 243 },
    { 133, 137, 133 }
};
static const int sl_off[2][3][3] = {    /* [dark][column phase][R G B] */
    { { 10, 1,  0 }, { 0, 5, 0 }, { 0, 1, 10 } },
    { {  5, 1,  0 }, { 0, 3, 0 }, { 0, 1,  5 } }
};

static unsigned char sl_apply(int v, int gain, int off)
{
    v = (v * gain + 127) / 255 + off;
    return (unsigned char)(v > 255 ? 255 : v);
}

void minitel_present_bgra(const unsigned char *src, int scale,
                          unsigned char *dst, int opts)
{
    int dw, dh, x, y, lit_rows;
    int scan = (opts & MT_P_SCANLINES) != 0;
    int mono = (opts & MT_P_MONO) != 0;

    if (scale < 1) scale = 1;
    dw = MINITEL_W * scale;
    dh = MINITEL_H * scale;
    lit_rows = (scale + 1) / 2;

    for (y = 0; y < dh; y++) {
        const unsigned char *srow = src + (size_t)(y / scale) * MINITEL_W * 4;
        unsigned char *d = dst + (size_t)y * dw * 4;
        int dark = (y % scale) >= lit_rows;
        for (x = 0; x < dw; x++, d += 4) {
            const unsigned char *s = srow + (size_t)(x / scale) * 4;
            if (!scan) {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0;
            } else if (mono) {
                int g = dark ? 134 : 245, o = dark ? 2 : 3;
                d[0] = sl_apply(s[0], g, o);
                d[1] = sl_apply(s[1], g, o);
                d[2] = sl_apply(s[2], g, o);
                d[3] = 0;
            } else {
                int k = x % 3;
                int g = sl_gain[dark][k];
                d[2] = sl_apply(s[2], g, sl_off[dark][k][0]);   /* R */
                d[1] = sl_apply(s[1], g, sl_off[dark][k][1]);   /* G */
                d[0] = sl_apply(s[0], g, sl_off[dark][k][2]);   /* B */
                d[3] = 0;
            }
        }
    }
}

/* One dot of an alphabetic glyph in the 8 x 10 cell. */
static int glyph_dot(int g, int x, int y)
{
    if (g == 0x20) return 0;
    if (g >= MT_G_FULLBLOCK && g < MT_G_END)
        return (minitel_ext8x10[g - MT_G_FULLBLOCK][y] >> x) & 1;
    switch (g) {
    case 0x5C: return (minitel_g0special8x10[0][y] >> x) & 1;
    case 0x5E: return (minitel_g0special8x10[1][y] >> x) & 1;
    case 0x60: return (minitel_g0special8x10[2][y] >> x) & 1;
    case 0x7B: return (minitel_g0special8x10[3][y] >> x) & 1;
    case 0x7C: return (minitel_g0special8x10[4][y] >> x) & 1;
    case 0x7D: return (minitel_g0special8x10[5][y] >> x) & 1;
    case 0x7E: return (minitel_g0special8x10[6][y] >> x) & 1;
    default: break;
    }
    /* The ASCII cells: the Suite's own 5 x 7 glyphs, placed one dot in
     * from the top-left corner of the 8 x 10 matrix (p30). */
    if (g < 0x21 || g > 0x7E) return 0;
    if (x < 1 || x > 5 || y < 1 || y > 7) return 0;
    return (naplps_font5x7[g - 0x20][y - 1] >> (x - 1)) & 1;
}

/*
 * One dot of a mosaic. The 8 x 10 matrix is cut into 2 x 3 blocks
 * (p31). STUM1B does not say how the ten lines divide among three block
 * rows (schema 2.6 draws them equal, which ten lines cannot be); this
 * renderer uses 3 / 4 / 3, top / middle / bottom, symmetric about the
 * cell's centre. Columns are 4 / 4.
 *
 * SEPARATED ("disjoint", p31): "2 barres verticales et 3 barres
 * horizontales appartenant au fond de l'ecran sont alors visualisees".
 * Here each block gives up its right-hand column and its bottom line to
 * the paper: two vertical bars (dots 3 and 7) and three horizontal bars
 * (lines 2, 6 and 9). Which side of the block the bars fall on is not
 * fixed by the text; this is the choice made, and it is recorded.
 */
static int mosaic_dot(int bits, int sep, int x, int y)
{
    int bx = x < 4 ? 0 : 1;
    int by = y < 3 ? 0 : (y < 7 ? 1 : 2);
    if (sep && (x == 3 || x == 7 || y == 2 || y == 6 || y == 9)) return 0;
    return (bits >> (by * 2 + bx)) & 1;
}

static void put_px(unsigned char *out, int x, int y, unsigned long rgb)
{
    unsigned char *p = out + ((size_t)y * MINITEL_W + (size_t)x) * 4;
    p[0] = (unsigned char)(rgb & 0xFF);
    p[1] = (unsigned char)((rgb >> 8) & 0xFF);
    p[2] = (unsigned char)((rgb >> 16) & 0xFF);
    p[3] = 0;
}

void minitel_render_bgra(const minitel_decoder_t *d, unsigned char *out,
                         int opts)
{
    int mono = (opts & MT_R_MONO) != 0;
    int flash_off = (opts & MT_R_FLASH) != 0;
    int cur_r = minitel_cursor_row(d), cur_c = minitel_cursor_col(d);
    int show_cursor = minitel_cursor_visible(d) && (opts & MT_R_CURSOR);
    int screen_masked = minitel_screen_masked(d);
    int row, col, x, y;

    for (row = 0; row < MINITEL_ROWS; row++) {
        /*
         * The serial context of this row, carried left to right by the
         * display processor. Every row starts from the defaults: black
         * paper, unmasked, unlined (p88).
         */
        int z_bg = MT_BLACK, z_mask = 0, z_ul = 0;

        for (col = 1; col <= MINITEL_COLS; col++) {
            const minitel_cell_t *c = minitel_cell(d, row, col);
            int ink, paper, masked, lined = 0, flash;
            int dw = (c->size & MT_S_DW) != 0, dh = (c->size & MT_S_DH) != 0;
            int right = (c->size & MT_S_RIGHT) != 0, top = (c->size & MT_S_TOP) != 0;

            if (c->kind == MT_KIND_DELIM) {
                z_bg   = c->bg;
                z_mask = (c->flags & MT_F_ZMASK) != 0;
                z_ul   = (c->flags & MT_F_ZUNDER) != 0;
            }

            masked = z_mask && screen_masked;
            flash  = (c->flags & MT_F_FLASH) && flash_off;

            if (c->kind == MT_KIND_MOSAIC) {
                ink = c->fg; paper = c->bg;
            } else if (c->kind == MT_KIND_DELIM) {
                /* "visualise comme un espace non souligne, non clignotant
                 * et ayant tous les autres attributs courants" (p84); it
                 * takes the inverse attribute into account (p86 note 2). */
                ink = c->fg; paper = c->bg;
                if (c->flags & MT_F_INVERT) paper = c->fg;
                ink = paper;
            } else {
                ink = c->fg; paper = z_bg;
                if (c->flags & MT_F_INVERT) { int t = ink; ink = paper; paper = t; }
                lined = z_ul;
            }

            if (row == cur_r && col == cur_c && show_cursor) {
                /* "l'alternance du caractere ... avec la meme forme dans
                 * la couleur complementaire. Le fond ... alterne aussi"
                 * (p99). Always visible, even on a delimiter or in a
                 * masked zone. */
                ink ^= 7; paper ^= 7; masked = 0;
            }

            for (y = 0; y < MINITEL_CELL_H; y++) {
                for (x = 0; x < MINITEL_CELL_W; x++) {
                    int on = 0;
                    if (!masked && !flash) {
                        if (c->kind == MT_KIND_MOSAIC) {
                            on = mosaic_dot(c->glyph,
                                            (c->flags & MT_F_SEPARATED) != 0, x, y);
                        } else if (c->kind == MT_KIND_ALPHA) {
                            /* Double sizes are the glyph doubled, "engendres
                             * a partir du bas gauche" (p91): this cell shows
                             * its quarter of a 16 x 20 image. */
                            int gx = dw ? ((right ? 8 : 0) + x) / 2 : x;
                            int gy = dh ? ((top ? 0 : 10) + y) / 2 : y;
                            on = glyph_dot(c->glyph, gx, gy);
                            /* Underline: "une barre continue, toujours en
                             * simple hauteur, occupant la derniere ligne de
                             * l'emplacement" (p85). Not on the top half of a
                             * double height, and never on DEL in code (p98). */
                            if (lined && y == MINITEL_CELL_H - 1 && !(dh && top)
                                && c->glyph != MT_G_FULLBLOCK)
                                on = 1;
                        }
                    }
                    put_px(out, (col - 1) * MINITEL_CELL_W + x,
                           row * MINITEL_CELL_H + y,
                           minitel_rgb(on ? ink : paper, mono));
                }
            }

            if (c->kind == MT_KIND_MOSAIC) {
                /* p86 note 1: a mosaic is the paper delimiter for the
                 * alphabetic zone that follows it; note 4: lining does not
                 * come back after a semi-graphic zone. */
                z_bg = c->bg;
                z_ul = 0;
            }
        }
    }

    /* The terminal's own state letter, row 00 column 39, "en noir sur
     * fond blanc" (p32). */
    if (minitel_get_status_letter(d)) {
        int g = (unsigned char)minitel_get_status_letter(d);
        for (y = 0; y < MINITEL_CELL_H; y++)
            for (x = 0; x < MINITEL_CELL_W; x++)
                put_px(out, 38 * MINITEL_CELL_W + x, y,
                       minitel_rgb(glyph_dot(g, x, y) ? MT_BLACK : MT_WHITE, mono));
    }
}
