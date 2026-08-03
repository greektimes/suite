/*
 * rip_button.h - RIP_BUTTON_STYLE state and button chrome drawing.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from docs/ripscrip-ref/RIPSCRIP-1.54.DOC. No Win32, no GDI.
 *
 * 1.54 calls RIP_BUTTON_STYLE "probably one of the most complex in the
 * entire protocol". This implements the parts the service actually uses
 * and reports the rest rather than pretending: see rip_button_draw.
 */

#ifndef RIP_BUTTON_H_INCLUDED
#define RIP_BUTTON_H_INCLUDED

#include "rip_ega.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RIP_BUTTON_STYLE <flags> bits (table "Flags Field #1"). */
#define RIP_BF_CLIPBOARD    1
#define RIP_BF_INVERTABLE   2
#define RIP_BF_RESET_AFTER  4
#define RIP_BF_CHISEL       8
#define RIP_BF_RECESSED     16
#define RIP_BF_DROPSHADOW   32
#define RIP_BF_AUTOSTAMP    64
#define RIP_BF_ICON         128
#define RIP_BF_PLAIN        256
#define RIP_BF_BEVEL        512
#define RIP_BF_MOUSE        1024
#define RIP_BF_UNDERLINE    2048
#define RIP_BF_HOTICONS     4096
#define RIP_BF_ADJVCENTER   8192
#define RIP_BF_RADIO        16384
#define RIP_BF_SUNKEN       32768

/* RIP_BUTTON_STYLE <flags2> bits (table "Flags Field #2"). */
#define RIP_BF2_CHECKBOX    1
#define RIP_BF2_HILIGHT     2
#define RIP_BF2_EXPLODE     4
#define RIP_BF2_LEFTJUST    8
#define RIP_BF2_RIGHTJUST   16

/* <orient> values: where the label sits relative to the button. */
#define RIP_BORIENT_ABOVE   0
#define RIP_BORIENT_LEFT    1
#define RIP_BORIENT_CENTER  2
#define RIP_BORIENT_RIGHT   3
#define RIP_BORIENT_BELOW   4

typedef struct RipButtonStyle {
    int wid, hgt;        /* fixed size, or 0 0 for dynamically sized     */
    int orient;          /* RIP_BORIENT_*                                */
    int flags;           /* RIP_BF_*                                     */
    int bevsize;         /* bevel thickness, used when RIP_BF_BEVEL      */
    int dfore, dback;    /* label colour, dropshadow colour              */
    int bright, dark;    /* highlight and shadow for the special effects */
    int surface;         /* plain button face colour                     */
    int grp_no;
    int flags2;          /* RIP_BF2_*                                    */
    int uline_col;       /* underline / hotkey highlight colour          */
    int corner_col;
} RipButtonStyle;

/* Reset to the state before any RIP_BUTTON_STYLE has been received. */
void rip_button_style_default(RipButtonStyle *st);

/* True if this style describes a button whose image we can construct.
 * Icon and Clipboard buttons need artwork we do not have yet, so they
 * are reported as undrawn rather than approximated. */
int  rip_button_style_drawable(const RipButtonStyle *st);

/* Apply the special-effect modifiers 1.54 lists for RIP_BUTTON so the
 * caller gets the button's outermost rectangle: bevel grows the box by
 * bevsize on every side, recess by two. Used for drawing AND for
 * hit-testing, so a click on the bevel counts as a click on the button. */
void rip_button_outer_rect(const RipButtonStyle *st,
                           int x0, int y0, int x1, int y1,
                           int *ox0, int *oy0, int *ox1, int *oy1);

/* The rectangle rip_button_draw actually paints as the button's IMAGE:
 * the base rectangle after any fixed size in the style, grown by the
 * bevel. This is what inverts when an Invertable button is pressed.
 *
 * It is deliberately not the same as rip_button_outer_rect. 1.54 says
 * the special effects invert along with the button because they are
 * part of its image, "all except for the Recessed effect", which "is
 * NEVER considered part of the actual button image ... it is just extra
 * graphics". The outer rectangle includes the recess because a click on
 * it still counts; the image rectangle does not because it must not
 * invert. */
void rip_button_image_rect(const RipButtonStyle *st,
                           int x0, int y0, int x1, int y1,
                           int *ix0, int *iy0, int *ix1, int *iy1);

/* Draw one button. (x0,y0)-(x1,y1) is the base image rectangle exactly
 * as it arrived in the RIP_BUTTON command. `label` may be empty.
 * `hotkey` is the ASCII code from the command, 0 for none. `font` and
 * `size` are the current RIP font state, which 1.54 says the label
 * uses. Returns non-zero if anything was drawn. */
int  rip_button_draw(RipEga *s, const RipButtonStyle *st,
                     int x0, int y0, int x1, int y1,
                     const char *label, int hotkey,
                     int font, int size);

#ifdef __cplusplus
}
#endif

#endif /* RIP_BUTTON_H_INCLUDED */
