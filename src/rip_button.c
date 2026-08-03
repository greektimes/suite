/*
 * rip_button.c - RIP_BUTTON_STYLE state and button chrome drawing.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from docs/ripscrip-ref/RIPSCRIP-1.54.DOC. No Win32, no GDI.
 *
 * WHAT IS IMPLEMENTED, AND WHY THAT SET.
 *
 * The service emits exactly three button styles, and between them they
 * use Plain, Mouse, Bevel, Invertable, Dropshadow, Underline hot-key and
 * Highlight hot-key. Nothing else. So this draws:
 *
 *   - the plain button face, filled in <surface>
 *   - the bevel, <bevsize> pixels thick outside the base image, lit from
 *     the top left with <bright> and shaded with <dark>, corners in
 *     <corner_col>
 *   - the label, centred per <orient>, in <dfore>, with the <dback>
 *     dropshadow drawn first one pixel down and right
 *   - the hot-key character highlighted in <uline_col> and underlined in
 *     the same colour, when those flags are set
 *
 * Icon buttons and Clipboard buttons need artwork the renderer does not
 * have yet. Rather than approximate them, rip_button_style_drawable()
 * returns false and the parser counts the button as undrawn, so the
 * deferred tally stays honest.
 *
 * Chisel, Recess, Sunken, Explode, radio and check-box behaviour and the
 * justify flags are parsed into the style but not drawn; the service
 * does not use any of them.
 */

#include "rip_button.h"
#include "rip_text.h"

#include <string.h>

void rip_button_style_default(RipButtonStyle *st)
{
    if (!st) return;
    memset(st, 0, sizeof *st);
    /* Before any RIP_BUTTON_STYLE arrives there is no button definition.
     * A zero style is not drawable, which is the safe default. */
    st->orient    = RIP_BORIENT_CENTER;
    st->surface   = 7;
    st->bright    = 15;
    st->dark      = 8;
    st->dfore     = 0;
    st->dback     = 15;
    st->uline_col = 4;
}

int rip_button_style_drawable(const RipButtonStyle *st)
{
    if (!st) return 0;
    if (st->flags & (RIP_BF_ICON | RIP_BF_CLIPBOARD)) return 0;
    return (st->flags & RIP_BF_PLAIN) ? 1 : 0;
}

/* Normalise the command's corners and apply a fixed size from the style.
 * A fixed size in the style wins over the command's rectangle; a zero
 * size means the button is dynamically sized and the command's own
 * corners are the base image. Shared by rip_button_draw and
 * rip_button_image_rect so the drawn image and the inverted image are
 * the same rectangle by construction. */
static void base_rect(const RipButtonStyle *st,
                      int *x0, int *y0, int *x1, int *y1)
{
    int t;
    if (*x0 > *x1) { t = *x0; *x0 = *x1; *x1 = t; }
    if (*y0 > *y1) { t = *y0; *y0 = *y1; *y1 = t; }
    if (st && st->wid > 0 && st->hgt > 0) {
        *x1 = *x0 + st->wid - 1;
        *y1 = *y0 + st->hgt - 1;
    }
}

void rip_button_image_rect(const RipButtonStyle *st,
                           int x0, int y0, int x1, int y1,
                           int *ix0, int *iy0, int *ix1, int *iy1)
{
    int grow = 0;
    base_rect(st, &x0, &y0, &x1, &y1);
    if (st && (st->flags & RIP_BF_BEVEL)) grow = st->bevsize;
    *ix0 = x0 - grow;
    *iy0 = y0 - grow;
    *ix1 = x1 + grow;
    *iy1 = y1 + grow;
}

void rip_button_outer_rect(const RipButtonStyle *st,
                           int x0, int y0, int x1, int y1,
                           int *ox0, int *oy0, int *ox1, int *oy1)
{
    int grow = 0;
    if (st) {
        /* 1.54 lists the modifiers RIP_BUTTON applies per effect: bevel
         * grows the box by its size on each side, recess by two, and
         * sunken and chisel do not change it. */
        if (st->flags & RIP_BF_BEVEL) grow += st->bevsize;
        if (st->flags & RIP_BF_RECESSED) grow += 2;
    }
    base_rect(st, &x0, &y0, &x1, &y1);
    *ox0 = x0 - grow;
    *oy0 = y0 - grow;
    *ox1 = x1 + grow;
    *oy1 = y1 + grow;
}

/* One ring of the bevel: lit along the top and left, shadowed along the
 * bottom and right, with the two transitional corners in corner_col. */
static void bevel_ring(RipEga *s, int x0, int y0, int x1, int y1,
                       int bright, int dark, int corner)
{
    int x, y;
    for (x = x0; x <= x1; x++) {
        rip_ega_pixel(s, x, y0, bright);
        rip_ega_pixel(s, x, y1, dark);
    }
    for (y = y0; y <= y1; y++) {
        rip_ega_pixel(s, x0, y, bright);
        rip_ega_pixel(s, x1, y, dark);
    }
    rip_ega_pixel(s, x1, y0, corner);
    rip_ega_pixel(s, x0, y1, corner);
}

/* Index of the first occurrence of the hot-key character in the label,
 * case-insensitive for letters, or -1. */
static int hotkey_index(const char *label, int hotkey)
{
    int i;
    int hk = hotkey;
    if (!label || hotkey <= 0) return -1;
    if (hk >= 'a' && hk <= 'z') hk -= 32;
    for (i = 0; label[i]; i++) {
        int c = (unsigned char)label[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == hk) return i;
    }
    return -1;
}

int rip_button_draw(RipEga *s, const RipButtonStyle *st,
                    int x0, int y0, int x1, int y1,
                    const char *label, int hotkey,
                    int font, int size)
{
    int bx0, by0, bx1, by1;
    int i, w, h;

    if (!s || !st) return 0;
    if (!rip_button_style_drawable(st)) return 0;

    base_rect(st, &x0, &y0, &x1, &y1);

    /* Base image: a solid rectangle in the surface colour. */
    rip_ega_bar(s, x0, y0, x1, y1, 1 /* solid */, st->surface);

    /* Bevel, drawn outside the base image, outermost ring first. */
    if ((st->flags & RIP_BF_BEVEL) && st->bevsize > 0) {
        for (i = st->bevsize; i >= 1; i--) {
            bx0 = x0 - i; by0 = y0 - i;
            bx1 = x1 + i; by1 = y1 + i;
            bevel_ring(s, bx0, by0, bx1, by1,
                       st->bright, st->dark, st->corner_col);
        }
    }

    /* Label. 1.54 says RIP_BUTTON uses the current font sizes, so the
     * caller passes the live RIP font state rather than a fixed face. */
    if (label && label[0]) {
        int tw = rip_text_width(font, size, label);
        int ch = rip_text_cell_height(font, size);
        int tx, ty, hk;

        w = x1 - x0 + 1;
        h = y1 - y0 + 1;

        switch (st->orient) {
        case RIP_BORIENT_ABOVE: tx = x0 + (w - tw) / 2; ty = y0 - ch;      break;
        case RIP_BORIENT_BELOW: tx = x0 + (w - tw) / 2; ty = y1 + 1;       break;
        case RIP_BORIENT_LEFT:  tx = x0 - tw - 1;       ty = y0 + (h - ch) / 2; break;
        case RIP_BORIENT_RIGHT: tx = x1 + 2;            ty = y0 + (h - ch) / 2; break;
        default:                tx = x0 + (w - tw) / 2; ty = y0 + (h - ch + 1) / 2; break;
        }
        /* The justify flags only move TOP, CENTER and BOTTOM labels. */
        if (st->orient == RIP_BORIENT_ABOVE ||
            st->orient == RIP_BORIENT_CENTER ||
            st->orient == RIP_BORIENT_BELOW) {
            int indent = (st->flags & RIP_BF_CHISEL) ? 20 : 10;
            if (st->flags2 & RIP_BF2_LEFTJUST)  tx = x0 + indent;
            if (st->flags2 & RIP_BF2_RIGHTJUST) tx = x1 - indent - tw;
        }

        /* Dropshadow first, one pixel down and right, then the label. */
        if (st->flags & RIP_BF_DROPSHADOW)
            rip_text_draw(s, font, RIP_DIR_HORIZ, size,
                          tx + 1, ty + 1, label, st->dback);

        rip_text_draw(s, font, RIP_DIR_HORIZ, size, tx, ty, label, st->dfore);

        hk = hotkey_index(label, hotkey);
        if (hk >= 0) {
            /* Width of the label up to the hot-key, so the character can
             * be found without assuming a fixed advance. */
            char head[256];
            char one[2];
            int off, cw;

            if (hk > (int)sizeof head - 1) hk = (int)sizeof head - 1;
            memcpy(head, label, (size_t)hk);
            head[hk] = '\0';
            off = rip_text_width(font, size, head);
            one[0] = label[hk];
            one[1] = '\0';
            cw = rip_text_width(font, size, one);

            if (st->flags2 & RIP_BF2_HILIGHT)
                rip_text_draw(s, font, RIP_DIR_HORIZ, size,
                              tx + off, ty, one, st->uline_col);

            if (st->flags & RIP_BF_UNDERLINE) {
                int uy = ty + rip_text_underline_offset(font, size);
                int ux;
                for (ux = tx + off; ux < tx + off + cw; ux++)
                    rip_ega_pixel(s, ux, uy, st->uline_col);
            }
        }
    }
    return 1;
}
