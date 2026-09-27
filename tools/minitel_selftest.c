/*
 * minitel_selftest.c - offline gate harness for the Minitel decoder.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The counterpart of naplps_selftest.c: it drives the same pure decoder
 * and renderer the Videotex tab drives, with no Win32 involved.
 *
 *   minitel_selftest                      run the G1 self-checks
 *   minitel_selftest FILE.vdt OUT.bmp [--mono] [--status C]
 *                                         decode a file, write a BMP,
 *                                         print the counters
 *   minitel_selftest --dump FILE.vdt      print the page memory, one
 *                                         line per cell, for the G2
 *                                         cell-by-cell comparison
 *   minitel_selftest --present IN.bmp OUT.bmp [--scale N] [--scanlines]
 *                    [--mono]             present a 320 x 250 raster at
 *                                         an integer scale, with the
 *                                         scan lines (the scan-line gate)
 *   FILE.vdt OUT.bmp also takes --scale N and --scanlines.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -Isrc -o tools/minitel_selftest.exe \
 *       tools/minitel_selftest.c src/minitel_decode.c src/minitel_screen.c \
 *       src/minitel_font_data.c src/naplps_font_data.c
 *
 * Every check cites the STUM1B page it comes from (printed page numbers).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "minitel_decode.h"
#include "minitel_screen.h"

static int g_fail = 0;
static int g_pass = 0;

static void ok(int cond, const char *what)
{
    if (cond) { g_pass++; printf("  PASS  %s\n", what); }
    else      { g_fail++; printf("  FAIL  %s\n", what); }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static unsigned char g_px[MINITEL_W * MINITEL_H * 4];

static minitel_decoder_t *fresh(void)
{
    return minitel_create();
}

static void feed(minitel_decoder_t *d, const char *s, size_t n)
{
    minitel_feed(d, (const unsigned char *)s, n);
}
#define FEED(d, lit) feed((d), (lit), sizeof(lit) - 1)

static const minitel_cell_t *C(minitel_decoder_t *d, int r, int c)
{
    return minitel_cell(d, r, c);
}

static int is_alpha(minitel_decoder_t *d, int r, int c, int g)
{
    const minitel_cell_t *x = C(d, r, c);
    return x->kind == MT_KIND_ALPHA && x->glyph == g;
}

static void render(minitel_decoder_t *d, int opts)
{
    minitel_render_bgra(d, g_px, opts);
}

static unsigned long px(int x, int y)
{
    const unsigned char *p = g_px + ((size_t)y * MINITEL_W + (size_t)x) * 4;
    return ((unsigned long)p[2] << 16) | ((unsigned long)p[1] << 8) | p[0];
}

/* The paper of an ASCII alphabetic cell: dot (7, 0) is never ink for a
 * 5 x 7 glyph placed at (1, 1). */
static unsigned long paper_of(int r, int c)
{
    return px((c - 1) * MINITEL_CELL_W + 7, r * MINITEL_CELL_H);
}

static int ink_dots(int r, int c, unsigned long ink)
{
    int x, y, n = 0;
    for (y = 0; y < MINITEL_CELL_H; y++)
        for (x = 0; x < MINITEL_CELL_W; x++)
            if (px((c - 1) * MINITEL_CELL_W + x, r * MINITEL_CELL_H + y) == ink) n++;
    return n;
}

#define RGB_BLACK   0x000000UL
#define RGB_BLUE    0x0000FFUL
#define RGB_RED     0xFF0000UL
#define RGB_GREEN   0x00FF00UL
#define RGB_WHITE   0xFFFFFFUL
#define RGB_MAGENTA 0xFF00FFUL

/* Reply capture. */
static unsigned char g_reply[32];
static size_t g_reply_n;
static void reply_cb(void *ctx, const unsigned char *b, size_t n)
{
    (void)ctx;
    if (g_reply_n + n <= sizeof g_reply) { memcpy(g_reply + g_reply_n, b, n); g_reply_n += n; }
}

/* ------------------------------------------------------------------ */
/* The named traps                                                     */
/* ------------------------------------------------------------------ */

static void t_us_order(void)
{
    minitel_decoder_t *d = fresh();
    printf("US addressing, row THEN column (p96; the eMinitel trap):\n");
    /* STUM1B's own example: "L'inscription de la lettre A en 25eme
     * colonne de la rangee 05 sera provoquee par ... US 0x45 0x59 0x41". */
    FEED(d, "\x0C\x1F\x45\x59" "A");
    ok(is_alpha(d, 5, 25, 'A'), "US 0x45 0x59 'A' puts A at row 05 column 25");
    ok(!is_alpha(d, 25 - 1, 5, 'A') && C(d, 24, 5)->kind == MT_KIND_MOSAIC,
       "... and NOT at row 25-ish / column 05 (the swapped reading)");
    FEED(d, "\x1F\x45\x41" "B");
    ok(is_alpha(d, 5, 1, 'B'), "US 0x45 0x41 is the first cell of row 05");
    FEED(d, "\x1F\x59\x45" "C");    /* row 25: cannot address the screen */
    ok(is_alpha(d, 5, 2, 'C') && minitel_get_stats(d)->us_ignored == 1,
       "US 0x59 0x45 (row 25) is ignored; C lands after B");
    FEED(d, "\x1F\x32\x34" "D");    /* US 3x 3y: 'ne doit etre en aucun cas utilise' */
    ok(is_alpha(d, 5, 3, 'D'), "US 0x32 0x34 (decimal form) addresses nothing");
    FEED(d, "\x1F\x45\x00\x59" "E");
    ok(is_alpha(d, 5, 25, 'E'), "NUL inside a US sequence is padding (p98)");
    minitel_destroy(d);
}

static void t_zone_delimiter(void)
{
    minitel_decoder_t *d = fresh();
    printf("Zone delimiters and serial attributes (p93-94):\n");
    FEED(d, "\x0C\x1B\x53  ");
    ok(C(d, 1, 1)->kind == MT_KIND_DELIM && C(d, 1, 1)->bg == MT_YELLOW,
       "p94: ESC 0x53 0x20 -- the first space is a delimiter");
    ok(C(d, 1, 2)->kind == MT_KIND_ALPHA,
       "p94: ESC 0x53 0x20 0x20 -- the second space is not");
    FEED(d, "\x0C\x1B\x56 \x1B\x56 ");
    ok(C(d, 1, 1)->kind == MT_KIND_DELIM && C(d, 1, 2)->kind == MT_KIND_DELIM,
       "p94: ESC 0x56 0x20 ESC 0x56 0x20 -- both are delimiters");

    /* A blue title bar: ESC 0x54, SPACE, text. The bar starts one column
     * after the ESC (trap M-1). */
    FEED(d, "\x0C\x1B\x54 AB");
    render(d, 0);
    ok(paper_of(1, 2) == RGB_BLUE && paper_of(1, 3) == RGB_BLUE,
       "text after the delimiter is on blue paper");
    /* The row was erased by FF, and an erased cell is a black mosaic,
     * which is itself a paper delimiter (p86 notes 1 and 3): the zone
     * ends at the first cell the page did not write. */
    ok(paper_of(1, 10) == RGB_BLACK,
       "the zone stops at the first erased cell (black mosaics, p86 note 3)");
    ok(paper_of(2, 2) == RGB_BLACK, "the zone does not cross into the next row");

    /* "L'ecriture d'un caractere a l'emplacement d'un delimiteur annule
     * les attributs serie ... ils deviennent immediatement ceux de la zone
     * situee a gauche" (p94). */
    FEED(d, "\x1F\x41\x41" "X");
    render(d, 0);
    ok(paper_of(1, 2) == RGB_BLACK && paper_of(1, 3) == RGB_BLACK,
       "writing on the delimiter cancels its zone (p94)");

    /* Serial attributes are not carried by moves (p94, trap M-3). */
    FEED(d, "\x0C\x1B\x54 A\x1F\x43\x41" "B");
    render(d, 0);
    ok(paper_of(3, 1) == RGB_BLACK, "a serial paper is not carried by US to another row");
    /* A zone ends "par defaut, par la fin de la rangee" (p84). Fill row 01
     * to column 40 with one blue zone and start row 02 with text: row 02
     * must begin from the defaults, not from where row 01 ended. */
    FEED(d, "\x0C\x1B\x54 \x12\x67\x1F\x42\x41" "B");
    render(d, 0);
    ok(paper_of(1, 40) == RGB_BLUE && paper_of(2, 1) == RGB_BLACK,
       "a zone that reaches column 40 ends there; the next row starts black (p84)");
    FEED(d, "\x0C\x1B\x54 A\x0D" "B");
    render(d, 0);
    ok(C(d, 1, 1)->kind == MT_KIND_ALPHA && paper_of(1, 2) == RGB_BLACK,
       "CR back over the delimiter: B cancels it and the row goes black");
    FEED(d, "\x0C\x1B\x54   AB\x0D\x09\x09\x09" "Z");
    render(d, 0);
    ok(paper_of(1, 4) == RGB_BLUE,
       "a character written inside an existing zone takes that zone's paper");
    minitel_destroy(d);
}

static void t_mosaic_delimiter(void)
{
    minitel_decoder_t *d = fresh();
    printf("A mosaic is the paper delimiter for what follows (p86 note 1):\n");
    /* The GATE24 cloud legend, the byte pattern G-M4 found live:
     * SI ESC 50 ' ' SO ESC 47 ESC 54 <mosaic> SI "Overcast". */
    FEED(d, "\x0C\x0F\x1B\x50 \x0E\x1B\x47\x1B\x54\x5F\x0F" "Ov");
    render(d, 0);
    ok(C(d, 1, 2)->kind == MT_KIND_MOSAIC && C(d, 1, 2)->bg == MT_BLUE,
       "the mosaic takes the latent blue paper");
    ok(paper_of(1, 3) == RGB_BLUE && paper_of(1, 4) == RGB_BLUE,
       "the label after it is on BLUE paper, as p86 note 1 requires");
    FEED(d, "\x0C\x0F\x1B\x50 \x0E\x1B\x43\x1B\x50\x5F\x0F\x1B\x47" "Ov");
    render(d, 0);
    ok(paper_of(1, 3) == RGB_BLACK,
       "... and on black when the swatch's own paper is black (the fix)");
    minitel_destroy(d);
}

static void t_page_wrap(void)
{
    minitel_decoder_t *d = fresh();
    printf("Wrap at row 24 column 40 (p33, p97):\n");
    FEED(d, "\x0C\x1F\x58\x68" "XY");
    ok(is_alpha(d, 24, 40, 'X'), "X written at row 24 column 40");
    ok(is_alpha(d, 1, 1, 'Y'), "PAGE mode: the 41st character lands on row 01 column 01");
    ok(minitel_cursor_row(d) == 1 && minitel_cursor_col(d) == 2, "cursor follows to row 01 column 02");

    FEED(d, "\x0C\x1F\x58\x68" "X");
    ok(minitel_cursor_row(d) == 24 && is_alpha(d, 24, 40, 'X') && C(d, 1, 1)->kind == MT_KIND_MOSAIC,
       "nothing is written on row 01 until a 41st character actually comes");

    FEED(d, "\x0C\x1B\x3A\x69\x43");               /* PRO2 START ROULEAU */
    ok(minitel_scroll_mode(d) == 1 && minitel_get_stats(d)->protocol == 1,
       "PRO2 START ROULEAU is swallowed and sets scroll mode (p105)");
    FEED(d, "\x1F\x58\x68" "XY");
    ok(is_alpha(d, 23, 40, 'X') && is_alpha(d, 24, 1, 'Y'),
       "SCROLL mode: the screen moves up one row instead");

    FEED(d, "\x1B\x3A\x6A\x43\x0C\x1F\x41\x68" "AB");
    ok(is_alpha(d, 1, 40, 'A') && is_alpha(d, 2, 1, 'B'), "an ordinary row wraps to the next");

    /* A double-height character after the 40th of row 23 goes to row 02. */
    FEED(d, "\x0C\x1F\x57\x68" "A\x1B\x4D" "B");
    ok(is_alpha(d, 2, 1, 'B') && C(d, 1, 1)->glyph == 'B',
       "p97: double height after column 40 of row 23 goes to row 02");
    minitel_destroy(d);
}

static void t_row0(void)
{
    minitel_decoder_t *d = fresh();
    printf("Row 00, the service row (p32, p95, p97):\n");
    FEED(d, "\x0C\x1F\x45\x45\x1B\x42" "ab\x1F\x40\x41" "HELLO");
    ok(is_alpha(d, 0, 1, 'H') && is_alpha(d, 0, 5, 'O'), "US 0x40 0x41 writes on row 00");
    FEED(d, "\x0A" "c");
    ok(is_alpha(d, 5, 7, 'c') && C(d, 5, 7)->fg == MT_GREEN,
       "LF leaves row 00 to where it was, with the attributes restored");
    FEED(d, "\x0C");
    ok(is_alpha(d, 0, 1, 'H'), "FF does not clear row 00");
    FEED(d, "\x1F\x40\x67" "123");
    ok(is_alpha(d, 0, 39, '1') && is_alpha(d, 0, 40, '3'),
       "after column 40 of row 00, characters overwrite column 40 and do not wrap");
    FEED(d, "\x1B[2J");
    ok(minitel_get_stats(d)->filtered >= 1, "CSI is not available on row 00 (p95)");
    minitel_destroy(d);
}

/* ------------------------------------------------------------------ */
/* The rest of part 2 chapter 2                                        */
/* ------------------------------------------------------------------ */

static void t_rep_can(void)
{
    minitel_decoder_t *d = fresh();
    printf("REP, CAN, DEL, SUB (p95, p98, p99):\n");
    FEED(d, "\x0C" "A\x12\x43");
    ok(is_alpha(d, 1, 4, 'A') && C(d, 1, 5)->kind == MT_KIND_MOSAIC,
       "A REP 0x43 is four A (the character is not in the count)");
    /* p98: "SO, ESC, 0x42 0x5F, ESC, 0x41, REP 0x4A, ESC, 0x42, REP 0x4C
     * provoquera la visualisation de 1 pave vert, 10 paves rouges, suivi
     * de 12 paves verts." */
    FEED(d, "\x0C\x0E\x1B\x42\x5F\x1B\x41\x12\x4A\x1B\x42\x12\x4C");
    ok(C(d, 1, 1)->fg == MT_GREEN && C(d, 1, 2)->fg == MT_RED && C(d, 1, 11)->fg == MT_RED
       && C(d, 1, 12)->fg == MT_GREEN && C(d, 1, 23)->fg == MT_GREEN
       && C(d, 1, 23)->glyph == 0x3F && C(d, 1, 24)->glyph == 0,
       "p98 worked example: 1 green, 10 red, 12 green blocks");

    FEED(d, "\x0C" "ABCDE\x08\x08\x08\x1B\x44\x18");
    ok(is_alpha(d, 1, 2, 'B') && is_alpha(d, 1, 3, 0x20) && is_alpha(d, 1, 40, 0x20)
       && C(d, 1, 3)->fg == MT_BLUE,
       "CAN fills to the end of the row with spaces in the current attributes");
    ok(minitel_cursor_col(d) == 3, "CAN does not move the cursor");
    ok(C(d, 1, 3)->kind == MT_KIND_ALPHA, "CAN is not a delimiter");

    FEED(d, "\x0C\x7F\x0E\x7F\x0F\x1A");
    ok(is_alpha(d, 1, 1, MT_G_FULLBLOCK), "DEL in code is a full block (p98)");
    ok(C(d, 1, 2)->kind == MT_KIND_MOSAIC && C(d, 1, 2)->glyph == 0x3F,
       "DEL out of code is exactly G1 0x5F");
    ok(is_alpha(d, 1, 3, MT_G_FULLBLOCK) && minitel_get_stats(d)->errors == 1,
       "SUB shows the error symbol (p99)");
    minitel_destroy(d);
}

static void t_erasures_csi(void)
{
    minitel_decoder_t *d = fresh();
    printf("FF, CSI moves and erasures (p94-96):\n");
    FEED(d, "\x0C" "ABC\x0C");
    ok(C(d, 1, 1)->kind == MT_KIND_MOSAIC && C(d, 1, 1)->glyph == 0 && C(d, 1, 1)->bg == MT_BLACK,
       "FF fills rows 01-24 with black mosaics (p96)");
    /* p96: "CSI 0x35 0x3B 0x32 0x35 0x48 0x41" puts A at row 05 column 25. */
    FEED(d, "\x1B[5;25HA");
    ok(is_alpha(d, 5, 25, 'A'), "CSI 5;25 H 'A' (the p96 example)");
    FEED(d, "\x1B[2A" "B");
    ok(is_alpha(d, 3, 26, 'B'), "CSI 2 A moves up two rows");
    FEED(d, "\x1B[30A" "C");
    ok(is_alpha(d, 1, 27, 'C'), "CSI A stops at the top");
    FEED(d, "\x1B[5;1H" "WXYZ\x1B[5;3H\x1B[K");
    ok(is_alpha(d, 5, 2, 'X') && C(d, 5, 3)->kind == MT_KIND_MOSAIC && C(d, 5, 25)->kind == MT_KIND_MOSAIC,
       "CSI K erases from the cursor, inclusive, to the end of the row");
    FEED(d, "\x1B[1K");
    ok(C(d, 5, 1)->kind == MT_KIND_MOSAIC, "CSI 1 K erases from the start of the row");
    FEED(d, "\x1B[3;1HQ\x1B[2J");
    ok(C(d, 3, 1)->kind == MT_KIND_MOSAIC && minitel_cursor_row(d) == 3,
       "CSI 2 J clears the screen and leaves the cursor");
    FEED(d, "\x1B[99z");
    ok(minitel_get_stats(d)->filtered >= 1, "an unknown CSI is filtered");
    minitel_destroy(d);
}

static void t_filters(void)
{
    minitel_decoder_t *d = fresh();
    const minitel_stats_t *s;
    printf("Filters, resynchronisation and transparency (p99):\n");
    FEED(d, "\x0C\x13" "AB");
    ok(is_alpha(d, 1, 1, 'B'), "SEP X is swallowed, never displayed (0x13 is not XOFF)");
    FEED(d, "\x0C\x1D" "AB\x1B\x35" "CD\x1B\x39\x7F" "E");
    ok(is_alpha(d, 1, 1, 'B') && is_alpha(d, 1, 2, 'D') && is_alpha(d, 1, 3, 'E'),
       "SS3 X, ESC 0x35 X and PRO1 RESET are swallowed");
    FEED(d, "\x0C\x1F\x45\x0D" "A");
    s = minitel_get_stats(d);
    ok(s->resync >= 1 && is_alpha(d, 1, 1, 'A'),
       "a C0 inside a US resynchronises and is executed (CR)");
    FEED(d, "\x0C" "Q\x1B\x0C" "R");
    ok(is_alpha(d, 1, 1, 'R'), "ESC followed by FF: the FF is executed");
    FEED(d, "\x0C\x1B\x25" "ABC\x0C\x1B\x25\x40" "D");
    ok(is_alpha(d, 1, 1, 'D'), "ESC 0x25 ... ESC 0x25 0x40: transparency, even FF ignored");
    FEED(d, "\x0C\x1B\x25" "AB\x1B\x2F\x3F" "E");
    ok(is_alpha(d, 1, 1, 'E'), "ESC 0x2F 0x3F also ends transparency");
    /* Trap M-6: a NAPLPS frame on a Minitel is a blank screen, not garbage. */
    FEED(d, "\x0C\x1B\x25\x41\x20\x3F\x7F\x40\x40\x40\x40");
    ok(C(d, 1, 1)->kind == MT_KIND_MOSAIC, "a NAPLPS frame (ESC % A ...) shows nothing");
    FEED(d, "\x1B\x25\x40\x0C\x1B\x7A" "A");
    ok(is_alpha(d, 1, 1, 'A'), "an undefined ESC X is filtered");
    FEED(d, "\x0C\x1B\x28\x40" "A");
    ok(is_alpha(d, 1, 1, 'A'), "an ISO 2022 designation ESC 2x F is filtered");
    minitel_destroy(d);
}

static void t_masking(void)
{
    minitel_decoder_t *d = fresh();
    printf("Masking (p85, p94):\n");
    FEED(d, "\x0C\x1B\x58 AB");
    render(d, 0);
    ok(ink_dots(1, 2, RGB_WHITE) == 0, "a masked zone shows paper only (masking on at power-on)");
    FEED(d, "\x1B\x23\x20\x5F");
    render(d, 0);
    ok(ink_dots(1, 2, RGB_WHITE) > 0, "ESC 0x23 0x20 0x5F reveals it");
    FEED(d, "\x1B\x23\x20\x58");
    render(d, 0);
    ok(ink_dots(1, 2, RGB_WHITE) == 0, "ESC 0x23 0x20 0x58 masks it again");
    minitel_destroy(d);
}

static void t_underline(void)
{
    minitel_decoder_t *d = fresh();
    printf("Lignage: underline in G0, separated in G1 (p85, p86 note 4, p91, p94):\n");
    FEED(d, "\x0C\x1B\x5A AB");
    render(d, 0);
    ok(px(1 * 8 + 0, 19) == RGB_WHITE && px(0 * 8 + 0, 19) == RGB_BLACK,
       "ESC 0x5A SPACE: the zone after the delimiter is underlined, the delimiter not");
    FEED(d, "\x0C\x1B\x5A AB\x0E\x1B\x50\x21\x0F" "CD");
    render(d, 0);
    ok(px(4 * 8 + 0, 19) == RGB_BLACK,
       "p86 note 4: underlining does not come back after a mosaic");
    FEED(d, "\x0C\x0E\x1B\x5A\x7F\x0F\x0E\x7F");
    ok((C(d, 1, 1)->flags & MT_F_SEPARATED) && !(C(d, 1, 2)->flags & MT_F_SEPARATED),
       "ESC 0x5A in G1 separates the mosaic; SI then SO clears it (p94)");
    render(d, 0);
    ok(px(3, 10) == RGB_BLACK && px(0, 10) == RGB_WHITE && px(8 + 3, 10) == RGB_WHITE,
       "a separated mosaic shows the paper grid; a joined one does not");
    minitel_destroy(d);
}

static void t_sizes(void)
{
    minitel_decoder_t *d = fresh();
    printf("Sizes (p91-93, p97):\n");
    FEED(d, "\x0C\x1F\x45\x41\x1B\x4D" "A");
    ok(is_alpha(d, 5, 1, 'A') && is_alpha(d, 4, 1, 'A')
       && (C(d, 4, 1)->size & MT_S_TOP) && !(C(d, 5, 1)->size & MT_S_TOP),
       "double height is written on the addressed row and extends UP (p91)");
    FEED(d, "\x0C\x1B\x4D" "B");
    ok(is_alpha(d, 1, 1, 'B') && C(d, 1, 1)->size == 0,
       "double height on row 01 is not taken (trap M-4)");
    FEED(d, "\x0C\x1F\x45\x41\x1B\x4E" "CD");
    ok(is_alpha(d, 5, 1, 'C') && is_alpha(d, 5, 2, 'C') && is_alpha(d, 5, 3, 'D')
       && (C(d, 5, 2)->size & MT_S_RIGHT), "double width takes two columns");
    FEED(d, "\x0C\x1F\x45\x68\x1B\x4F" "E");
    ok(is_alpha(d, 5, 40, 'E') && is_alpha(d, 4, 40, 'E') && !(C(d, 5, 40)->size & MT_S_DW),
       "double size in column 40 degrades to double height (p97)");
    FEED(d, "\x0C\x1F\x45\x41\x1B\x4F\x1B\x5D\x0E\x0F" "F");
    ok(is_alpha(d, 5, 1, 'F') && C(d, 5, 1)->size == 0 && !(C(d, 5, 1)->flags & MT_F_INVERT),
       "SO cancels size and inverse, definitively after SI (p91)");
    minitel_destroy(d);
}

static void t_g2(void)
{
    minitel_decoder_t *d = fresh();
    printf("G2 by SS2 (p88-90, schema 2.8):\n");
    FEED(d, "\x0C\x19\x42" "e\x19\x23\x19\x41" "a\x19\x4B" "c\x19\x41" "x\x19\x7B\x19\x6F");
    ok(is_alpha(d, 1, 1, MT_G_E_ACUTE), "SS2 0x42 0x65 is e acute (the p88 example)");
    ok(is_alpha(d, 1, 2, MT_G_POUND), "SS2 0x23 is the pound (p90)");
    ok(is_alpha(d, 1, 3, MT_G_A_GRAVE) && is_alpha(d, 1, 4, MT_G_C_CEDIL), "a grave, c cedilla");
    ok(is_alpha(d, 1, 5, 'x'), "an accent on a letter outside the repertoire shows the letter alone");
    ok(is_alpha(d, 1, 6, MT_G_BETA), "SS2 0x7B is beta");
    ok(is_alpha(d, 1, 7, MT_G_LOWBAR), "an unknown G2 code shows the low bar (p90)");
    FEED(d, "\x0C\x0E\x19\x21");
    ok(C(d, 1, 1)->kind == MT_KIND_MOSAIC && C(d, 1, 1)->glyph == 0x01,
       "SS2 is ignored in G1 (p90)");
    FEED(d, "\x0C\x0F\x19\x0D" "A");
    ok(is_alpha(d, 1, 1, 'A'), "SS2 followed by C0: the control is executed");
    minitel_destroy(d);
}

static void t_render_misc(void)
{
    minitel_decoder_t *d = fresh();
    printf("Rendering: grey table, flash, cursor, inverse (p32, p84, p99):\n");
    ok(minitel_rgb(MT_BLUE, 1) == 0x666666UL && minitel_rgb(MT_RED, 1) == 0x808080UL
       && minitel_rgb(MT_YELLOW, 1) == 0xE6E6E6UL && minitel_rgb(MT_WHITE, 1) == 0xFFFFFFUL,
       "monochrome maps through STUM1B's grey table (blue 40%, red 50%, yellow 90%)");
    FEED(d, "\x0C\x1B\x48" "A");
    render(d, MT_R_FLASH);
    ok(ink_dots(1, 1, RGB_WHITE) == 0, "flash: the off phase shows the paper");
    render(d, 0);
    ok(ink_dots(1, 1, RGB_WHITE) > 0, "flash: the on phase shows the ink");
    FEED(d, "\x0C\x1B\x5D" "A");
    render(d, 0);
    ok(paper_of(1, 1) == RGB_WHITE, "inverse: ink and paper change places");
    FEED(d, "\x0C\x11");
    render(d, MT_R_CURSOR);
    ok(paper_of(1, 1) == RGB_WHITE, "Con: the cursor cell shows the complementary colours");
    FEED(d, "\x14");
    render(d, MT_R_CURSOR);
    ok(paper_of(1, 1) == RGB_BLACK, "Coff: no cursor");
    minitel_set_status_letter(d, 'C');
    render(d, 0);
    ok(px(38 * 8 + 7, 0) == RGB_WHITE, "the status letter is drawn black on white at row 00 column 39");
    minitel_destroy(d);
}

static void t_replies(void)
{
    minitel_decoder_t *d = fresh();
    printf("Terminal replies (p98, p139):\n");
    minitel_set_reply(d, reply_cb, NULL);
    g_reply_n = 0;
    FEED(d, "\x0C\x1F\x45\x59\x1B\x61");
    ok(g_reply_n == 3 && g_reply[0] == 0x1F && g_reply[1] == 0x45 && g_reply[2] == 0x59,
       "ESC 0x61 answers US row col, row first");
    g_reply_n = 0;
    FEED(d, "\x1B\x39\x7B");
    ok(g_reply_n == 5 && g_reply[0] == 0x01 && g_reply[4] == 0x04, "PRO1 ENQROM answers SOH x x x EOT");
    minitel_destroy(d);
}

/* The scan-line presentation (the values measured on MiniPavi's
 * emulator; see minitel_screen.c and the report, section 9). */
static unsigned char g_big[MINITEL_W * 3 * MINITEL_H * 3 * 4];

static void big_px(int w, int x, int y, int *r, int *g, int *b)
{
    const unsigned char *p = g_big + ((size_t)y * w + x) * 4;
    *b = p[0]; *g = p[1]; *r = p[2];
}

static void t_present(void)
{
    minitel_decoder_t *d = fresh();
    int r, g, b, w2 = MINITEL_W * 2, w3 = MINITEL_W * 3;
    printf("Presentation and scan lines (observed on MiniPavi's emulator):\n");
    /* A white full block at row 1 column 1, the rest black. */
    FEED(d, "\x0C\x7F");
    render(d, 0);
    minitel_present_bgra(g_px, 2, g_big, 0);
    big_px(w2, 0, 20, &r, &g, &b);
    ok(r == 255 && g == 255 && b == 255, "without scan lines, 2x is a plain pixel doubling");
    minitel_present_bgra(g_px, 2, g_big, MT_P_SCANLINES);
    big_px(w2, 0, 20, &r, &g, &b);
    ok(r == 253 && g == 244 && b == 243, "lit row, column phase 0: white is (253,244,243) as measured");
    big_px(w2, 1, 20, &r, &g, &b);
    ok(r == 250 && g == 255 && b == 250, "lit row, column phase 1: white is (250,255,250)");
    big_px(w2, 2, 20, &r, &g, &b);
    ok(r == 243 && g == 244 && b == 253, "lit row, column phase 2: white is (243,244,253)");
    big_px(w2, 0, 21, &r, &g, &b);
    ok(r == 138 && g == 134 && b == 133, "dark row, column phase 0: white is (138,134,133)");
    big_px(w2, 0, 0, &r, &g, &b);
    ok(r == 10 && g == 1 && b == 0, "black is lifted to the triad's grain, (10,1,0) on a lit row");
    minitel_present_bgra(g_px, 3, g_big, MT_P_SCANLINES);
    big_px(w3, 0, 30, &r, &g, &b); ok(r == 253, "3x: the first two rows of a pixel row are lit");
    big_px(w3, 0, 31, &r, &g, &b); ok(r == 253, "3x: ... the second too");
    big_px(w3, 0, 32, &r, &g, &b); ok(r == 138, "3x: the third is dark");
    minitel_present_bgra(g_px, 2, g_big, MT_P_SCANLINES | MT_P_MONO);
    big_px(w2, 0, 20, &r, &g, &b);
    ok(r == g && g == b && r == 248, "monochrome: rows only, no RGB triad (white 248 on a lit row)");
    big_px(w2, 1, 21, &r, &g, &b);
    ok(r == g && g == b && r == 136, "monochrome: white 136 on a dark row");
    minitel_destroy(d);
}

static void run_selftests(void)
{
    t_us_order();
    t_zone_delimiter();
    t_mosaic_delimiter();
    t_page_wrap();
    t_row0();
    t_rep_can();
    t_erasures_csi();
    t_filters();
    t_masking();
    t_underline();
    t_sizes();
    t_g2();
    t_render_misc();
    t_replies();
    t_present();
    printf("%d passed, %d failed\n", g_pass, g_fail);
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > (16L << 20)) { fclose(f); return NULL; }
    buf = (unsigned char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    *n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    return buf;
}

/* A 24-bit BMP of a B G R 0 buffer. Widths here are multiples of 4, so
 * rows need no padding. */
static int write_bmp_buf(const char *path, const unsigned char *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    unsigned char hdr[54];
    int rowbytes = w * 3, y, x;
    unsigned long size = 54UL + (unsigned long)rowbytes * (unsigned long)h;
    if (!f) return 1;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (unsigned char)size; hdr[3] = (unsigned char)(size >> 8);
    hdr[4] = (unsigned char)(size >> 16); hdr[5] = (unsigned char)(size >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (unsigned char)w; hdr[19] = (unsigned char)(w >> 8);
    hdr[22] = (unsigned char)h; hdr[23] = (unsigned char)(h >> 8);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (y = h - 1; y >= 0; y--) {
        for (x = 0; x < w; x++) {
            const unsigned char *p = px + ((size_t)y * w + (size_t)x) * 4;
            fputc(p[0], f); fputc(p[1], f); fputc(p[2], f);
        }
    }
    fclose(f);
    return 0;
}

static int write_bmp(const char *path)
{
    return write_bmp_buf(path, g_px, MINITEL_W, MINITEL_H);
}

/* Read a 320 x 250, 24-bit, bottom-up BMP into g_px. */
static int read_bmp_320x250(const char *path)
{
    size_t n = 0;
    unsigned char *b = slurp(path, &n);
    unsigned off;
    int w, h, x, y;
    if (!b || n < 54) { free(b); return 1; }
    off = b[10] | (b[11] << 8) | ((unsigned)b[12] << 16);
    w = b[18] | (b[19] << 8);
    h = b[22] | (b[23] << 8);
    if (w != MINITEL_W || h != MINITEL_H || b[28] != 24
        || n < off + (size_t)MINITEL_W * MINITEL_H * 3) { free(b); return 1; }
    for (y = 0; y < MINITEL_H; y++)
        for (x = 0; x < MINITEL_W; x++) {
            const unsigned char *s = b + off + ((size_t)(MINITEL_H - 1 - y) * MINITEL_W + x) * 3;
            unsigned char *d = g_px + ((size_t)y * MINITEL_W + x) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0;
        }
    free(b);
    return 0;
}

/* Parse --scale N and --scanlines / --mono; write either the raw raster
 * or the presented one. */
static int write_output(const char *path, int argc, char **argv, int first)
{
    int i, scale = 1, popts = 0;
    for (i = first; i < argc; i++) {
        if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) scale = atoi(argv[++i]);
        else if (strcmp(argv[i], "--scanlines") == 0) popts |= MT_P_SCANLINES;
        else if (strcmp(argv[i], "--mono") == 0) popts |= MT_P_MONO;
    }
    if (scale < 1 || scale > 3) scale = 1;
    if (scale == 1 && !(popts & MT_P_SCANLINES)) return write_bmp(path);
    minitel_present_bgra(g_px, scale, g_big, popts);
    return write_bmp_buf(path, g_big, MINITEL_W * scale, MINITEL_H * scale);
}

static void print_stats(const minitel_decoder_t *d)
{
    const minitel_stats_t *s = minitel_get_stats(d);
    printf("%lu bytes  %lu chars  %lu filtered  %lu resync  %lu error  "
           "%lu row00  %lu pro  %lu unrep  %lu bit8  %lu bel  %lu us-ign\n",
           s->bytes_in, s->chars, s->filtered, s->resync, s->errors,
           s->row0, s->protocol, s->unrepresent, s->bit8, s->bells, s->us_ignored);
}

static int decode_file(int argc, char **argv)
{
    size_t n = 0;
    unsigned char *buf = slurp(argv[1], &n);
    minitel_decoder_t *d;
    int opts = 0, i;
    if (!buf) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    d = minitel_create();
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--mono") == 0) opts |= MT_R_MONO;
        else if (strcmp(argv[i], "--status") == 0 && i + 1 < argc) {
            minitel_set_status_letter(d, argv[++i][0]);
        }
    }
    minitel_feed(d, buf, n);
    render(d, opts);
    free(buf);
    print_stats(d);
    i = write_output(argv[2], argc, argv, 3);
    minitel_destroy(d);
    return i;
}

/*
 * One line per cell: row col kind glyph fg bg flags size. For an
 * alphabetic cell the paper printed is the one the renderer will use,
 * derived along the row exactly as minitel_screen.c derives it, so the
 * G2 comparison compares like with like.
 */
static int dump_file(const char *path)
{
    size_t n = 0;
    unsigned char *buf = slurp(path, &n);
    minitel_decoder_t *d;
    int r, c;
    if (!buf) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    d = minitel_create();
    minitel_feed(d, buf, n);
    free(buf);
    for (r = 0; r < MINITEL_ROWS; r++) {
        int z_bg = MT_BLACK, z_mask = 0, z_ul = 0;
        for (c = 1; c <= MINITEL_COLS; c++) {
            const minitel_cell_t *x = minitel_cell(d, r, c);
            int paper;
            if (x->kind == MT_KIND_DELIM) {
                z_bg = x->bg;
                z_mask = (x->flags & MT_F_ZMASK) != 0;
                z_ul = (x->flags & MT_F_ZUNDER) != 0;
            }
            paper = (x->kind == MT_KIND_ALPHA) ? z_bg : x->bg;
            printf("%d %d %c %d %d %d %d %d %d %d\n", r, c,
                   x->kind == MT_KIND_ALPHA ? 'A' : x->kind == MT_KIND_MOSAIC ? 'M' : 'D',
                   x->glyph, x->fg, paper, x->flags, x->size,
                   z_mask && minitel_screen_masked(d), z_ul);
            if (x->kind == MT_KIND_MOSAIC) { z_bg = x->bg; z_ul = 0; }
        }
    }
    print_stats(d);
    minitel_destroy(d);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--dump") == 0) return dump_file(argv[2]);
    if (argc >= 4 && strcmp(argv[1], "--present") == 0) {
        if (read_bmp_320x250(argv[2])) {
            fprintf(stderr, "%s is not a 320 x 250 24-bit BMP\n", argv[2]);
            return 1;
        }
        return write_output(argv[3], argc, argv, 4);
    }
    if (argc >= 3) return decode_file(argc, argv);
    run_selftests();
    return g_fail ? 1 : 0;
}
