/*
 * rip_ega.c - 640x350 16-colour indexed EGA surface and raster primitives.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from docs/ripscrip-ref/RIPSCRIP-1.54.DOC. No Win32, no GDI.
 */

#include "rip_ega.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

/* --------------------------------------------------------------------
 * The 64-colour EGA master palette.
 *
 * An EGA palette register holds six bits. The low three bits are the
 * primary R, G, B and the high three bits are the secondary (half
 * intensity) r, g, b, so a channel's level is 2*primary + secondary and
 * the four levels map to 0, 85, 170, 255:
 *
 *     bit  5   4   3   2   1   0
 *          r   g   b   R   G   B
 *
 * Checked against the values the specification itself names: 63 is
 * White (all levels 3), 7 is Light Gray (all levels 2), 56 is Dark Gray
 * (all levels 1), 1 is Blue, 20 is Brown (170, 85, 0).
 *
 * SPECIFICATION CONFLICT, resolved deliberately: the RIP_SET_PALETTE and
 * RIP_ONE_PALETTE tables in 1.54 list the default palette's entry 06
 * ("Brown") as master 7 and entry 07 ("Light Gray") as master 20, which
 * is those two rows' master values transposed: master 7 resolves to
 * (170,170,170) and master 20 to (170,85,0). The colour NAMES in those
 * tables, the names in RIP_COLOR's own table, and the EGA hardware
 * default palette all agree that entry 6 is brown and entry 7 is light
 * gray. We follow the names and the hardware, so the default palette
 * below puts master 20 at index 6 and master 7 at index 7.
 * ------------------------------------------------------------------ */

static const unsigned char RIP_DEFAULT_PAL16[16] = {
     0,  /* 00 Black         */
     1,  /* 01 Blue          */
     2,  /* 02 Green         */
     3,  /* 03 Cyan          */
     4,  /* 04 Red           */
     5,  /* 05 Magenta       */
    20,  /* 06 Brown         */
     7,  /* 07 Light Gray    */
    56,  /* 08 Dark Gray     */
    57,  /* 09 Light Blue    */
    58,  /* 0A Light Green   */
    59,  /* 0B Light Cyan    */
    60,  /* 0C Light Red     */
    61,  /* 0D Light Magenta */
    62,  /* 0E Yellow        */
    63   /* 0F White         */
};

void rip_ega_invert_rect(RipEga *s, int x0, int y0, int x1, int y1)
{
    int x, y, t;

    if (!s) return;
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > RIP_EGA_W - 1) x1 = RIP_EGA_W - 1;
    if (y1 > RIP_EGA_H - 1) y1 = RIP_EGA_H - 1;
    if (x0 > x1 || y0 > y1) return;

    /* Straight into the pixel array: see the header on why the viewport
     * and the write mode do not apply to press feedback. */
    for (y = y0; y <= y1; y++) {
        unsigned char *row = &s->pix[(size_t)y * RIP_EGA_W];
        for (x = x0; x <= x1; x++)
            row[x] = (unsigned char)(~row[x] & 0x0F);
    }
}

void rip_ega_master_rgb(int master, unsigned char *r,
                        unsigned char *g, unsigned char *b)
{
    int hi, lo;
    if (master < 0) master = 0;
    if (master > 63) master = 63;

    lo = (master >> 3) & 7;   /* secondary r g b */
    hi = master & 7;          /* primary   R G B */

    *r = (unsigned char)((((hi >> 2) & 1) * 2 + ((lo >> 2) & 1)) * 85);
    *g = (unsigned char)((((hi >> 1) & 1) * 2 + ((lo >> 1) & 1)) * 85);
    *b = (unsigned char)((((hi     ) & 1) * 2 + ((lo     ) & 1)) * 85);
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

void rip_ega_reset(RipEga *s)
{
    if (!s) return;
    memcpy(s->pal16, RIP_DEFAULT_PAL16, sizeof s->pal16);
    s->vx0 = 0;
    s->vy0 = 0;
    s->vx1 = RIP_EGA_W - 1;
    s->vy1 = RIP_EGA_H - 1;
    s->viewport_disabled = 0;
    s->write_mode = RIP_WMODE_COPY;
    memset(s->pix, 0, sizeof s->pix);
}

void rip_ega_erase_view(RipEga *s)
{
    int y;
    if (!s || s->viewport_disabled) return;
    for (y = s->vy0; y <= s->vy1; y++) {
        if (y < 0 || y >= RIP_EGA_H) continue;
        memset(s->pix + (size_t)y * RIP_EGA_W + s->vx0, 0,
               (size_t)(s->vx1 - s->vx0 + 1));
    }
}

void rip_ega_set_palette16(RipEga *s, const unsigned char master[16])
{
    int i;
    if (!s || !master) return;
    for (i = 0; i < 16; i++)
        if (master[i] <= 63) s->pal16[i] = master[i];
}

void rip_ega_set_one_palette(RipEga *s, int color, int master)
{
    if (!s) return;
    if (color < 0 || color > 15) return;
    if (master < 0 || master > 63) return;
    s->pal16[color] = (unsigned char)master;
}

void rip_ega_set_viewport(RipEga *s, int x0, int y0, int x1, int y1)
{
    if (!s) return;

    /* All parameters zero disables the graphics window (1.54: commands
     * that adhere to the viewport are parsed but ignored). */
    if (x0 == 0 && y0 == 0 && x1 == 0 && y1 == 0) {
        s->viewport_disabled = 1;
        s->vx0 = s->vy0 = 0;
        s->vx1 = s->vy1 = 0;
        return;
    }
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > RIP_EGA_W - 1) x1 = RIP_EGA_W - 1;
    if (y1 > RIP_EGA_H - 1) y1 = RIP_EGA_H - 1;
    s->vx0 = x0; s->vy0 = y0;
    s->vx1 = x1; s->vy1 = y1;
    s->viewport_disabled = 0;
}

/* ------------------------------------------------------------------ */
/* Primitives                                                          */
/* ------------------------------------------------------------------ */

/* The one place a pixel is written. Clips to the viewport and applies
 * the write mode. */
void rip_ega_pixel(RipEga *s, int x, int y, int color)
{
    unsigned char *p;
    if (!s || s->viewport_disabled) return;
    if (x < s->vx0 || x > s->vx1 || y < s->vy0 || y > s->vy1) return;
    if (x < 0 || x >= RIP_EGA_W || y < 0 || y >= RIP_EGA_H) return;

    p = &s->pix[(size_t)y * RIP_EGA_W + x];
    if (s->write_mode == RIP_WMODE_XOR)
        *p = (unsigned char)((*p ^ color) & 0x0F);
    else
        *p = (unsigned char)(color & 0x0F);
}

/* Bresenham. Thickness 3 (the specification's "thick" line) is drawn as
 * three parallel runs across the minor axis, which is what a 3-pixel
 * BGI line looks like. Any other thickness draws a single run. */
static void ega_line_1px(RipEga *s, int x0, int y0, int x1, int y1, int color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        rip_ega_pixel(s, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

void rip_ega_line(RipEga *s, int x0, int y0, int x1, int y1,
                  int color, int thickness)
{
    if (!s) return;
    if (thickness == 3) {
        if (abs(x1 - x0) >= abs(y1 - y0)) {
            ega_line_1px(s, x0, y0 - 1, x1, y1 - 1, color);
            ega_line_1px(s, x0, y0,     x1, y1,     color);
            ega_line_1px(s, x0, y0 + 1, x1, y1 + 1, color);
        } else {
            ega_line_1px(s, x0 - 1, y0, x1 - 1, y1, color);
            ega_line_1px(s, x0,     y0, x1,     y1, color);
            ega_line_1px(s, x0 + 1, y0, x1 + 1, y1, color);
        }
        return;
    }
    ega_line_1px(s, x0, y0, x1, y1, color);
}

void rip_ega_rect(RipEga *s, int x0, int y0, int x1, int y1,
                  int color, int thickness)
{
    if (!s) return;
    rip_ega_line(s, x0, y0, x1, y0, color, thickness);
    rip_ega_line(s, x1, y0, x1, y1, color, thickness);
    rip_ega_line(s, x1, y1, x0, y1, color, thickness);
    rip_ega_line(s, x0, y1, x0, y0, color, thickness);
}

/* Fills ignore write mode per the specification (RIP_BAR does not list
 * Write Mode, and RIP_FILL_POLYGON says the interior fill does not use
 * it), so they write the index directly through a copy-mode helper. */
static void ega_fill_span(RipEga *s, int y, int xa, int xb, int color)
{
    int x;
    if (s->viewport_disabled) return;
    if (y < s->vy0 || y > s->vy1) return;
    if (y < 0 || y >= RIP_EGA_H) return;
    if (xa > xb) { int t = xa; xa = xb; xb = t; }
    if (xa < s->vx0) xa = s->vx0;
    if (xb > s->vx1) xb = s->vx1;
    if (xa < 0) xa = 0;
    if (xb > RIP_EGA_W - 1) xb = RIP_EGA_W - 1;
    if (xa > xb) return;
    for (x = xa; x <= xb; x++)
        s->pix[(size_t)y * RIP_EGA_W + x] = (unsigned char)(color & 0x0F);
}

/* Pattern 00 fills with the background colour (index 0); every other
 * pattern currently fills solid in the fill colour. Patterns 02-0B are
 * hatches that are not implemented yet; rip_parser counts each use so
 * the gap is reported rather than hidden. */
static int ega_fill_index(int pattern, int fill_color)
{
    if (pattern == 0) return 0;
    return fill_color;
}

void rip_ega_bar(RipEga *s, int x0, int y0, int x1, int y1,
                 int pattern, int fill_color)
{
    int y, c;
    if (!s || s->viewport_disabled) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    c = ega_fill_index(pattern, fill_color);
    for (y = y0; y <= y1; y++)
        ega_fill_span(s, y, x0, x1, c);
}

/* Even-odd scanline fill. */
void rip_ega_fill_polygon(RipEga *s, const int *pts, int npoints,
                          int pattern, int fill_color)
{
    int ymin, ymax, y, i, c;
    int *xs;

    if (!s || !pts || npoints < 2 || s->viewport_disabled) return;
    c = ega_fill_index(pattern, fill_color);

    ymin = ymax = pts[1];
    for (i = 1; i < npoints; i++) {
        int yy = pts[i * 2 + 1];
        if (yy < ymin) ymin = yy;
        if (yy > ymax) ymax = yy;
    }
    if (ymin < s->vy0) ymin = s->vy0;
    if (ymax > s->vy1) ymax = s->vy1;
    if (ymin > ymax) return;

    xs = (int *)malloc(sizeof(int) * (size_t)npoints);
    if (!xs) return;

    for (y = ymin; y <= ymax; y++) {
        int n = 0, j, k;
        for (i = 0; i < npoints; i++) {
            int j2 = (i + 1) % npoints;
            int y0 = pts[i * 2 + 1], y1 = pts[j2 * 2 + 1];
            int x0 = pts[i * 2],     x1 = pts[j2 * 2];
            if (y0 == y1) continue;
            /* Half-open in y so shared vertices count once. */
            if ((y >= y0 && y < y1) || (y >= y1 && y < y0)) {
                xs[n++] = x0 + (int)(((double)(y - y0) * (x1 - x0))
                                     / (double)(y1 - y0));
            }
        }
        /* Insertion sort: n is small (polygon crossings on one line). */
        for (j = 1; j < n; j++) {
            int v = xs[j];
            for (k = j - 1; k >= 0 && xs[k] > v; k--) xs[k + 1] = xs[k];
            xs[k + 1] = v;
        }
        for (j = 0; j + 1 < n; j += 2)
            ega_fill_span(s, y, xs[j], xs[j + 1], c);
    }
    free(xs);
}

void rip_ega_polygon(RipEga *s, const int *pts, int npoints,
                     int color, int thickness)
{
    int i;
    if (!s || !pts || npoints < 2) return;
    for (i = 0; i < npoints; i++) {
        int j = (i + 1) % npoints;
        rip_ega_line(s, pts[i * 2], pts[i * 2 + 1],
                     pts[j * 2], pts[j * 2 + 1], color, thickness);
    }
}

void rip_ega_polyline(RipEga *s, const int *pts, int npoints,
                      int color, int thickness)
{
    int i;
    if (!s || !pts || npoints < 2) return;
    for (i = 0; i + 1 < npoints; i++)
        rip_ega_line(s, pts[i * 2], pts[i * 2 + 1],
                     pts[i * 2 + 2], pts[i * 2 + 3], color, thickness);
}

/* ------------------------------------------------------------------ */
/* Curves (Phase 2)                                                    */
/*                                                                     */
/* 1.54 says RIP_CIRCLE "understands aspect ratios and will draw a      */
/* truly circular circle", based on the EGA 640x350 resolution, but it  */
/* does not publish the constant. It follows from the geometry: 640x350 */
/* addressable pixels on a 4:3 display make each pixel taller than it   */
/* is wide, so a circle of one physical radius spans                    */
/*                                                                     */
/*     (350 * 4) / (640 * 3) = 1400 / 1920 = 0.7292                     */
/*                                                                     */
/* as many pixels vertically as horizontally. That ratio is applied to  */
/* the circle, arc and pie commands. The oval family passes its radii   */
/* through untouched, which is what the document requires.              */
/* ------------------------------------------------------------------ */

int rip_ega_aspect_y(int r)
{
    if (r <= 0) return 0;
    return (r * 1400 + 960) / 1920;
}

/* Segment count for a curve: roughly one segment every two pixels of
 * perimeter, bounded so tiny curves stay cheap and big ones stay smooth. */
static int arc_steps(int rx, int ry, double sweep_rad)
{
    int rmax = (rx > ry) ? rx : ry;
    int n;
    if (rmax < 1) rmax = 1;
    n = (int)((sweep_rad * rmax) / 2.0) + 4;
    if (n < 6) n = 6;
    if (n > 720) n = 720;
    return n;
}

/* Normalise a counterclockwise sweep from start to end, in radians. */
static void arc_range(int start_ang, int end_ang, double *a0, double *sweep)
{
    int s = start_ang % 360;
    int e = end_ang % 360;
    int span;
    if (s < 0) s += 360;
    if (e < 0) e += 360;
    span = e - s;
    if (span <= 0) span += 360;
    /* A full turn is expressed as 0 to 360, which normalises to 360. */
    if (start_ang != end_ang && span == 0) span = 360;
    *a0 = s * 3.14159265358979323846 / 180.0;
    *sweep = span * 3.14159265358979323846 / 180.0;
}

void rip_ega_arc(RipEga *s, int cx, int cy, int start_ang, int end_ang,
                 int rx, int ry, int color, int thickness)
{
    double a0, sweep;
    int i, n, px = 0, py = 0;

    if (!s || s->viewport_disabled) return;
    if (rx <= 0 && ry <= 0) return;
    if (start_ang == end_ang) return;      /* 1.54: nothing is drawn */

    arc_range(start_ang, end_ang, &a0, &sweep);
    n = arc_steps(rx, ry, sweep);

    for (i = 0; i <= n; i++) {
        double t = a0 + sweep * ((double)i / (double)n);
        /* Screen y grows downward, angles grow counterclockwise. */
        int x = cx + (int)(rx * cos(t) + (cos(t) >= 0 ? 0.5 : -0.5));
        int y = cy - (int)(ry * sin(t) + (sin(t) >= 0 ? 0.5 : -0.5));
        if (i > 0) rip_ega_line(s, px, py, x, y, color, thickness);
        px = x;
        py = y;
    }
}

void rip_ega_ellipse(RipEga *s, int cx, int cy, int rx, int ry,
                     int color, int thickness)
{
    rip_ega_arc(s, cx, cy, 0, 360, rx, ry, color, thickness);
}

void rip_ega_circle(RipEga *s, int cx, int cy, int radius,
                    int color, int thickness)
{
    rip_ega_arc(s, cx, cy, 0, 360, radius, rip_ega_aspect_y(radius),
                color, thickness);
}

void rip_ega_fill_ellipse(RipEga *s, int cx, int cy, int rx, int ry,
                          int pattern, int fill_color)
{
    int dy, c;
    if (!s || s->viewport_disabled) return;
    if (rx <= 0 || ry <= 0) return;
    c = ega_fill_index(pattern, fill_color);

    for (dy = -ry; dy <= ry; dy++) {
        double f = 1.0 - ((double)dy * dy) / ((double)ry * ry);
        int dx;
        if (f < 0.0) f = 0.0;
        dx = (int)(rx * sqrt(f) + 0.5);
        ega_fill_span(s, cy + dy, cx - dx, cx + dx, c);
    }
}

void rip_ega_pie(RipEga *s, int cx, int cy, int start_ang, int end_ang,
                 int rx, int ry, int pattern, int fill_color,
                 int color, int thickness)
{
    double a0, sweep;
    int i, n, *pts;

    if (!s || s->viewport_disabled) return;
    if (rx <= 0 && ry <= 0) return;
    if (start_ang == end_ang) return;

    arc_range(start_ang, end_ang, &a0, &sweep);
    n = arc_steps(rx, ry, sweep);

    /* Centre plus the arc samples, filled as one polygon. */
    pts = (int *)malloc(sizeof(int) * 2 * (size_t)(n + 2));
    if (!pts) return;
    pts[0] = cx;
    pts[1] = cy;
    for (i = 0; i <= n; i++) {
        double t = a0 + sweep * ((double)i / (double)n);
        pts[(i + 1) * 2]     = cx + (int)(rx * cos(t) + (cos(t) >= 0 ? 0.5 : -0.5));
        pts[(i + 1) * 2 + 1] = cy - (int)(ry * sin(t) + (sin(t) >= 0 ? 0.5 : -0.5));
    }
    rip_ega_fill_polygon(s, pts, n + 2, pattern, fill_color);

    /* Outline: the arc, then the two radii that converge on the centre. */
    rip_ega_arc(s, cx, cy, start_ang, end_ang, rx, ry, color, thickness);
    rip_ega_line(s, cx, cy, pts[2], pts[3], color, thickness);
    rip_ega_line(s, cx, cy, pts[(n + 1) * 2], pts[(n + 1) * 2 + 1],
                 color, thickness);
    free(pts);
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

void rip_ega_to_bgra(const RipEga *s, unsigned char *out)
{
    unsigned char r[16], g[16], b[16];
    int i;
    size_t n;

    if (!s || !out) return;
    for (i = 0; i < 16; i++)
        rip_ega_master_rgb(s->pal16[i], &r[i], &g[i], &b[i]);

    for (n = 0; n < (size_t)RIP_EGA_W * RIP_EGA_H; n++) {
        int c = s->pix[n] & 0x0F;
        out[n * 4 + 0] = b[c];
        out[n * 4 + 1] = g[c];
        out[n * 4 + 2] = r[c];
        out[n * 4 + 3] = 0xFF;
    }
}
