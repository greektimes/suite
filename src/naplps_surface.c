/*
 * naplps_surface.c - 256x200 indexed videotex surface for the NAPLPS
 *                    renderer, plus the raster primitives the decoder
 *                    drives.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from ANSI X3.110-1983 / CSA T500-1983. No Win32, no GDI.
 * See naplps_decode.h for the full provenance statement.
 */

#include <string.h>
#include "naplps_surface.h"

/* ------------------------------------------------------------------ */
/* Colour map                                                          */
/* ------------------------------------------------------------------ */

/*
 * The default colour map, 5.3.2.5.2 and Figure 25, for N = 4 and M = 9.
 *
 * First half, addresses 0 to 7: "The first half of the default color map
 * is used to store a complete, uniformly spaced grey scale. This
 * comprises the ordered set of colors where G = R = B." So entry k is
 * (k,k,k), giving nominal black at 0 and nominal white at 7.
 *
 * Second half, addresses 8 to 15: "a full range of hues equally spaced
 * around the perimeter of the hue circle", blue at 0 degrees, red at
 * 120, green at 240, walked counterclockwise from 0 in 45 degree steps.
 * The standard's algorithm sets the closest primary to all ones, the
 * furthest to zero, and the second closest to |ang h - ang P1| / 60
 * scaled over the primary's range.
 *
 * These eight were computed from that algorithm and then compared
 * against the worked table in Figure 25. They agree on all eight, which
 * is what settles the two 0.5 cases (90 and 270 degrees, entries 10 and
 * 14) where the scaled value lands exactly between 3 and 4: the figure
 * rounds them up to 4.
 *
 * Order within each triple is GREEN, RED, BLUE, the standard's own
 * order, "which is the order of decreasing luminance" (5.3.1.1).
 */
static const naplps_color_t naplps_default_map[NAPLPS_CMAP_SIZE] = {
    { 0, 0, 0 },   /*  0  nominal black                */
    { 1, 1, 1 },   /*  1                               */
    { 2, 2, 2 },   /*  2                               */
    { 3, 3, 3 },   /*  3   grey scale                  */
    { 4, 4, 4 },   /*  4                               */
    { 5, 5, 5 },   /*  5                               */
    { 6, 6, 6 },   /*  6                               */
    { 7, 7, 7 },   /*  7  nominal white                */
    { 0, 0, 7 },   /*  8  hue   0 deg, blue            */
    { 0, 5, 7 },   /*  9  hue  45 deg, violet          */
    { 0, 7, 4 },   /* 10  hue  90 deg, magenta-red     */
    { 2, 7, 0 },   /* 11  hue 135 deg, red-orange      */
    { 7, 7, 0 },   /* 12  hue 180 deg, yellow          */
    { 7, 2, 0 },   /* 13  hue 225 deg, yellow-green    */
    { 7, 0, 4 },   /* 14  hue 270 deg, green-cyan      */
    { 5, 0, 7 }    /* 15  hue 315 deg, cyan-blue       */
};

void naplps_surface_default_cmap(naplps_surface_t *s)
{
    if (!s) return;
    memcpy(s->cmap, naplps_default_map, sizeof(naplps_default_map));
}

void naplps_surface_clear(naplps_surface_t *s, unsigned char addr)
{
    if (!s) return;
    memset(s->pix, (int)(addr & (NAPLPS_CMAP_SIZE - 1)), sizeof(s->pix));
}

void naplps_surface_init(naplps_surface_t *s)
{
    if (!s) return;
    naplps_surface_default_cmap(s);
    naplps_surface_clear(s, 0);
}

/* ------------------------------------------------------------------ */
/* Texture masks                                                       */
/* ------------------------------------------------------------------ */

void naplps_mask_solid(naplps_mask_t *m)
{
    int i;
    if (!m) return;
    for (i = 0; i < NAPLPS_MASK_DIM; i++) m->row[i] = 0xFFFF;
    m->kind = NAPLPS_MASK_BITMAP;
    m->pw = m->ph = 1;
    m->off_addr = -1;
}

/*
 * Area texture patterns, Table 13 and Figure 21 (5.3.2.4.4).
 *
 * CORRECTED 2026-09-26. This used to say the standard does not fix the
 * built-in patterns and drew eight of this renderer's own invention
 * (dots, checker, diagonals). That was wrong: Table 13 fixes 000 solid,
 * 001 vertical hatching, 010 horizontal hatching, 011 cross-hatching and
 * 1xx the programmable masks A-D, Figure 21 draws them, and the SRM's
 * conformance table requires all four predefined patterns to be
 * presented. The real-client gate found it: Personality+III drew the
 * Phase 2b cloud map's horizontal, vertical and cross hatches, and this
 * renderer drew a checker, dots and horizontal lines for the same bytes.
 *
 * The text fixes the geometry too: "The width and spacing of hatching
 * lines in the vertical hatching pattern are equal to the width of the
 * logical pel. The height and spacing of hatching lines in the
 * horizontal hatching pattern are equal to the height of the logical
 * pel." A line therefore starts every second pel, counted from the
 * surface origin so that registration holds across figures. "If the
 * logical pel size is (0,0), solid texture patterns will always be
 * drawn"; the caller decides that, since only it knows the unrounded
 * pel. Masks A-D are never defined here (DEF TEXTURE is skipped and
 * counted), so they keep their default "null texture pattern, resulting
 * in no fill in color modes 0 and 1, and a fill with the background
 * color in color mode 2".
 */
void naplps_mask_pattern(naplps_mask_t *m, int pattern, int pw, int ph,
                         int off_addr)
{
    if (!m) return;
    naplps_mask_solid(m);
    m->pw = pw < 1 ? 1 : pw;
    m->ph = ph < 1 ? 1 : ph;
    switch (pattern & 7) {
    case 0:  return;                               /* solid: all on      */
    case 1:  m->kind = NAPLPS_MASK_VHATCH; break;
    case 2:  m->kind = NAPLPS_MASK_HHATCH; break;
    case 3:  m->kind = NAPLPS_MASK_XHATCH; break;
    default: m->kind = NAPLPS_MASK_NULL;   break;  /* masks A-D, null    */
    }
    m->off_addr = off_addr;
}

static int mask_test(const naplps_mask_t *m, int x, int y)
{
    int mx, my;
    if (!m) return 1;
    /* Fills are clipped to the surface first, so x and y are never
     * negative here and plain division gives the pel index. */
    switch (m->kind) {
    case NAPLPS_MASK_VHATCH: return ((x / m->pw) & 1) == 0;
    case NAPLPS_MASK_HHATCH: return ((y / m->ph) & 1) == 0;
    case NAPLPS_MASK_XHATCH: return ((x / m->pw) & 1) == 0 ||
                                    ((y / m->ph) & 1) == 0;
    case NAPLPS_MASK_NULL:   return 0;
    default: break;
    }
    /* C's % can go negative; the surface never passes a negative here,
     * but a fill clipped at the left edge could, so normalise. */
    mx = x % NAPLPS_MASK_DIM; if (mx < 0) mx += NAPLPS_MASK_DIM;
    my = y % NAPLPS_MASK_DIM; if (my < 0) my += NAPLPS_MASK_DIM;
    return (m->row[my] >> mx) & 1;
}

/* One pixel of an area fill: the drawing colour where the mask is on,
 * and in colour mode 2 the background where it is off (5.3.2.4.4: "In
 * color mode 2, the fill areas not drawn in the drawing color are drawn
 * in the background color"). */
static void mask_put(naplps_surface_t *s, const naplps_mask_t *m,
                     int x, int y, unsigned char addr)
{
    if (mask_test(m, x, y))
        s->pix[y * NAPLPS_W + x] = (unsigned char)(addr & (NAPLPS_CMAP_SIZE - 1));
    else if (m && m->off_addr >= 0)
        s->pix[y * NAPLPS_W + x] =
            (unsigned char)(m->off_addr & (NAPLPS_CMAP_SIZE - 1));
}

/* ------------------------------------------------------------------ */
/* Raster primitives                                                   */
/* ------------------------------------------------------------------ */

void naplps_plot(naplps_surface_t *s, int x, int y, unsigned char addr)
{
    if (!s) return;
    if (x < 0 || x >= NAPLPS_W || y < 0 || y >= NAPLPS_H) return;
    s->pix[y * NAPLPS_W + x] = (unsigned char)(addr & (NAPLPS_CMAP_SIZE - 1));
}

/*
 * Lay the logical pel down with its drawing point at (x,y).
 *
 * 5.3.2.2.6: the logical pel is a rectangle whose alignment relative to
 * the drawing point depends on the SIGNS of dx and dy, and "Note that if
 * the width and height of the logical pel are both reduced to 0, the
 * logical pel reduces to the dimensionless drawing point." The
 * dimensionless case still has to mark something, so it marks one pixel.
 */
static void stamp_pel(naplps_surface_t *s, int x, int y,
                      int pw, int ph, unsigned char addr)
{
    int ix, iy, x0, y0, w, h;

    w = pw < 0 ? -pw : pw;
    h = ph < 0 ? -ph : ph;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    x0 = (pw < 0) ? x - (w - 1) : x;
    y0 = (ph < 0) ? y - (h - 1) : y;

    for (iy = 0; iy < h; iy++)
        for (ix = 0; ix < w; ix++)
            naplps_plot(s, x0 + ix, y0 + iy, addr);
}

void naplps_draw_point(naplps_surface_t *s, int x, int y,
                       int pw, int ph, unsigned char addr)
{
    stamp_pel(s, x, y, pw, ph, addr);
}

/*
 * Bresenham, with the logical pel as the brush and an optional 16-bit
 * repeating line texture consulted once per step. A texture of 0xFFFF is
 * a solid line, which is the default.
 */
void naplps_draw_line(naplps_surface_t *s, int x0, int y0, int x1, int y1,
                      int pw, int ph, unsigned char addr,
                      unsigned short line_texture)
{
    int dx, dy, sx, sy, err, e2, step;

    if (!s) return;
    if (line_texture == 0) line_texture = 0xFFFF;

    dx = x1 - x0; if (dx < 0) dx = -dx;
    dy = y1 - y0; if (dy < 0) dy = -dy;
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx - dy;
    step = 0;

    for (;;) {
        if ((line_texture >> (step & 15)) & 1)
            stamp_pel(s, x0, y0, pw, ph, addr);
        step++;

        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void naplps_fill_rect(naplps_surface_t *s, int x, int y, int w, int h,
                      unsigned char addr, const naplps_mask_t *mask)
{
    int ix, iy, x1, y1;

    if (!s) return;

    /* Normalise a negative extent into a positive rectangle. */
    if (w < 0) { x += w + 1; w = -w; }
    if (h < 0) { y += h + 1; h = -h; }
    if (w <= 0 || h <= 0) return;

    x1 = x + w; y1 = y + h;
    if (x  < 0) x = 0;
    if (y  < 0) y = 0;
    if (x1 > NAPLPS_W) x1 = NAPLPS_W;
    if (y1 > NAPLPS_H) y1 = NAPLPS_H;

    for (iy = y; iy < y1; iy++)
        for (ix = x; ix < x1; ix++)
            mask_put(s, mask, ix, iy, addr);
}

/*
 * Scanline polygon fill, even-odd.
 *
 * The standard requires at least 256 vertices (5.3.3.5.1). The crossing
 * buffer is sized well past that so a long polygon cannot overflow it;
 * anything beyond is dropped rather than scribbling.
 */
#define MAX_CROSS 1024

void naplps_fill_poly(naplps_surface_t *s, const int *pts, int n,
                      unsigned char addr, const naplps_mask_t *mask)
{
    int y, i, j, nc, tmp, xs, xe, x;
    int cross[MAX_CROSS];

    if (!s || !pts || n < 3) return;

    for (y = 0; y < NAPLPS_H; y++) {
        /* Sample scan lines through pixel centres so that a polygon
         * whose edge lands exactly on an integer row fills predictably
         * instead of flickering on and off with rounding. */
        int yc2 = 2 * y + 1;              /* 2 * (y + 0.5) */
        nc = 0;

        for (i = 0, j = n - 1; i < n; j = i++) {
            int ay = pts[2 * i + 1], by = pts[2 * j + 1];
            int ax = pts[2 * i],     bx = pts[2 * j];
            int lo = (ay < by) ? ay : by;
            int hi = (ay < by) ? by : ay;

            if (2 * lo > yc2 || 2 * hi <= yc2) continue;
            if (ay == by) continue;

            if (nc < MAX_CROSS) {
                /* x at the crossing, rounded to the nearest pixel. */
                long num = (long)(bx - ax) * (long)(yc2 - 2 * ay);
                long den = (long)(by - ay) * 2;
                cross[nc++] = ax + (int)(num / den);
            }
        }

        if (nc < 2) continue;

        /* Insertion sort: nc is small and this keeps it allocation-free. */
        for (i = 1; i < nc; i++) {
            tmp = cross[i];
            for (j = i - 1; j >= 0 && cross[j] > tmp; j--)
                cross[j + 1] = cross[j];
            cross[j + 1] = tmp;
        }

        for (i = 0; i + 1 < nc; i += 2) {
            xs = cross[i];
            xe = cross[i + 1];
            if (xe < 0 || xs >= NAPLPS_W) continue;
            if (xs < 0) xs = 0;
            if (xe > NAPLPS_W - 1) xe = NAPLPS_W - 1;
            for (x = xs; x <= xe; x++)
                mask_put(s, mask, x, y, addr);
        }
    }
}

void naplps_stroke_poly(naplps_surface_t *s, const int *pts, int n,
                        int closed, int pw, int ph, unsigned char addr,
                        unsigned short line_texture)
{
    int i;

    if (!s || !pts || n < 2) return;

    for (i = 0; i + 1 < n; i++)
        naplps_draw_line(s, pts[2 * i], pts[2 * i + 1],
                         pts[2 * i + 2], pts[2 * i + 3],
                         pw, ph, addr, line_texture);

    if (closed && n > 2)
        naplps_draw_line(s, pts[2 * (n - 1)], pts[2 * (n - 1) + 1],
                         pts[0], pts[1], pw, ph, addr, line_texture);
}

/* ------------------------------------------------------------------ */
/* Presentation                                                        */
/* ------------------------------------------------------------------ */

/* 0..7 component to 0..255, with 7 mapping to full intensity. */
static int comp8(int c)
{
    if (c < 0) c = 0;
    if (c > NAPLPS_PRIMARY_MAX) c = NAPLPS_PRIMARY_MAX;
    return (c * 255) / NAPLPS_PRIMARY_MAX;
}

/*
 * Luminance for the monochrome view.
 *
 * The standard does not give a luminance equation; what it gives is the
 * ORDERING, "green, red, blue, which is the order of decreasing
 * luminance" (5.3.1.1). Rec. 601 luma weights respect that ordering and
 * are the ordinary choice, so they are used here. The grey half of the
 * default colour map is unaffected by the choice, because G = R = B
 * makes every weighting agree; only the hue half depends on it, and the
 * hue half is exactly what the monochrome view exists to show collapsing.
 */
int naplps_color_luma(const naplps_color_t *c)
{
    int g, r, b, y;
    if (!c) return 0;
    g = comp8(c->g); r = comp8(c->r); b = comp8(c->b);
    y = (587 * g + 299 * r + 114 * b) / 1000;
    if (y < 0) y = 0;
    if (y > 255) y = 255;
    return y;
}

void naplps_surface_to_bgra(const naplps_surface_t *s, unsigned char *out,
                            int mono)
{
    int y, x, i;
    unsigned char lut[NAPLPS_CMAP_SIZE][3];   /* B, G, R */

    if (!s || !out) return;

    for (i = 0; i < NAPLPS_CMAP_SIZE; i++) {
        if (mono) {
            int l = naplps_color_luma(&s->cmap[i]);
            lut[i][0] = (unsigned char)l;
            lut[i][1] = (unsigned char)l;
            lut[i][2] = (unsigned char)l;
        } else {
            lut[i][0] = (unsigned char)comp8(s->cmap[i].b);
            lut[i][1] = (unsigned char)comp8(s->cmap[i].g);
            lut[i][2] = (unsigned char)comp8(s->cmap[i].r);
        }
    }

    /*
     * THE Y FLIP, and the only one in the renderer. Surface row 0 is the
     * BOTTOM scan line because the standard's origin is the lower left
     * (4.2.2); a top-down DIB wants the top row first.
     */
    for (y = 0; y < NAPLPS_H; y++) {
        const unsigned char *src = &s->pix[(NAPLPS_H - 1 - y) * NAPLPS_W];
        unsigned char *dst = out + (size_t)y * NAPLPS_W * 4;
        for (x = 0; x < NAPLPS_W; x++) {
            const unsigned char *c = lut[src[x] & (NAPLPS_CMAP_SIZE - 1)];
            *dst++ = c[0];
            *dst++ = c[1];
            *dst++ = c[2];
            *dst++ = 0xFF;
        }
    }
}
