/*
 * rip_hershey.h - Hershey stroke glyph data for the RIPscrip stroked fonts.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The Hershey Fonts were originally created by Dr. A. V. Hershey while
 * working at the U. S. National Bureau of Standards. The format of the
 * font data in the distribution used here was originally created by
 * James Hurt, Cognition Inc. The fonts are public domain; see
 * THIRD-PARTY-NOTICES.md for the full acknowledgement.
 *
 * No Borland .CHR data and no TeleGrafix asset is used or redistributed.
 *
 * Glyph coordinates are in the native Hershey unit grid: x runs left to
 * right, y runs downward, and the baseline sits at y = +9. Every face in
 * this set has a capital height of exactly 21 units, which is what the
 * renderer scales against (see rip_text.c).
 */

#ifndef RIP_HERSHEY_H_INCLUDED
#define RIP_HERSHEY_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

/* Number of distinct Hershey faces bound below. */
#define RIP_HERSHEY_FACE_COUNT  9

/* Capital height of every face in this set, in Hershey units. Measured
 * from 'H' at generation time; all nine agree. */
#define RIP_HERSHEY_CAP_UNITS   21

/* Baseline position in the Hershey unit grid. */
#define RIP_HERSHEY_BASELINE    9

/* A point whose x is this value is a pen-up move, not a vertex. */
#define RIP_HERSHEY_PENUP       (-128)

typedef struct RipHersheyGlyph {
    signed char left;    /* left hand position  */
    signed char right;   /* right hand position; advance is right - left */
    short       first;   /* index of the first point in the face's pool */
    short       count;   /* number of points, pen-up markers included    */
} RipHersheyGlyph;

typedef struct RipHersheyFace {
    const char            *name;
    const RipHersheyGlyph *glyphs;   /* ASCII 0x20 through 0x7E */
    const signed char     *points;   /* interleaved x, y pairs   */
} RipHersheyFace;

extern const RipHersheyFace rip_hershey_faces[RIP_HERSHEY_FACE_COUNT];

/* RIP font number (1-10) to face index. Index 0 of this array is unused
 * because RIP font 0 is the bitmap font. */
extern const unsigned char rip_hershey_font_map[11];

#ifdef __cplusplus
}
#endif

#endif /* RIP_HERSHEY_H_INCLUDED */
