/*
 * minitel_decode.h - Minitel (Teletel videotex) decoder and page memory.
 *                    A shared Suite service: any module may drive it.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * ---------------------------------------------------------------------
 * PROVENANCE
 *
 * Every byte format decoded by this module was derived solely from the
 * published specification
 *
 *     "Specifications Techniques d'Utilisation du Minitel 1B" (STUM1B),
 *     France Telecom, part 2 chapter 2 (pages 84 to 104) and part 2
 *     chapter 6 (the Protocol syntax, pages 134 to 145).
 *     sha256 of the reference scan worked from:
 *     6d1250f7e721a3f43b847a58e897b008828bef98fb4db20441956017f478ef15
 *
 * The code tables (schemas 2.4 to 2.8, printed pages 92 and 100 to 103)
 * were read from the PAGE IMAGES of that scan, not from a transcription.
 * The prose was read from the volunteer transcription at
 * jbellue.github.io/stum1b as a reading aid, and every byte value this
 * module depends on was checked against the scan.
 *
 * The reference copy is held outside this repository and is NOT
 * redistributed with the Suite: it is a France Telecom document, held
 * for deriving an independent implementation exactly as the NAPLPS
 * standard is held for naplps_decode.
 *
 * No Minitel decoder, emulator or emitter source was read, copied or
 * adapted. In particular none of the projects on the estate's
 * DO-NOT-READ list (the Unicorn Minitel feasibility study, section 6.5)
 * was opened: not MiEdit or MiniPavi's web emulator, not eMinitel or
 * Minitel-core, not VDT2BMP, not Xtel, not any emitter library.
 *
 * eMinitel was ASSESSED FOR REUSE from its README and LICENSE only and
 * declined; the reasons are in docs/2026-09-27_SUITE_MINITEL_MODE.md
 * section 1.
 *
 * DISCLOSED CROSS-CHECKS, because a provenance claim that overstates its
 * own isolation is worse than one that describes it accurately:
 *
 *   - VDT2BMP 1.5.1.1 by Jean-Francois Del Nero, run as a BLACK-BOX
 *     release binary: the same frames in, BMPs out, compared cell by
 *     cell. Disagreements are settled by re-reading STUM1B, never by
 *     matching the other implementation.
 *   - MiniPavi's hosted web emulator and eMinitel 0.2.b were run as
 *     black-box oracles by earlier gates (screenshots and wire bytes
 *     only); their observed key maps are the source of the PC key
 *     bindings in the tab, which is a UI choice, not decoding.
 * ---------------------------------------------------------------------
 *
 * SCOPE. The Minitel 1B Teletel VIDEOTEX mode, 40 columns (STUM1B p13:
 * the power-on mode, and what every Minitel decodes). Implemented: C0,
 * the C1 attributes, US addressing with the row 00 rules, zone (serial)
 * attributes and delimiters, G1 mosaics joined and separated, G2 by SS2
 * with accent composition, sizes, REP, CAN, DEL, SUB, the CSI moves and
 * erasures of p94-95, the filters and resynchronisation of p99, screen
 * transparency, page and scroll mode, the Protocol sequences parsed and
 * swallowed, the cursor-position request answered. NOT implemented, by
 * mission scope: Mixte (80 columns), Teleinformatique, DRCS.
 *
 * This translation unit is deliberately free of Win32 and GDI calls.
 */

#ifndef MINITEL_DECODE_H_INCLUDED
#define MINITEL_DECODE_H_INCLUDED

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MINITEL_COLS    40
#define MINITEL_ROWS    25          /* rows 00..24; row 00 is the service row */
#define MINITEL_CELL_W  8           /* STUM1B p30: an 8 x 10 dot matrix      */
#define MINITEL_CELL_H  10
#define MINITEL_W       (MINITEL_COLS * MINITEL_CELL_W)   /* 320 */
#define MINITEL_H       (MINITEL_ROWS * MINITEL_CELL_H)   /* 250 */

/* Colour codes, in the ESC 0x40..0x47 order of schema 2.4. */
enum {
    MT_BLACK = 0, MT_RED, MT_GREEN, MT_YELLOW,
    MT_BLUE, MT_MAGENTA, MT_CYAN, MT_WHITE
};

/* What a page-memory cell holds. STUM1B p86 distinguishes exactly
 * three kinds: alphabetic characters, semi-graphic characters and zone
 * delimiters. */
enum {
    MT_KIND_ALPHA  = 0,
    MT_KIND_MOSAIC = 1,
    MT_KIND_DELIM  = 2
};

/* Cell flags. */
#define MT_F_FLASH      0x01    /* character level                       */
#define MT_F_INVERT     0x02    /* character level, alphabetic only      */
#define MT_F_SEPARATED  0x04    /* mosaic: lignage = disjoint (p91)      */
#define MT_F_ZMASK      0x08    /* delimiter: the zone it opens is masked */
#define MT_F_ZUNDER     0x10    /* delimiter: the zone it opens is lined  */

/* Size bits. A double-size character is written into 2 or 4 cells with
 * the same configuration (p93); the part bits say which quarter this
 * cell shows. */
#define MT_S_DW         0x01
#define MT_S_DH         0x02
#define MT_S_RIGHT      0x04    /* right half of a double width          */
#define MT_S_TOP        0x08    /* top half of a double height           */

/*
 * Glyph codes for alphabetic cells. 0x20..0x7E are G0 as drawn in
 * schema 2.5. Codes from 0x80 are the decoder's own names for G2
 * characters, composed accented letters and the two special blocks;
 * minitel_font.h maps them to bitmaps.
 */
enum {
    MT_G_FULLBLOCK = 0x80,   /* DEL in code (p98) and the error symbol (p99)  */
    MT_G_LOWBAR,             /* "je suis incapable de representer" (p90)      */
    MT_G_POUND, MT_G_SECTION, MT_G_ARROW_L, MT_G_ARROW_U, MT_G_ARROW_R,
    MT_G_ARROW_D, MT_G_DEGREE, MT_G_PLUSMINUS, MT_G_DIVIDE,
    MT_G_QUARTER, MT_G_HALF, MT_G_3QUARTER,
    MT_G_OE_UC, MT_G_OE_LC, MT_G_BETA,
    MT_G_A_GRAVE, MT_G_E_GRAVE, MT_G_U_GRAVE, MT_G_E_ACUTE,
    MT_G_A_CIRC, MT_G_E_CIRC, MT_G_I_CIRC, MT_G_O_CIRC, MT_G_U_CIRC,
    MT_G_A_DIAER, MT_G_E_DIAER, MT_G_I_DIAER, MT_G_O_DIAER, MT_G_U_DIAER,
    MT_G_C_CEDIL,
    MT_G_END
};

typedef struct minitel_cell {
    unsigned char kind;     /* MT_KIND_*                                 */
    unsigned char glyph;    /* alpha: glyph code; mosaic: the 6 bits     */
    unsigned char fg;       /* character colour                          */
    unsigned char bg;       /* mosaic: its own paper; delimiter: the
                               paper of the zone it opens. Alphabetic
                               cells take the zone's paper at render
                               time, because that is how serial
                               attributes work (p94).                    */
    unsigned char flags;    /* MT_F_*                                    */
    unsigned char size;     /* MT_S_*                                    */
} minitel_cell_t;

/*
 * Counters, under the counter-honesty rule the NAPLPS and RIPscrip tabs
 * follow: what the decoder declines to act on is counted, so a sparse
 * page and a mis-decoded one can be told apart without a log.
 */
typedef struct minitel_stats {
    unsigned long bytes_in;
    unsigned long chars;          /* cells written by printable codes   */
    unsigned long filtered;       /* sequences consumed without action  */
    unsigned long resync;         /* sequences cut by a C0 (p99)        */
    unsigned long errors;         /* error symbols shown (SUB)          */
    unsigned long row0;           /* characters written on row 00       */
    unsigned long protocol;       /* PRO1/2/3 sequences swallowed       */
    unsigned long unrepresent;    /* SS2 codes outside the repertoire   */
    unsigned long bit8;           /* bytes arriving with bit 8 set      */
    unsigned long bells;          /* BEL received (counted, not sounded) */
    unsigned long us_ignored;     /* US pairs that address nothing      */
} minitel_stats_t;

typedef struct minitel_decoder minitel_decoder_t;

/* Bytes the TERMINAL must send back up the line: the answer to the
 * cursor position request ESC 0x61 (p98) and to PRO1 ENQROM (p139). The
 * decoder never sends anything itself; a caller that has a line installs
 * this, and a caller that has none (a file, the harness) does not. */
typedef void (*minitel_reply_fn)(void *ctx, const unsigned char *buf,
                                 size_t len);

minitel_decoder_t *minitel_create(void);
void               minitel_destroy(minitel_decoder_t *d);

/* Power-on state (p87-88): page memory filled with black mosaics, cursor
 * row 01 column 01 and not shown, page mode, screen masking enabled,
 * white on black, G0. Counters zeroed. */
void minitel_reset(minitel_decoder_t *d);

void minitel_feed(minitel_decoder_t *d, const unsigned char *buf,
                  size_t len);

void minitel_set_reply(minitel_decoder_t *d, minitel_reply_fn fn,
                       void *ctx);

/* The terminal's own state letter in row 00 column 39 (p32): 'F' local,
 * 'C' connected, 0 for none. Drawn black on white by the renderer; it is
 * the terminal's indicator, not a byte from the line. */
void minitel_set_status_letter(minitel_decoder_t *d, char letter);
char minitel_get_status_letter(const minitel_decoder_t *d);

const minitel_cell_t *minitel_cell(const minitel_decoder_t *d,
                                   int row, int col);   /* row 0..24, col 1..40 */
const minitel_stats_t *minitel_get_stats(const minitel_decoder_t *d);

int  minitel_cursor_row(const minitel_decoder_t *d);
int  minitel_cursor_col(const minitel_decoder_t *d);
int  minitel_cursor_visible(const minitel_decoder_t *d);
int  minitel_screen_masked(const minitel_decoder_t *d);  /* p85 full-screen mask */
int  minitel_scroll_mode(const minitel_decoder_t *d);
int  minitel_ever_drew(const minitel_decoder_t *d);

#ifdef __cplusplus
}
#endif

#endif /* MINITEL_DECODE_H_INCLUDED */
