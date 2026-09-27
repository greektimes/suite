/*
 * minitel_decode.c - Minitel (Teletel videotex) decoder and page memory.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from STUM1B; the provenance statement is at the top of
 * minitel_decode.h. Page citations below are STUM1B's PRINTED page
 * numbers (scan PDF page = printed page + 3).
 *
 * THE MODEL, in one paragraph, because everything else follows from it.
 * The Minitel keeps a page memory of 25 x 40 cells (p28). Each cell is
 * one of three things (p86): an alphabetic character, a semi-graphic
 * (mosaic) character, or a zone DELIMITER. Character-level attributes
 * (ink, flash, size, inverse; and for a mosaic its own paper and the
 * separated look) are stored in the cell. SERIAL attributes (paper,
 * masking, underline of an alphabetic zone) are not: the display
 * processor carries them along the row from the last delimiter, and a
 * mosaic also hands its paper on to the alphabetic zone that follows it
 * (p86 note 1, p93). So an alphabetic cell has no paper of its own; the
 * renderer (minitel_screen.c) derives it by scanning the row, which is
 * exactly why "writing a character on a delimiter cancels that zone, and
 * the zone takes the attributes of the one to its left" (p94) needs no
 * code at all here. The decoder's job is to keep the LATENT serial
 * context and to decide when a SPACE becomes a delimiter.
 *
 * This translation unit is deliberately free of Win32 and GDI calls.
 */

#include "minitel_decode.h"

#include <stdlib.h>
#include <string.h>

/* Parser states. */
enum {
    ST_GROUND = 0,
    ST_ESC,          /* after ESC                                        */
    ST_ISO,          /* ESC 2x ... F, collecting intermediates (p99)     */
    ST_CSI,          /* ESC [ params intermediates final (p94-95, p99)   */
    ST_US1, ST_US2,  /* US row col (p96)                                 */
    ST_REP,          /* REP count (p98)                                  */
    ST_SS2,          /* SS2 code (p88-90)                                */
    ST_SS2_ACC,      /* SS2 accent, waiting for its letter               */
    ST_SKIP1,        /* SEP X, SS3 X, ESC 35/36/37 X: one byte (p99)     */
    ST_PRO,          /* PRO1/2/3 arguments (p134)                        */
    ST_TRANSP,       /* screen transparency (p99)                        */
    ST_TRANSP_ESC,
    ST_TRANSP_ESC25,
    ST_TRANSP_ESC2F
};

/* The character-level attributes and the code position, which US, RS
 * and FF reset (p96) and which the row 00 excursion saves (p97). */
typedef struct {
    unsigned char fg;
    unsigned char flash;
    unsigned char invert;
    unsigned char size;       /* MT_S_DW | MT_S_DH                        */
    unsigned char separated;  /* lignage in G1                            */
    unsigned char g1;         /* SO in force                              */
    /* The latent serial context (p93). */
    unsigned char s_bg;
    unsigned char s_mask;
    unsigned char s_ul;
    unsigned char s_pending;       /* any serial definition since the last
                                      delimiter                          */
    unsigned char s_pending_other; /* ... other than the paper colour    */
} attr_t;

/* What REP repeats: the last character DISPLAYED (p98), as a code in its
 * set, so it is redrawn with the attributes current at the REP. */
enum { LAST_NONE = 0, LAST_G0, LAST_G1, LAST_GLYPH };

struct minitel_decoder {
    minitel_cell_t mem[MINITEL_ROWS][MINITEL_COLS + 2];  /* col 1..40 */

    int row, col;          /* current position; col 41 = wrap pending   */
    attr_t a;

    int in_row0;           /* the row 00 excursion (p97)                */
    int sv_row, sv_col;
    attr_t sv_a;

    int cursor_on;
    int screen_mask;       /* 1 = masked zones hidden (the default, p85) */
    int scroll;            /* 0 page, 1 rouleau                          */
    int insert;            /* CSI 4 h                                    */

    int st;
    unsigned char us_r;
    int acc;               /* the pending SS2 accent code                */
    int iso_first;         /* the first intermediate of an ESC 2x seq    */
    int iso_n;
    unsigned char iso[4];
    int csi_p[4], csi_np, csi_have, csi_bad, csi_priv;
    int pro_left, pro_n;
    unsigned char pro[4];

    int last_kind;
    unsigned char last_code;

    char status_letter;
    int ever;

    minitel_reply_fn reply;
    void *reply_ctx;

    minitel_stats_t stats;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void attr_default(attr_t *a)
{
    memset(a, 0, sizeof *a);
    a->fg = MT_WHITE;
    a->s_bg = MT_BLACK;
}

/*
 * The erased cell. "Un effacement de l'ecran du Minitel correspond au
 * remplissage ... de la memoire de page avec des codes correspondants a
 * des paves noirs semi-graphiques afin d'eviter que la couleur de fond ou
 * le lignage anticipent" (p86 note 3; p96 for FF, CSI J and CSI K). A
 * black mosaic is also a paper delimiter, which is the point: an erased
 * cell stops a zone's paper leaking along the row.
 */
static minitel_cell_t black_mosaic(void)
{
    minitel_cell_t c;
    c.kind  = MT_KIND_MOSAIC;
    c.glyph = 0;
    c.fg    = MT_BLACK;
    c.bg    = MT_BLACK;
    c.flags = 0;
    c.size  = 0;
    return c;
}

static void clear_row(minitel_decoder_t *d, int r, int c0, int c1)
{
    int c;
    minitel_cell_t z = black_mosaic();
    if (c0 < 1) c0 = 1;
    if (c1 > MINITEL_COLS) c1 = MINITEL_COLS;
    for (c = c0; c <= c1; c++) d->mem[r][c] = z;
}

static void clear_rows(minitel_decoder_t *d, int r0, int r1)
{
    int r;
    for (r = r0; r <= r1; r++) clear_row(d, r, 1, MINITEL_COLS);
}

static void scroll_up(minitel_decoder_t *d, int top, int bottom, int n)
{
    int r;
    while (n-- > 0) {
        for (r = top; r < bottom; r++)
            memcpy(d->mem[r], d->mem[r + 1], sizeof d->mem[r]);
        clear_row(d, bottom, 1, MINITEL_COLS);
    }
}

static void scroll_down(minitel_decoder_t *d, int top, int bottom, int n)
{
    int r;
    while (n-- > 0) {
        for (r = bottom; r > top; r--)
            memcpy(d->mem[r], d->mem[r - 1], sizeof d->mem[r]);
        clear_row(d, top, 1, MINITEL_COLS);
    }
}

static void send_reply(minitel_decoder_t *d, const unsigned char *b, size_t n)
{
    if (d->reply) d->reply(d->reply_ctx, b, n);
}

/*
 * US, RS and FF are article separators: "l'ecran est remis en position SI
 * ... Les caracteres recus sont initialises en blanc sur fond noir, non
 * masques, non lignes, fixes et de simple taille" (p96). They do not
 * touch page or scroll mode.
 */
static void separator_reset(minitel_decoder_t *d)
{
    attr_default(&d->a);
}

/* Leave row 00 without the LF restore (RS, FF, US to another row). */
static void leave_row0(minitel_decoder_t *d)
{
    d->in_row0 = 0;
}

/* Normalise the pending-wrap position before any move but a character. */
static void settle_col(minitel_decoder_t *d)
{
    if (d->col > MINITEL_COLS) d->col = MINITEL_COLS;
}

/* ------------------------------------------------------------------ */
/* Moves, p94-97                                                       */
/* ------------------------------------------------------------------ */

static void do_lf(minitel_decoder_t *d)
{
    settle_col(d);
    if (d->in_row0) {
        /* "la reception de LF deplace cette position courante au dernier
         * emplacement entre les rangees 1 et 24 ... et tous les attributs
         * definis au moment du deplacement vers la rangee 00 seront
         * restitues et valides" (p97). */
        d->row = d->sv_row;
        d->col = d->sv_col;
        d->a   = d->sv_a;
        d->in_row0 = 0;
        return;
    }
    if (d->row >= 24) {
        if (d->scroll) scroll_up(d, 1, 24, 1);
        else d->row = 1;
    } else {
        d->row++;
    }
}

static void do_vt(minitel_decoder_t *d)
{
    settle_col(d);
    if (d->in_row0) return;                  /* "VT ne provoque aucune action" */
    if (d->row <= 1) {
        if (d->scroll) scroll_down(d, 1, 24, 1);
        else d->row = 24;
    } else {
        d->row--;
    }
}

static void do_bs(minitel_decoder_t *d)
{
    if (d->col > MINITEL_COLS) { d->col = MINITEL_COLS; return; }
    if (d->col > 1) { d->col--; return; }
    if (d->in_row0) return;
    if (d->row > 1) { d->row--; d->col = MINITEL_COLS; }
    else { d->row = 24; d->col = MINITEL_COLS; }
}

static void do_ht(minitel_decoder_t *d)
{
    settle_col(d);
    if (d->col < MINITEL_COLS) { d->col++; return; }
    if (d->in_row0) return;
    d->col = 1;
    if (d->row < 24) d->row++;
    else d->row = 1;             /* p97: col 40 of row 24 -> row 01 col 01 */
}

/* ------------------------------------------------------------------ */
/* Writing a character                                                 */
/* ------------------------------------------------------------------ */

/*
 * Put one cell template at the current position, honouring the size it
 * carries, then advance.
 *
 * THE WRAP IS DEFERRED, and STUM1B decides that: "dans le cas d'un
 * caractere double hauteur ... transmis APRES le 40e caractere de la
 * rangee 24 ou de la rangee 23, il est affiche au debut de la rangee 02"
 * (p97). How far the 41st character wraps depends on ITS OWN size, so
 * the wrap can only be made when that character arrives. Column 41 is
 * the pending state.
 *
 * PAGE MODE TRAP: the 41st character of row 24 lands on row 01 (p33,
 * p97), i.e. at the top of the screen. Tested in the self-test.
 */
static void put_cell(minitel_decoder_t *d, minitel_cell_t c)
{
    int dw, dh, r, k;

    d->ever = 1;

    if (d->in_row0) {
        /* Row 00: no sizes, and nothing wraps; everything after column 40
         * overwrites column 40 (p97). */
        c.size = 0;
        if (d->col > MINITEL_COLS) d->col = MINITEL_COLS;
        d->mem[0][d->col] = c;
        d->stats.row0++;
        d->stats.chars++;
        if (d->col < MINITEL_COLS) d->col++;
        return;
    }

    dh = (c.size & MT_S_DH) != 0;
    dw = (c.size & MT_S_DW) != 0;

    if (d->col > MINITEL_COLS) {
        d->col = 1;
        d->row += dh ? 2 : 1;
        if (d->row > 24) {
            if (d->scroll) {
                scroll_up(d, 1, 24, d->row - 24);
                d->row = 24;
            } else {
                d->row = dh ? 2 : 1;
            }
        }
    }

    /* A double height cannot start on row 01 (p93, p97). */
    if (dh && d->row < 2) { dh = 0; c.size &= (unsigned char)~MT_S_DH; }

    /* "Lorsqu'un caractere double taille (resp. double largeur) doit etre
     * affiche alors que la position courante est sur la colonne 40, le
     * traitement assure la visualisation de ce caractere en double
     * hauteur (resp. taille normale) sur cette colonne 40" (p97). */
    if (dw && d->col == MINITEL_COLS) { dw = 0; c.size &= (unsigned char)~MT_S_DW; }

    if (d->insert && !dw) {
        memmove(&d->mem[d->row][d->col + 1], &d->mem[d->row][d->col],
                (size_t)(MINITEL_COLS - d->col) * sizeof(minitel_cell_t));
    }

    /* The same configuration on 2 or 4 cells (p93); the part bits say
     * which quarter each one shows. */
    for (r = 0; r <= dh; r++) {
        for (k = 0; k <= dw; k++) {
            minitel_cell_t q = c;
            q.size = (unsigned char)((c.size & (MT_S_DW | MT_S_DH))
                     | (k ? MT_S_RIGHT : 0) | (r ? MT_S_TOP : 0));
            d->mem[d->row - r][d->col + k] = q;
        }
    }

    d->stats.chars++;
    d->col += dw ? 2 : 1;        /* may reach 41: the pending wrap */
}

/* The template for an alphabetic cell in the current attributes. */
static minitel_cell_t alpha_cell(minitel_decoder_t *d, unsigned char glyph)
{
    minitel_cell_t c;
    c.kind  = MT_KIND_ALPHA;
    c.glyph = glyph;
    c.fg    = d->a.fg;
    c.bg    = MT_BLACK;          /* unused: the zone decides (p94) */
    c.flags = (unsigned char)((d->a.flash ? MT_F_FLASH : 0)
                            | (d->a.invert ? MT_F_INVERT : 0));
    c.size  = d->a.size;
    return c;
}

/*
 * A mosaic takes its paper from the latent context: "lors de la
 * reception d'un caractere semi-graphique (jeu G1) qui permet de valider
 * l'attribut couleur de fond" (p93). It validates ONLY the paper; "La
 * prise en compte des autres attributs de zone latents se fera avec le
 * premier delimiteur explicite rencontre". So if the paper was the only
 * pending definition, the next space is an ordinary space again; if
 * masking or lining is still latent, the next space is still a
 * delimiter. That split is this implementation's reading of p93 and is
 * recorded in the report.
 */
static minitel_cell_t mosaic_cell(minitel_decoder_t *d, unsigned char bits)
{
    minitel_cell_t c;
    c.kind  = MT_KIND_MOSAIC;
    c.glyph = (unsigned char)(bits & 0x3F);
    c.fg    = d->a.fg;
    c.bg    = d->a.s_bg;
    c.flags = (unsigned char)((d->a.flash ? MT_F_FLASH : 0)
                            | (d->a.separated ? MT_F_SEPARATED : 0));
    c.size  = 0;                 /* sizes do not apply in G1 (p91) */
    if (!d->a.s_pending_other) d->a.s_pending = 0;
    return c;
}

/*
 * G1 bit layout, read from schema 2.6 (printed p101): b1 top left, b2 top
 * right, b3 middle left, b4 middle right, b5 bottom left, b7 bottom
 * right; b6 is 1 for every mosaic, which is why they live in columns 2,
 * 3, 6 and 7. 0x5F is the full block (schema 2.6 and the p98 example).
 * Columns 4 and 5 otherwise hold nothing in the drawing; those codes are
 * shown as an empty mosaic and counted as unrepresentable.
 */
static void put_g1(minitel_decoder_t *d, unsigned char b)
{
    unsigned char bits;

    if (b == 0x5F) bits = 0x3F;
    else if (b >= 0x40 && b <= 0x5E) { bits = 0; d->stats.unrepresent++; }
    else bits = (unsigned char)((b & 0x1F) | ((b & 0x40) >> 1));

    put_cell(d, mosaic_cell(d, bits));
    d->last_kind = LAST_G1;
    d->last_code = b;
}

/*
 * SPACE (p93-94, p98). In G0, the first space after a serial attribute
 * definition becomes a DELIMITER carrying the latent context, "meme s'il
 * n'y a pas modification"; any other space is an ordinary alphabetic
 * space and takes the zone it lands in. The p94 examples, both tested:
 * "ESC 0x53 0x20 0x20 le second espace n'est pas un delimiteur"; "ESC
 * 0x56 0x20 ESC 0x56 0x20 les deux espaces sont des delimiteurs".
 *
 * In G1 the space is written as an EMPTY MOSAIC: it is "affecte par tous
 * les attributs de visualisation courants" (p98), which in G1 includes
 * the character-level paper. This is this implementation's reading and
 * is recorded in the report.
 */
static void put_space(minitel_decoder_t *d)
{
    if (d->a.g1) {
        put_cell(d, mosaic_cell(d, 0));
    } else if (d->a.s_pending) {
        minitel_cell_t c = alpha_cell(d, 0x20);
        c.kind = MT_KIND_DELIM;
        c.bg   = d->a.s_bg;
        c.flags = (unsigned char)((d->a.invert ? MT_F_INVERT : 0)
                                | (d->a.s_mask ? MT_F_ZMASK : 0)
                                | (d->a.s_ul   ? MT_F_ZUNDER : 0));
        d->a.s_pending = 0;
        d->a.s_pending_other = 0;
        put_cell(d, c);
    } else {
        minitel_cell_t c = alpha_cell(d, 0x20);
        c.flags &= (unsigned char)~MT_F_FLASH;  /* "il ne clignote pas" */
        put_cell(d, c);
    }
    d->last_kind = LAST_G0;
    d->last_code = 0x20;
}

static void put_glyph(minitel_decoder_t *d, unsigned char glyph)
{
    put_cell(d, alpha_cell(d, glyph));
    d->last_kind = LAST_GLYPH;
    d->last_code = glyph;
}

static void put_g0(minitel_decoder_t *d, unsigned char b)
{
    if (b == 0x20) { put_space(d); return; }
    put_cell(d, alpha_cell(d, b));
    d->last_kind = LAST_G0;
    d->last_code = b;
}

/*
 * DEL (p98): "En position hors code, le code 0x7F est interprete
 * exactement comme le code 0x5F avec tous les attributs, y compris le
 * lignage. En position en code, il n'y a pas de lignage": a full block in
 * the ink colour, never underlined.
 */
static void put_del(minitel_decoder_t *d)
{
    if (d->a.g1) { put_g1(d, 0x5F); return; }
    {
        minitel_cell_t c = alpha_cell(d, MT_G_FULLBLOCK);
        put_cell(d, c);
        d->last_kind = LAST_GLYPH;
        d->last_code = MT_G_FULLBLOCK;
    }
}

static void put_printable(minitel_decoder_t *d, unsigned char b)
{
    if (b == 0x7F) { put_del(d); return; }
    if (b == 0x20) { put_space(d); return; }
    if (d->a.g1) put_g1(d, b);
    else put_g0(d, b);
}

/* ------------------------------------------------------------------ */
/* G2 (p88-90, schema 2.8)                                             */
/* ------------------------------------------------------------------ */

static int g2_glyph(unsigned char b)
{
    switch (b) {
    case 0x23: return MT_G_POUND;
    case 0x24: return '$';
    case 0x26: return '#';
    case 0x27: return MT_G_SECTION;
    case 0x2C: return MT_G_ARROW_L;
    case 0x2D: return MT_G_ARROW_U;
    case 0x2E: return MT_G_ARROW_R;
    case 0x2F: return MT_G_ARROW_D;
    case 0x30: return MT_G_DEGREE;
    case 0x31: return MT_G_PLUSMINUS;
    case 0x38: return MT_G_DIVIDE;
    case 0x3C: return MT_G_QUARTER;
    case 0x3D: return MT_G_HALF;
    case 0x3E: return MT_G_3QUARTER;
    case 0x6A: return MT_G_OE_UC;
    case 0x7A: return MT_G_OE_LC;
    case 0x7B: return MT_G_BETA;
    default: break;
    }
    return -1;
}

static int is_accent(unsigned char b)
{
    return b == 0x41 || b == 0x42 || b == 0x43 || b == 0x48 || b == 0x4B;
}

/* The accented repertoire of p90: lower-case only, VGP5. */
static int compose(int acc, unsigned char letter)
{
    switch (acc) {
    case 0x41:
        if (letter == 'a') return MT_G_A_GRAVE;
        if (letter == 'e') return MT_G_E_GRAVE;
        if (letter == 'u') return MT_G_U_GRAVE;
        break;
    case 0x42:
        if (letter == 'e') return MT_G_E_ACUTE;
        break;
    case 0x43:
        if (letter == 'a') return MT_G_A_CIRC;
        if (letter == 'e') return MT_G_E_CIRC;
        if (letter == 'i') return MT_G_I_CIRC;
        if (letter == 'o') return MT_G_O_CIRC;
        if (letter == 'u') return MT_G_U_CIRC;
        break;
    case 0x48:
        if (letter == 'a') return MT_G_A_DIAER;
        if (letter == 'e') return MT_G_E_DIAER;
        if (letter == 'i') return MT_G_I_DIAER;
        if (letter == 'o') return MT_G_O_DIAER;
        if (letter == 'u') return MT_G_U_DIAER;
        break;
    case 0x4B:
        if (letter == 'c') return MT_G_C_CEDIL;
        break;
    default: break;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* ESC + C1 attributes (schema 2.4, p91-94)                            */
/* ------------------------------------------------------------------ */

static void serial_def(minitel_decoder_t *d, int other)
{
    d->a.s_pending = 1;
    if (other) d->a.s_pending_other = 1;
}

static void do_attribute(minitel_decoder_t *d, unsigned char x)
{
    if (x >= 0x40 && x <= 0x47) { d->a.fg = (unsigned char)(x - 0x40); return; }
    if (x >= 0x50 && x <= 0x57) {
        d->a.s_bg = (unsigned char)(x - 0x50);
        serial_def(d, 0);
        return;
    }
    switch (x) {
    case 0x48: d->a.flash = 1; return;
    case 0x49: d->a.flash = 0; return;

    /* Sizes. Not taken in G1 (p91), and not on rows 00 or 01 (p93),
     * where "Le Minitel ne tient pas compte des sequences de double
     * taille et de double hauteur". Double width alone is allowed on row
     * 01; the service row takes no size at all. */
    case 0x4C: if (!d->a.g1) d->a.size = 0; return;
    case 0x4D:
        if (d->a.g1 || d->in_row0 || d->row < 2) { d->stats.filtered++; return; }
        d->a.size = MT_S_DH; return;
    case 0x4E:
        if (d->a.g1 || d->in_row0) { d->stats.filtered++; return; }
        d->a.size = MT_S_DW; return;
    case 0x4F:
        if (d->a.g1 || d->in_row0 || d->row < 2) { d->stats.filtered++; return; }
        d->a.size = MT_S_DW | MT_S_DH; return;

    case 0x58: d->a.s_mask = 1; serial_def(d, 1); return;
    case 0x5F: d->a.s_mask = 0; serial_def(d, 1); return;

    /* Lignage: underline, a serial attribute, in G0; separated mosaics,
     * a character attribute, in G1 (p91, p94). */
    case 0x59:
        if (d->a.g1) d->a.separated = 0;
        else { d->a.s_ul = 0; serial_def(d, 1); }
        return;
    case 0x5A:
        if (d->a.g1) d->a.separated = 1;
        else { d->a.s_ul = 1; serial_def(d, 1); }
        return;

    case 0x5C: if (!d->a.g1) d->a.invert = 0; return;
    case 0x5D:
        if (d->a.g1) { d->stats.filtered++; return; }
        d->a.invert = 1; return;

    default:
        /* 0x4A, 0x4B, 0x5E: free codes of the C1 grid, "filtrees" (p91). */
        d->stats.filtered++;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* C0                                                                  */
/* ------------------------------------------------------------------ */

static void do_c0(minitel_decoder_t *d, unsigned char b)
{
    switch (b) {
    case 0x07: d->stats.bells++; return;                 /* BEL, p98 */
    case 0x08: do_bs(d); return;
    case 0x09: do_ht(d); return;
    case 0x0A: do_lf(d); return;
    case 0x0B: do_vt(d); return;
    case 0x0C:                                           /* FF, p95-96 */
        clear_rows(d, 1, 24);
        leave_row0(d);
        d->row = 1; d->col = 1;
        separator_reset(d);
        d->ever = 1;
        return;
    case 0x0D: settle_col(d); d->col = 1; return;        /* CR */
    case 0x0E:                                           /* SO, p90-91 */
        d->a.g1 = 1;
        /* Size and inverse "le terminal les annulera definitivement meme
         * apres le retour en code" (p91); underline is reset "lors du
         * passage hors code" (p94). */
        d->a.size = 0;
        d->a.invert = 0;
        if (d->a.s_ul) { d->a.s_ul = 0; }
        return;
    case 0x0F:                                           /* SI */
        d->a.g1 = 0;
        d->a.separated = 0;          /* "Le disjoint est desactive lors du
                                        passage en code" (p94) */
        return;
    case 0x11: d->cursor_on = 1; return;                 /* Con, p99 */
    case 0x12: d->st = ST_REP; return;                   /* REP, p98 */
    case 0x13: d->st = ST_SKIP1; return;                 /* SEP X, p99 */
    case 0x14: d->cursor_on = 0; return;                 /* Coff */
    case 0x18: {                                         /* CAN, p95 */
        /* Fill to the end of the row with spaces of the current set in
         * the current attributes; the cursor does not move, and it is
         * NOT a delimiter. */
        int c, col = d->col > MINITEL_COLS ? MINITEL_COLS : d->col;
        int r = d->in_row0 ? 0 : d->row;
        minitel_cell_t t;
        if (d->a.g1) {
            t.kind = MT_KIND_MOSAIC; t.glyph = 0; t.fg = d->a.fg;
            t.bg = d->a.s_bg; t.size = 0;
            t.flags = d->a.separated ? MT_F_SEPARATED : 0;
        } else {
            t = alpha_cell(d, 0x20);
            t.size = 0;
            t.flags &= (unsigned char)~MT_F_FLASH;
        }
        for (c = col; c <= MINITEL_COLS; c++) d->mem[r][c] = t;
        d->ever = 1;
        return;
    }
    case 0x19:                                           /* SS2, p90 */
        if (d->a.g1) return;     /* "en mode semi-graphique, la reception
                                    du code SS2 est ignoree" */
        d->st = ST_SS2;
        return;
    case 0x1A:                                           /* SUB, p99 */
        d->stats.errors++;
        if (d->a.g1) put_g1(d, 0x5F);
        else put_glyph(d, MT_G_FULLBLOCK);
        return;
    case 0x1B: d->st = ST_ESC; return;
    case 0x1D: d->st = ST_SKIP1; return;                 /* SS3 X, p99 */
    case 0x1E:                                           /* RS, p95 */
        leave_row0(d);
        d->row = 1; d->col = 1;
        separator_reset(d);
        return;
    case 0x1F: d->st = ST_US1; return;                   /* US, p96 */
    default:
        /* "Tous les codes de commande non retenus dans le jeu C0 n'ont
         * aucune action sur le Minitel" (p87). */
        return;
    }
}

/* ------------------------------------------------------------------ */
/* US (p96-97)                                                         */
/* ------------------------------------------------------------------ */

static void do_us(minitel_decoder_t *d, unsigned char r, unsigned char c)
{
    int row, col;

    /*
     * "Si les octets correspondant a ces deux caracteres appartiennent
     * tous deux aux colonnes 4 a 7, ils representent RESPECTIVEMENT ...
     * le numero de RANGEE et le numero de COLONNE" (p96). ROW FIRST. Its
     * own example, US 0x45 0x59 "A", puts the A at row 05 column 25. A
     * decoder that reads the pair the other way round draws every page
     * of every service on the wrong rows; G-M4 found exactly that in
     * eMinitel 0.2.b, so the order is a named trap in the self-test.
     */
    if (r < 0x40 || c < 0x40) { d->stats.us_ignored++; return; }
    row = r - 0x40;
    col = c - 0x40;
    if (row > 24 || col < 1 || col > MINITEL_COLS) {
        d->stats.us_ignored++;       /* "la fonction US est ignoree" */
        return;
    }

    if (row == 0) {
        /* Only US 0x40 X/Y reaches row 00 (p97). Save the position and
         * the attributes of the moment for LF to restore. */
        if (!d->in_row0) {
            d->sv_row = d->row;
            d->sv_col = d->col > MINITEL_COLS ? MINITEL_COLS : d->col;
            d->sv_a   = d->a;
        }
        d->in_row0 = 1;
    } else {
        leave_row0(d);
    }
    d->row = row;
    d->col = col;
    separator_reset(d);
}

/* ------------------------------------------------------------------ */
/* CSI (p94-95, p99)                                                   */
/* ------------------------------------------------------------------ */

static int csi_arg(const minitel_decoder_t *d, int i, int dflt)
{
    if (i >= d->csi_np || d->csi_p[i] < 0) return dflt;
    return d->csi_p[i];
}

static void do_csi(minitel_decoder_t *d, unsigned char final)
{
    int n, r, c, p0;

    if (d->in_row0 || d->csi_bad || d->csi_priv) {
        /* "Les fonctions de type CSI ne sont pas disponibles en rangee
         * 00" (p95); unknown and private forms are filtered (p99). */
        d->stats.filtered++;
        return;
    }
    settle_col(d);
    n  = csi_arg(d, 0, 1);
    if (n < 1) n = 1;
    p0 = csi_arg(d, 0, 0);

    switch (final) {
    case 'A': d->row -= n; if (d->row < 1) d->row = 1; return;
    case 'B': d->row += n; if (d->row > 24) d->row = 24; return;
    case 'C': d->col += n; if (d->col > MINITEL_COLS) d->col = MINITEL_COLS; return;
    case 'D': d->col -= n; if (d->col < 1) d->col = 1; return;
    case 'H':
        r = csi_arg(d, 0, 1); c = csi_arg(d, 1, 1);
        if (r < 1 || r > 24 || c < 1 || c > MINITEL_COLS) { d->stats.filtered++; return; }
        d->row = r; d->col = c;
        return;
    case 'J':
        if (p0 == 0) { clear_row(d, d->row, d->col, MINITEL_COLS); if (d->row < 24) clear_rows(d, d->row + 1, 24); }
        else if (p0 == 1) { if (d->row > 1) clear_rows(d, 1, d->row - 1); clear_row(d, d->row, 1, d->col); }
        else if (p0 == 2) clear_rows(d, 1, 24);
        else d->stats.filtered++;
        d->ever = 1;
        return;
    case 'K':
        if (p0 == 0) clear_row(d, d->row, d->col, MINITEL_COLS);
        else if (p0 == 1) clear_row(d, d->row, 1, d->col);
        else if (p0 == 2) clear_row(d, d->row, 1, MINITEL_COLS);
        else d->stats.filtered++;
        d->ever = 1;
        return;
    case 'P':                     /* delete n characters within the row */
        if (n > MINITEL_COLS - d->col + 1) n = MINITEL_COLS - d->col + 1;
        memmove(&d->mem[d->row][d->col], &d->mem[d->row][d->col + n],
                (size_t)(MINITEL_COLS - d->col + 1 - n) * sizeof(minitel_cell_t));
        clear_row(d, d->row, MINITEL_COLS - n + 1, MINITEL_COLS);
        return;
    case '@':                     /* insert n characters, no overflow */
        if (n > MINITEL_COLS - d->col + 1) n = MINITEL_COLS - d->col + 1;
        memmove(&d->mem[d->row][d->col + n], &d->mem[d->row][d->col],
                (size_t)(MINITEL_COLS - d->col + 1 - n) * sizeof(minitel_cell_t));
        clear_row(d, d->row, d->col, d->col + n - 1);
        return;
    case 'M': if (n > 25 - d->row) n = 25 - d->row; scroll_up(d, d->row, 24, n); return;
    case 'L': if (n > 25 - d->row) n = 25 - d->row; scroll_down(d, d->row, 24, n); return;
    case 'h': if (p0 == 4) d->insert = 1; else d->stats.filtered++; return;
    case 'l': if (p0 == 4) d->insert = 0; else d->stats.filtered++; return;
    default:
        d->stats.filtered++;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Protocol (p134-145): parsed and swallowed                           */
/* ------------------------------------------------------------------ */

static void do_pro(minitel_decoder_t *d)
{
    d->stats.protocol++;
    if (d->pro_n == 1) {
        if (d->pro[0] == 0x7B) {
            /* ENQROM (p139): SOH, three identification bytes, EOT. The
             * three are this terminal's own choosing, "MGS" for the MGT
             * Suite; a service must never depend on them. */
            static const unsigned char id[] = { 0x01, 'M', 'G', 'S', 0x04 };
            send_reply(d, id, sizeof id);
        } else if (d->pro[0] == 0x7F) {
            /* PRO1 RESET (p145): 40 columns, cursor off, default
             * attributes, page mode; the screen is NOT cleared. */
            attr_default(&d->a);
            d->cursor_on = 0;
            d->scroll = 0;
            d->insert = 0;
        }
    } else if (d->pro_n == 2 && d->pro[1] == 0x43) {
        /* PRO2 START/STOP ROULEAU (p105): scroll mode on or off. The
         * acknowledgement a real terminal returns is NOT sent; a terminal
         * is not obliged to be believed, and the estate's own service
         * sends no Protocol at all. */
        if (d->pro[0] == 0x69) d->scroll = 1;
        else if (d->pro[0] == 0x6A) d->scroll = 0;
    }
}

/* ------------------------------------------------------------------ */
/* The byte loop                                                       */
/* ------------------------------------------------------------------ */

static void enter_transparency(minitel_decoder_t *d)
{
    d->st = ST_TRANSP;
}

static void feed_byte(minitel_decoder_t *d, unsigned char b)
{
    /* Screen transparency (p99): everything is ignored until ESC 0x25
     * 0x40 or ESC 0x2F 0x3F, and "Il n'y a pas resynchronisation sur les
     * colonnes 0 et 1 en transparence ecran". A NAPLPS frame sent to a
     * Minitel lands here, since ESC 0x25 is its introducer. */
    switch (d->st) {
    case ST_TRANSP:
        if (b == 0x1B) d->st = ST_TRANSP_ESC;
        d->stats.filtered++;
        return;
    case ST_TRANSP_ESC:
        if (b == 0x25) d->st = ST_TRANSP_ESC25;
        else if (b == 0x2F) d->st = ST_TRANSP_ESC2F;
        else if (b != 0x1B) d->st = ST_TRANSP;
        return;
    case ST_TRANSP_ESC25:
        if (b == 0x40) d->st = ST_GROUND;
        else d->st = (b == 0x1B) ? ST_TRANSP_ESC : ST_TRANSP;
        return;
    case ST_TRANSP_ESC2F:
        if (b == 0x3F) d->st = ST_GROUND;
        else d->st = (b == 0x1B) ? ST_TRANSP_ESC : ST_TRANSP;
        return;
    default: break;
    }

    /* NUL is padding "a n'importe quel moment, y compris dans le courant
     * de sequences" (p98). */
    if (b == 0x00) return;

    /* RESYNCHRONISATION (p99): a C0 inside an unfinished US, ESC, REP,
     * SS2, SEP, SS3, CSI or Protocol sequence abandons the sequence and
     * is itself executed. */
    if (b < 0x20 && d->st != ST_GROUND) {
        d->stats.resync++;
        d->st = ST_GROUND;
    }

    switch (d->st) {
    case ST_GROUND:
        if (b < 0x20) do_c0(d, b);
        else put_printable(d, b);
        return;

    case ST_ESC:
        d->st = ST_GROUND;
        if (b >= 0x40 && b <= 0x5F && b != 0x5B) { do_attribute(d, b); return; }
        if (b == 0x5B) {
            d->st = ST_CSI;
            d->csi_np = 0; d->csi_have = 0; d->csi_bad = 0; d->csi_priv = 0;
            d->csi_p[0] = d->csi_p[1] = d->csi_p[2] = d->csi_p[3] = -1;
            return;
        }
        if (b == 0x25) { enter_transparency(d); return; }
        if (b >= 0x20 && b <= 0x2F) {
            d->st = ST_ISO; d->iso_first = b; d->iso_n = 1; d->iso[0] = b;
            return;
        }
        if (b == 0x35 || b == 0x36 || b == 0x37) { d->st = ST_SKIP1; return; }
        if (b == 0x39 || b == 0x3A || b == 0x3B) {
            d->st = ST_PRO; d->pro_n = 0; d->pro_left = b - 0x38;
            return;
        }
        if (b == 0x61) {
            /* Cursor position request (p98): US row col, row first. */
            unsigned char rep[3];
            int col = d->col > MINITEL_COLS ? MINITEL_COLS : d->col;
            rep[0] = 0x1F;
            rep[1] = (unsigned char)(0x40 + (d->in_row0 ? 0 : d->row));
            rep[2] = (unsigned char)(0x40 + col);
            send_reply(d, rep, 3);
            d->stats.protocol++;
            return;
        }
        d->stats.filtered++;          /* undefined ESC X (p99) */
        return;

    case ST_ISO:
        /* ESC 2x 2y ... F (ISO 2022), filtered (p99), except the
         * full-screen mask and unmask, ESC 0x23 0x20 0x58 / 0x5F (p94). */
        if (b >= 0x20 && b <= 0x2F) {
            if (d->iso_n < 4) d->iso[d->iso_n] = b;
            d->iso_n++;
            return;
        }
        d->st = ST_GROUND;
        if (d->iso_n == 2 && d->iso[0] == 0x23 && d->iso[1] == 0x20) {
            if (b == 0x58) { d->screen_mask = 1; return; }
            if (b == 0x5F) { d->screen_mask = 0; return; }
        }
        d->stats.filtered++;
        return;

    case ST_CSI:
        if (b >= 0x30 && b <= 0x39) {
            if (d->csi_np == 0) d->csi_np = 1;
            if (d->csi_p[d->csi_np - 1] < 0) d->csi_p[d->csi_np - 1] = 0;
            if (d->csi_p[d->csi_np - 1] < 1000)
                d->csi_p[d->csi_np - 1] = d->csi_p[d->csi_np - 1] * 10 + (b - 0x30);
            return;
        }
        if (b == 0x3B) {
            if (d->csi_np == 0) d->csi_np = 1;
            if (d->csi_np < 4) d->csi_np++;
            else d->csi_bad = 1;
            return;
        }
        if (b >= 0x3C && b <= 0x3F) { d->csi_priv = 1; return; }
        if (b == 0x3A) { d->csi_bad = 1; return; }
        if (b >= 0x20 && b <= 0x2F) { d->csi_bad = 1; return; }   /* intermediates */
        d->st = ST_GROUND;
        do_csi(d, b);
        return;

    case ST_US1:
        d->us_r = b; d->st = ST_US2;
        return;
    case ST_US2:
        d->st = ST_GROUND;
        do_us(d, d->us_r, b);
        return;

    case ST_REP:
        d->st = ST_GROUND;
        if (b < 0x40) { d->stats.filtered++; return; }   /* not conforming */
        {
            int n = b & 0x3F, i;
            for (i = 0; i < n; i++) {
                switch (d->last_kind) {
                case LAST_G0:
                    if (d->last_code == 0x20) put_space(d);
                    else { put_cell(d, alpha_cell(d, d->last_code)); }
                    break;
                case LAST_G1: put_g1(d, d->last_code); break;
                case LAST_GLYPH: put_glyph(d, d->last_code); break;
                default: break;
                }
            }
        }
        return;

    case ST_SS2:
        d->st = ST_GROUND;
        if (is_accent(b)) { d->acc = b; d->st = ST_SS2_ACC; return; }
        {
            int g = g2_glyph(b);
            if (g < 0) { g = MT_G_LOWBAR; d->stats.unrepresent++; }
            put_glyph(d, (unsigned char)g);
        }
        return;

    case ST_SS2_ACC:
        d->st = ST_GROUND;
        {
            /* "l'adjonction d'un accent ... a un caractere graphique ...
             * ne faisant pas partie du repertoire, conduit a l'affichage
             * du seul caractere graphique" (p90). */
            int g = compose(d->acc, b);
            if (g >= 0) put_glyph(d, (unsigned char)g);
            else put_printable(d, b);
        }
        return;

    case ST_SKIP1:
        d->st = ST_GROUND;
        d->stats.filtered++;
        return;

    case ST_PRO:
        if (d->pro_n < 4) d->pro[d->pro_n] = b;
        d->pro_n++;
        if (--d->pro_left <= 0) { d->st = ST_GROUND; do_pro(d); }
        return;

    default:
        d->st = ST_GROUND;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

minitel_decoder_t *minitel_create(void)
{
    minitel_decoder_t *d = (minitel_decoder_t *)calloc(1, sizeof *d);
    if (d) minitel_reset(d);
    return d;
}

void minitel_destroy(minitel_decoder_t *d)
{
    free(d);
}

void minitel_reset(minitel_decoder_t *d)
{
    minitel_reply_fn fn = d->reply;
    void *ctx = d->reply_ctx;
    char letter = d->status_letter;

    memset(d, 0, sizeof *d);
    d->reply = fn;
    d->reply_ctx = ctx;
    d->status_letter = letter;

    clear_rows(d, 0, 24);
    attr_default(&d->a);
    d->row = 1;
    d->col = 1;
    d->cursor_on = 0;
    d->screen_mask = 1;
    d->scroll = 0;
    d->st = ST_GROUND;
}

void minitel_feed(minitel_decoder_t *d, const unsigned char *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned char b = buf[i];
        d->stats.bytes_in++;
        /* The line carries 7 useful bits and an even parity bit (p81). A
         * TCP service sends bit 8 clear; a byte with it set is folded to
         * its 7 bits and counted rather than guessed at. */
        if (b & 0x80) { d->stats.bit8++; b &= 0x7F; }
        feed_byte(d, b);
    }
}

void minitel_set_reply(minitel_decoder_t *d, minitel_reply_fn fn, void *ctx)
{
    d->reply = fn;
    d->reply_ctx = ctx;
}

void minitel_set_status_letter(minitel_decoder_t *d, char letter)
{
    d->status_letter = letter;
}

char minitel_get_status_letter(const minitel_decoder_t *d)
{
    return d->status_letter;
}

const minitel_cell_t *minitel_cell(const minitel_decoder_t *d, int row, int col)
{
    if (row < 0) row = 0;
    if (row > 24) row = 24;
    if (col < 1) col = 1;
    if (col > MINITEL_COLS) col = MINITEL_COLS;
    return &d->mem[row][col];
}

const minitel_stats_t *minitel_get_stats(const minitel_decoder_t *d)
{
    return &d->stats;
}

int minitel_cursor_row(const minitel_decoder_t *d)
{
    return d->in_row0 ? 0 : d->row;
}

int minitel_cursor_col(const minitel_decoder_t *d)
{
    return d->col > MINITEL_COLS ? MINITEL_COLS : d->col;
}

int minitel_cursor_visible(const minitel_decoder_t *d) { return d->cursor_on; }
int minitel_screen_masked(const minitel_decoder_t *d)  { return d->screen_mask; }
int minitel_scroll_mode(const minitel_decoder_t *d)    { return d->scroll; }
int minitel_ever_drew(const minitel_decoder_t *d)      { return d->ever; }
