/*
 * naplps_surface.h - 256x200 indexed videotex surface for the NAPLPS
 *                    renderer, plus the raster primitives the decoder
 *                    drives.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from ANSI X3.110-1983 / CSA T500-1983, Videotex/Teletext
 * Presentation Level Protocol Syntax (North American PLPS), December
 * 1983. No NAPLPS decoder or renderer source was read, copied or
 * adapted. See naplps_decode.h for the full provenance statement.
 *
 * This translation unit is deliberately free of Win32 and GDI calls: it
 * owns nothing but an indexed pixel buffer, a 16-entry colour map in the
 * standard's GRB system, and the raster primitives. The module
 * (naplps_module.c) turns the surface into a DIB and blits it; the
 * offline harness (tools/naplps_selftest.c) turns the same surface into
 * a BMP. Both consume naplps_surface_to_bgra().
 *
 * THE Y AXIS. The standard puts the origin at the LOWER LEFT with Y
 * increasing UPWARD (4.2.2). This surface keeps that convention, so the
 * geometry code reads the way the standard reads. Row 0 of pix[] is the
 * BOTTOM scan line. The flip to a top-down display buffer happens in
 * exactly one place, naplps_surface_to_bgra(), and nowhere else. Do not
 * "fix" a coordinate here to make a picture look right; the bug will be
 * somewhere else.
 */

#ifndef NAPLPS_SURFACE_H_INCLUDED
#define NAPLPS_SURFACE_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The videotex Service Reference Model raster, Appendix D Table D1:
 * "Resolution shall be on the order of 256 pixels horizontal by 200
 * pixels vertical. The full X dimension (width) of the unit screen, and
 * the Y dimension (height) from 0.0 to 0.78125, shall be visible in the
 * display area."
 *
 * 200 / 256 is exactly 0.78125, so one pixel is 1/256 of the unit screen
 * on BOTH axes: unit-screen pixels are square here. That is what makes
 * the integer-scale presentation in naplps_module.c correct.
 */
#define NAPLPS_W            256
#define NAPLPS_H            200

/* Unit-screen coordinate 1.0 spans this many pixels on either axis. */
#define NAPLPS_UNIT         256

/*
 * Colour map size. The standard leaves N (address bits) and M (colour
 * value bits) to the implementation and recommends M >= 3(N-1). The
 * worked default map of Figure 25 is given for N = 4, M = 9, which is
 * the videotex profile this renderer targets: 16 entries, 3 bits per
 * primary.
 */
#define NAPLPS_CMAP_N       4
#define NAPLPS_CMAP_SIZE    (1 << NAPLPS_CMAP_N)
#define NAPLPS_PRIMARY_MAX  7      /* 3 bits per primary */

/*
 * One colour map entry, in the standard's own primary order: green,
 * red, blue, "which is the order of decreasing luminance" (5.3.1.1,
 * Figure 23). Each component is 0..7.
 */
typedef struct naplps_color {
    unsigned char g;
    unsigned char r;
    unsigned char b;
} naplps_color_t;

/*
 * An area texture mask. Fills consult this to decide whether a pixel is
 * laid down. A solid fill is the all-ones mask. 16x16 is the
 * programmable texture mask size the videotex SRM requires ("at least
 * 32 bytes per programmable texture mask, organised 16 x 16").
 */
#define NAPLPS_MASK_DIM     16

/*
 * What a mask is. The predefined hatches are NOT 16x16 bitmaps: 5.3.2.4.4
 * ties their line width and spacing to the logical pel, which can be any
 * size, so they are tested arithmetically in surface coordinates. That
 * also gives the registration the standard asks for ("Registration of the
 * patterns shall be maintained across figures if the logical pel size is
 * the same") for free.
 */
#define NAPLPS_MASK_BITMAP  0   /* row[] below                              */
#define NAPLPS_MASK_VHATCH  1   /* Table 13 pattern 001                     */
#define NAPLPS_MASK_HHATCH  2   /* Table 13 pattern 010                     */
#define NAPLPS_MASK_XHATCH  3   /* Table 13 pattern 011                     */
#define NAPLPS_MASK_NULL    4   /* masks A-D in their default, null state   */

typedef struct naplps_mask {
    unsigned short row[NAPLPS_MASK_DIM];   /* bit 0 is the leftmost column */
    int kind;       /* NAPLPS_MASK_*                                        */
    int pw, ph;     /* hatch line width and height in pixels, at least 1    */
    int off_addr;   /* colour mode 2: address painted where the mask is off;
                       -1 leaves those pixels alone (colour modes 0 and 1)  */
} naplps_mask_t;

typedef struct naplps_surface {
    unsigned char  pix[NAPLPS_W * NAPLPS_H]; /* colour map addresses, row 0 = BOTTOM */
    naplps_color_t cmap[NAPLPS_CMAP_SIZE];
} naplps_surface_t;

/*
 * Reset the surface: clear to colour map address 0 (nominal black) and
 * load the default colour map of 5.3.2.5.2.
 */
void naplps_surface_init(naplps_surface_t *s);

/*
 * Load the default colour map only, leaving the pixels alone. The first
 * half is a uniformly spaced grey ramp (G = R = B = address); the second
 * half is eight hues equally spaced counterclockwise around the hue
 * circle from blue at 0 degrees. Matches Figure 25 for N = 4, M = 9.
 */
void naplps_surface_default_cmap(naplps_surface_t *s);

/* Clear the whole display area to one colour map address. */
void naplps_surface_clear(naplps_surface_t *s, unsigned char addr);

/* The all-ones (solid) area texture mask. */
void naplps_mask_solid(naplps_mask_t *m);

/*
 * The area texture pattern of Table 13, 5.3.2.4.4, for a TEXTURE pattern
 * number 0..7 (bits b6 b5 b4) and the current logical pel in pixels:
 * 0 solid, 1 vertical hatching, 2 horizontal hatching, 3 cross-hatching,
 * 4..7 programmable masks A-D. The standard FIXES these, and its SRM
 * requires all four predefined ones to be presented; they are not a
 * rendering choice. off_addr is the colour mode 2 background, or -1.
 */
void naplps_mask_pattern(naplps_mask_t *m, int pattern, int pw, int ph,
                         int off_addr);

/*
 * Raster primitives. All coordinates are PIXEL coordinates in the
 * surface's bottom-left-origin space. Everything clips to the display
 * area; nothing here can write outside pix[].
 *
 * "pel" is the logical pel of 5.3.2.2.6, the rectangular brush that
 * gives stroke width. pw and ph are its width and height in pixels; a
 * 0x0 pel is the dimensionless drawing point and still marks one pixel.
 */
void naplps_plot(naplps_surface_t *s, int x, int y, unsigned char addr);

void naplps_draw_point(naplps_surface_t *s, int x, int y,
                       int pw, int ph, unsigned char addr);

void naplps_draw_line(naplps_surface_t *s, int x0, int y0, int x1, int y1,
                      int pw, int ph, unsigned char addr,
                      unsigned short line_texture);

void naplps_fill_rect(naplps_surface_t *s, int x, int y, int w, int h,
                      unsigned char addr, const naplps_mask_t *mask);

/*
 * Scanline polygon fill, even-odd rule. pts is 2*n integers, x then y.
 * The caller supplies the closed vertex ring; closure is implicit.
 */
void naplps_fill_poly(naplps_surface_t *s, const int *pts, int n,
                      unsigned char addr, const naplps_mask_t *mask);

void naplps_stroke_poly(naplps_surface_t *s, const int *pts, int n,
                        int closed, int pw, int ph, unsigned char addr,
                        unsigned short line_texture);

/*
 * Turn the indexed surface into 32bpp BGRA for a top-down DIB. out must
 * hold NAPLPS_W * NAPLPS_H * 4 bytes.
 *
 * THIS IS THE ONLY PLACE THE Y AXIS FLIPS. Surface row 0 is the bottom
 * scan line; DIB row 0 is the top.
 *
 * mono != 0 renders every colour by its computed luminance instead of
 * its hue, which is what a monochrome Alextel would show. The grey half
 * of the default colour map survives this unchanged; the hue half does
 * not, and seeing that is the point of the toggle.
 */
void naplps_surface_to_bgra(const naplps_surface_t *s, unsigned char *out,
                            int mono);

/* Luminance 0..255 of a colour map entry, in the standard's GRB order. */
int naplps_color_luma(const naplps_color_t *c);

#ifdef __cplusplus
}
#endif

#endif /* NAPLPS_SURFACE_H_INCLUDED */
