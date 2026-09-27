/*
 * naplps_font.h - 5x7 glyphs for the NAPLPS primary character set.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * WHY 5x7 AND NOT THE SUITE'S EXISTING 8x8 FONT. The standard says the
 * glyph shapes are ours to choose: "The particular patterns (font) chosen
 * for the characters are implementation-dependent" (5.1). What is NOT
 * ours to choose is the character field, and NORMAL TEXT is dx = 1/40 by
 * dy = 5/128 (6.2.7.8), which on the videotex SRM's 256x200 raster is a
 * cell only 6.4 by 10 pixels. The Suite's rip_font8x8 glyphs are up to
 * seven columns wide and cannot be squeezed into six without losing
 * strokes from M, W and friends. A 5x7 glyph in a 6x10 cell is what
 * period videotex and teletext hardware actually used, and it is legible
 * at this size, which the alternative is not.
 *
 * The glyph shapes are ORIGINAL WORK for this project, drawn as ASCII art
 * in tools/gen_naplps_font.py, which generates naplps_font_data.c. No
 * font file or font package was copied.
 *
 * Seven bytes per glyph, one per row, TOP row first, BIT 0 LEFTMOST,
 * matching the convention rip_font8x8.h documents. Only bits 0 to 4 are
 * ever set.
 *
 * Index 0 is ASCII 0x20 (SPACE); index 94 is ASCII 0x7E (tilde). Those
 * are exactly the 94 graphic characters of the primary set (5.1, "This
 * set is identical to the graphic character set of ANSI X3.4-1977") plus
 * SPACE.
 */

#ifndef NAPLPS_FONT_H_INCLUDED
#define NAPLPS_FONT_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#define NAPLPS_GLYPH_W      5
#define NAPLPS_GLYPH_H      7
#define NAPLPS_GLYPH_FIRST  0x20
#define NAPLPS_GLYPH_COUNT  95

extern const unsigned char naplps_font5x7[NAPLPS_GLYPH_COUNT][NAPLPS_GLYPH_H];

#ifdef __cplusplus
}
#endif

#endif /* NAPLPS_FONT_H_INCLUDED */
