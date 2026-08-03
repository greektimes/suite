/*
 * rip_ega.h - 640x350 16-colour indexed EGA surface for the RIPscrip
 *             renderer, plus the drawing primitives the parser drives.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from the published RIPscrip 1.54 specification
 * (docs/ripscrip-ref/RIPSCRIP-1.54.DOC). RIPscrip is a trademark of
 * TeleGrafix Communications, Inc.
 *
 * This translation unit is deliberately free of Win32 and GDI calls: it
 * owns nothing but an indexed pixel buffer, a 16-entry palette mapping
 * into the 64-colour EGA master palette, and the raster primitives. The
 * module (ripscrip_module.c) turns the surface into a DIB and blits it;
 * the offline harness tool (tools/rip_render.c) turns the same surface
 * into a BMP. Both consume rip_ega_to_bgra().
 */

#ifndef RIP_EGA_H_INCLUDED
#define RIP_EGA_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#define RIP_EGA_W  640
#define RIP_EGA_H  350

/* Write modes per RIP_WRITE_MODE. 00 is normal copy, 01 is XOR. */
#define RIP_WMODE_COPY  0
#define RIP_WMODE_XOR   1

typedef struct RipEga {
    unsigned char pix[RIP_EGA_W * RIP_EGA_H];  /* palette indices 0-15 */

    /* pal16[i] is the 64-colour master palette entry currently bound to
     * RIP colour i. RIP_SET_PALETTE / RIP_ONE_PALETTE rewrite this; the
     * pixel buffer is untouched, so a palette change recolours what is
     * already on screen exactly as EGA hardware did. */
    unsigned char pal16[16];

    /* Graphics viewport (inclusive). Everything clips to this. When
     * disabled (all four args zero) drawing commands are ignored. */
    int  vx0, vy0, vx1, vy1;
    int  viewport_disabled;

    int  write_mode;      /* RIP_WMODE_* */
} RipEga;

/* Reset to the RIP_RESET_WINDOWS state: default 16-colour palette,
 * full-screen viewport, copy write mode, screen cleared to colour 0. */
void rip_ega_reset(RipEga *s);

/* Clear the viewport to the current background colour (index 0). */
void rip_ega_erase_view(RipEga *s);

/* Palette. `master` values are 0-63; out-of-range values are ignored. */
void rip_ega_set_palette16(RipEga *s, const unsigned char master[16]);
void rip_ega_set_one_palette(RipEga *s, int color, int master);

/* Viewport. All-zero arguments disable the graphics window. */
void rip_ega_set_viewport(RipEga *s, int x0, int y0, int x1, int y1);

/* Primitives. `color` is a RIP colour index 0-15. All clip to the
 * viewport and honour the current write mode unless noted. */
void rip_ega_pixel(RipEga *s, int x, int y, int color);
void rip_ega_line(RipEga *s, int x0, int y0, int x1, int y1,
                  int color, int thickness);
void rip_ega_rect(RipEga *s, int x0, int y0, int x1, int y1,
                  int color, int thickness);

/* Filled rectangle (RIP_BAR). No border. Uses the fill pattern/colour,
 * which the caller supplies: pattern 00 paints in colour 0 (background),
 * every other pattern paints in `fill_color`. Patterns 02-0B are not yet
 * hatched; see rip_parser's unsupported counters. Fills ignore write
 * mode per the specification. */
void rip_ega_bar(RipEga *s, int x0, int y0, int x1, int y1,
                 int pattern, int fill_color);

/* Filled polygon, even-odd rule (RIP_FILL_POLYGON's interior). `pts` is
 * npoints (x,y) pairs. The caller draws the outline separately. */
void rip_ega_fill_polygon(RipEga *s, const int *pts, int npoints,
                          int pattern, int fill_color);

/* Polygon outline, closed (RIP_POLYGON). */
void rip_ega_polygon(RipEga *s, const int *pts, int npoints,
                     int color, int thickness);

/* Open polyline (RIP_POLYLINE, new in 1.54). */
void rip_ega_polyline(RipEga *s, const int *pts, int npoints,
                      int color, int thickness);

/* ---- curves (Phase 2) --------------------------------------------
 * Angles are in degrees, zero at 3 o'clock, increasing counterclockwise,
 * exactly as 1.54 draws it. Arcs run counterclockwise from start to end;
 * equal angles draw nothing.
 *
 * RIP_CIRCLE, RIP_ARC and RIP_PIE_SLICE are aspect corrected so a circle
 * comes out truly circular on a 640x350 EGA screen shown at 4:3; the
 * oval family is not, because an ellipse's radii are already explicit.
 * See rip_ega_aspect_y() for how the correction is derived. */

/* Vertical radius that makes a horizontal radius `r` look circular. */
int  rip_ega_aspect_y(int r);

void rip_ega_circle(RipEga *s, int cx, int cy, int radius,
                    int color, int thickness);
void rip_ega_ellipse(RipEga *s, int cx, int cy, int rx, int ry,
                     int color, int thickness);
void rip_ega_fill_ellipse(RipEga *s, int cx, int cy, int rx, int ry,
                          int pattern, int fill_color);
void rip_ega_arc(RipEga *s, int cx, int cy, int start_ang, int end_ang,
                 int rx, int ry, int color, int thickness);
/* Pie slice: fills the interior, then draws the arc and the two radii. */
void rip_ega_pie(RipEga *s, int cx, int cy, int start_ang, int end_ang,
                 int rx, int ry, int pattern, int fill_color,
                 int color, int thickness);

/* Invert every pixel in a rectangle, inclusive of both corners: each
 * palette index becomes its four-bit complement, so black and white
 * swap and the surface colour 7 swaps with the shadow colour 8. This is
 * the same complement 1.54 describes for XOR write mode, applied to a
 * region rather than to a primitive.
 *
 * Deliberately IGNORES the viewport and the write mode. 1.54 stores
 * mouse regions in global screen coordinates precisely so that a region
 * inverts at the right place whatever viewport happens to be current,
 * and press feedback is the terminal's own drawing, not the host's.
 * Clipped to the screen only. Applying it twice restores the region
 * exactly, which is how the pressed look is taken back off. */
void rip_ega_invert_rect(RipEga *s, int x0, int y0, int x1, int y1);

/* Resolve one master palette value (0-63) to 8-bit RGB. Exposed so the
 * harness can report colours. */
void rip_ega_master_rgb(int master, unsigned char *r,
                        unsigned char *g, unsigned char *b);

/* Expand the surface into a top-down 32-bit BGRA buffer of
 * RIP_EGA_W * RIP_EGA_H pixels (4 bytes each). `out` must hold
 * RIP_EGA_W * RIP_EGA_H * 4 bytes. Alpha is set to 0xFF. */
void rip_ega_to_bgra(const RipEga *s, unsigned char *out);

#ifdef __cplusplus
}
#endif

#endif /* RIP_EGA_H_INCLUDED */
