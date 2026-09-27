/*
 * naplps_decode.c - NAPLPS (videotex) presentation-layer decoder.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from ANSI X3.110-1983 / CSA T500-1983. No Win32, no GDI.
 * The full provenance statement, including the disclosed cross-check
 * oracles, is at the top of naplps_decode.h and is the authoritative one.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "naplps_decode.h"
#include "naplps_font.h"

/* ------------------------------------------------------------------ */
/* Fixed point                                                         */
/* ------------------------------------------------------------------ */

/*
 * Unit-screen coordinates are held as 16.16 fixed point: FX_ONE is the
 * coordinate 1.0. The standard's coordinates run from 0 inclusive to 1
 * noninclusive (4.2.2) and are signed two's complement fractions, so a
 * signed 32-bit value covers the whole legal range with room to spare
 * for relative arithmetic that temporarily leaves the unit screen.
 *
 * One pixel is 1/256 of the unit screen on both axes (see
 * naplps_surface.h), so pixels are fx >> 8.
 */
#define FX_BITS      16
#define FX_ONE       (1L << FX_BITS)
#define FX_TO_PX(v)  ((int)((v) >> (FX_BITS - 8)))

/*
 * The visible height. Appendix D Table D1 puts the visible Y range at
 * 0.0 to 0.78125, which is exactly 200/256, so the top of the display
 * area is at fx 51200. Anything drawn above this is legal and invisible
 * (4.2.2: "it is always permissible to draw anywhere on the unit screen"
 * but only the part coinciding with the display area is visible).
 */
#define VIS_Y_FX     ((long)NAPLPS_H * FX_ONE / NAPLPS_UNIT)

/* The five C1 text sizes, 6.2.7.6 to 6.2.7.10, as exact fractions. */
#define CF_SMALL_DX   (FX_ONE / 80)
#define CF_SMALL_DY   (FX_ONE * 5 / 128)
#define CF_MEDIUM_DX  (FX_ONE / 32)
#define CF_MEDIUM_DY  (FX_ONE * 3 / 64)
#define CF_NORMAL_DX  (FX_ONE / 40)
#define CF_NORMAL_DY  (FX_ONE * 5 / 128)
#define CF_DBLHI_DX   (FX_ONE / 40)
#define CF_DBLHI_DY   (FX_ONE * 5 / 64)
#define CF_DBLSZ_DX   (FX_ONE / 20)
#define CF_DBLSZ_DY   (FX_ONE * 5 / 64)

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* Which set is designated into a G slot. */
typedef enum {
    GSET_PRIMARY = 0, GSET_SUPP, GSET_PDI, GSET_MOSAIC,
    GSET_MACRO, GSET_DRCS, GSET_NULL
} gset_t;

/* ESC state machine, 4.3.2. */
typedef enum { ESC_NONE = 0, ESC_SEEN } escstate_t;

/* Pending multi-byte C0 operand. */
typedef enum { PEND_NONE = 0, PEND_APS, PEND_NSR, PEND_REPEAT } pend_t;

#define OPBUF_MAX 4096
#define MAX_PTS   512        /* the standard requires at least 256      */

struct naplps_decoder {
    naplps_surface_t surf;
    naplps_stats_t   st;
    naplps_mode_t    mode;
    int              ever_drew;
    int              wait_tenths;

    /* Code extension state, 4.3 */
    gset_t      gset[4];
    int         gl;             /* G slot invoked into GL (cols 2-7)    */
    int         gr;             /* G slot invoked into GR (cols 10-15)  */
    int         single_shift;   /* -1, or the G slot for the next byte  */

    /*
     * 7-bit or 8-bit environment, 4.3.2 and 4.3.3.
     *
     * 4.3.1.4 says the choice "may be explicitly established or changed
     * for a particular service, or may be implicitly established by prior
     * agreement" - so there is NO in-band declaration to read, and a
     * viewer opening a file it was handed has to decide for itself.
     *
     * The rule here: assume the 7-bit environment, and switch to 8-bit
     * the first time a byte arrives with bit 8 set. That is safe in both
     * directions. A genuinely 7-bit stream never sets bit 8, so it never
     * switches. A genuinely 8-bit stream's leading bytes, before its
     * first high byte, are C0 controls and escape sequences that decode
     * identically in either environment, so nothing is lost by having
     * read them as 7-bit.
     *
     * The alternative, masking bit 8 off unconditionally, is what the
     * ALEXtel's own Even/7/1 frame would require (README.PP3 6.2) and is
     * wrong for a local file: it silently turns an 8-bit frame into
     * garbage rather than failing visibly.
     */
    int         eightbit;
    escstate_t  escstate;
    unsigned char escbuf[8];
    int         esclen;

    /* Deliberately-skipped definition bodies (macros, DRCS, texture
     * masks). 8.3.2 requires unimplemented features to be skipped
     * SAFELY, and a macro body is presentation code that would otherwise
     * execute as garbage, so it is swallowed until a terminating C1. */
    int         swallow;

    pend_t      pend;
    unsigned char pendbuf[2];
    int         pendlen;

    /* DOMAIN state, 5.3.2.2 */
    int         sv_len;         /* 1..4 bytes  */
    int         mv_len;         /* 1..8 bytes  */
    int         dims;           /* 2 or 3      */
    long        pel_dx, pel_dy; /* logical pel, fx */

    /* Drawing state */
    long        dpx, dpy;       /* drawing point, fx */
    int         color_mode;     /* 0, 1 or 2 */
    naplps_color_t draw_value;  /* mode 0: the colour itself */
    int         draw_addr;      /* the colour map address in use */
    int         bg_addr;        /* mode 2 only */
    int         cmap_used[NAPLPS_CMAP_SIZE];
    int         addr_black, addr_white;

    /* Texture, 5.3.2.4 */
    naplps_mask_t  area_mask;       /* built per fill by area_mask()   */
    int            area_pattern;    /* Table 13, bits b6 b5 b4         */
    unsigned short line_texture;

    /* Text state */
    long        cx, cy;         /* cursor: character field origin, fx  */
    long        cf_dx, cf_dy;   /* character field dimensions, fx      */
    int         rotation;       /* 0, 90, 180, 270                     */
    int         path;           /* 0 right, 1 left, 2 up, 3 down       */
    int         ics_num, ics_den;  /* intercharacter spacing           */
    int         prop_spacing;      /* Table 8 pattern 11               */
    int         irs_num, irs_den;  /* interrow spacing                 */
    int         reverse_video;
    int         underline;
    int         word_wrap;
    int         scroll;
    int         cursor_lead, dp_lead;
    unsigned char last_spacing_char;
    int         have_last_spacing;

    /*
     * The ACTIVE FIELD, 5.3.3.6.2. Its first listed purpose is
     * "columnated text", which is ordinary text layout and squarely in
     * scope; the unprotected-field and INCREMENTAL POINT uses of the same
     * rectangle are not implemented. Held normalised with the origin at
     * the lower left and positive extents, because FIELD itself allows
     * either sign "so that the origin point may be placed in any of the
     * four corners of the field".
     */
    long        af_x, af_y, af_w, af_h;
    int         af_set;

    /* PDI accumulation, 5.3.1.1 and 5.3.2.2.5 */
    int         pdi_op;         /* -1 when no sequence is open */
    unsigned char opbuf[OPBUF_MAX];
    int         oplen;
    int         reps;           /* executions so far in this sequence */

    naplps_trace_fn trace;
    void           *trace_ctx;
};

void naplps_set_trace(naplps_decoder_t *d, naplps_trace_fn fn, void *ctx)
{
    if (!d) return;
    d->trace = fn;
    d->trace_ctx = ctx;
}

/* ------------------------------------------------------------------ */
/* Defaults                                                            */
/* ------------------------------------------------------------------ */

static void find_bw(naplps_decoder_t *d)
{
    int i;
    d->addr_black = 0;
    d->addr_white = NAPLPS_CMAP_SIZE - 1;
    for (i = 0; i < NAPLPS_CMAP_SIZE; i++) {
        const naplps_color_t *c = &d->surf.cmap[i];
        if (c->g == 0 && c->r == 0 && c->b == 0) { d->addr_black = i; break; }
    }
    for (i = 0; i < NAPLPS_CMAP_SIZE; i++) {
        const naplps_color_t *c = &d->surf.cmap[i];
        if (c->g == NAPLPS_PRIMARY_MAX && c->r == NAPLPS_PRIMARY_MAX &&
            c->b == NAPLPS_PRIMARY_MAX) { d->addr_white = i; break; }
    }
}

static void reset_domain(naplps_decoder_t *d)
{
    /* Table 28 / 5.3.2.2: single-value 1 byte, multi-value 3 bytes,
     * two dimensions, logical pel dx = 0 dy = 0. */
    d->sv_len = 1;
    d->mv_len = 3;
    d->dims   = 2;
    d->pel_dx = 0;
    d->pel_dy = 0;
}

static void reset_field(naplps_decoder_t *d)
{
    /* "The default active field is the unit screen." */
    d->af_x = 0; d->af_y = 0;
    d->af_w = FX_ONE; d->af_h = FX_ONE;
    d->af_set = 0;
}

static void reset_text(naplps_decoder_t *d)
{
    reset_field(d);
    d->cf_dx = CF_NORMAL_DX;
    d->cf_dy = CF_NORMAL_DY;
    d->rotation = 0;
    d->path = 0;
    d->ics_num = 1; d->ics_den = 1;
    d->prop_spacing = 0;
    d->irs_num = 1; d->irs_den = 1;
    d->reverse_video = 0;
    d->underline = 0;
    d->word_wrap = 0;    /* WORD WRAP OFF is the default, 6.2.7.12 */
    d->scroll = 0;       /* SCROLL OFF is the default, 6.2.7.14    */
    d->cursor_lead = 0;
    d->dp_lead = 0;
    d->have_last_spacing = 0;
    /* Home: upper left, with the TOP of the character field on the top
     * boundary of the display area (6.1.2.8). */
    d->cx = 0;
    d->cy = VIS_Y_FX - (d->cf_dy < 0 ? -d->cf_dy : d->cf_dy);
}

static void reset_texture(naplps_decoder_t *d)
{
    naplps_mask_solid(&d->area_mask);
    d->area_pattern = 0;
    d->line_texture = 0xFFFF;
}

static void reset_color(naplps_decoder_t *d, int to_default_map)
{
    int i;
    if (to_default_map) {
        naplps_surface_default_cmap(&d->surf);
        for (i = 0; i < NAPLPS_CMAP_SIZE; i++) d->cmap_used[i] = 0;
        find_bw(d);
    }
    d->color_mode = 0;
    d->draw_value.g = NAPLPS_PRIMARY_MAX;
    d->draw_value.r = NAPLPS_PRIMARY_MAX;
    d->draw_value.b = NAPLPS_PRIMARY_MAX;
    d->draw_addr = d->addr_white;
    d->bg_addr = d->addr_black;
}

void naplps_reset(naplps_decoder_t *d)
{
    if (!d) return;

    memset(&d->st, 0, sizeof(d->st));
    d->mode = NAPLPS_MODE_NAPLPS;
    d->ever_drew = 0;
    d->wait_tenths = 0;

    /* 4.3.1.3: "In the default state GO contains the primary character
     * set, G1 the POI set, G2 the supplementary character set, and G3
     * the mosaic set." */
    d->gset[0] = GSET_PRIMARY;
    d->gset[1] = GSET_PDI;
    d->gset[2] = GSET_SUPP;
    d->gset[3] = GSET_MOSAIC;
    /* 4.3.3: "G0 is invoked into GL by default as in the 7-bit
     * environments. In addition, G1 is invoked into GR." */
    d->gl = 0;
    d->gr = 1;
    d->eightbit = 0;
    d->single_shift = -1;
    d->escstate = ESC_NONE;
    d->esclen = 0;
    d->swallow = 0;
    d->pend = PEND_NONE;
    d->pendlen = 0;

    naplps_surface_init(&d->surf);
    find_bw(d);
    reset_domain(d);
    reset_texture(d);
    reset_color(d, 1);
    reset_text(d);

    d->dpx = 0;
    d->dpy = 0;
    d->pdi_op = -1;
    d->oplen = 0;
}

naplps_decoder_t *naplps_create(void)
{
    naplps_decoder_t *d = (naplps_decoder_t *)calloc(1, sizeof(*d));
    if (d) naplps_reset(d);
    return d;
}

void naplps_destroy(naplps_decoder_t *d) { free(d); }

const naplps_surface_t *naplps_get_surface(const naplps_decoder_t *d)
{ return d ? &d->surf : NULL; }

const naplps_stats_t *naplps_get_stats(const naplps_decoder_t *d)
{ return d ? &d->st : NULL; }

naplps_mode_t naplps_get_mode(const naplps_decoder_t *d)
{ return d ? d->mode : NAPLPS_MODE_ASCII; }

int naplps_ever_drew(const naplps_decoder_t *d)
{ return d ? d->ever_drew : 0; }

int naplps_take_wait(naplps_decoder_t *d)
{
    int t;
    if (!d) return 0;
    t = d->wait_tenths;
    d->wait_tenths = 0;
    return t;
}

/* ------------------------------------------------------------------ */
/* Operand unpacking                                                   */
/* ------------------------------------------------------------------ */

/*
 * A multi-value coordinate operand, Figure 11.
 *
 * The six payload bits of each byte are SPLIT BETWEEN THE AXES, not
 * concatenated per axis: in two-dimensional mode b6 b5 b4 belong to X
 * and b3 b2 b1 belong to Y, most significant byte first. So an L-byte
 * operand gives each axis 3L bits. In three-dimensional mode the split is
 * two bits each for X, Y and Z, and "If three-dimensional coordinates
 * are received, the Z coordinate is to be ignored" (5.3.2.2.4).
 *
 * "All coordinate operands are interpreted as signed, two's complement
 * numbers, ie, binary decimals where the MSB represents the digit just
 * to the right of the decimal point." So the raw n-bit field is a signed
 * fraction in [-1, 1), and scaling it to 16.16 is a shift.
 */
static long unpack_frac(unsigned long raw, int nbits)
{
    long v;
    int shift;

    if (nbits <= 0) return 0;

    /* Sign-extend the two's complement field. */
    if (raw & (1UL << (nbits - 1)))
        v = (long)raw - (long)(1UL << nbits);
    else
        v = (long)raw;

    /*
     * SCALE. The divisor is 2^(nbits-1), not 2^nbits.
     *
     * The Definitions clause is decisive and says it twice, for absolute
     * and for relative coordinates alike: they are "signed numbers
     * between -1 (inclusive) and 1 (noninclusive) ... in two's complement
     * arithmetic". An n-bit two's complement field spanning [-1, 1) has
     * its binary point immediately after the SIGN bit, so the full-scale
     * magnitude is 2^(n-1).
     *
     * 5.3.1.1's wording, "binary decimals where the MSB represents the
     * digit just to the right of the decimal point", reads at first as if
     * the divisor were 2^n, which would confine positive coordinates to
     * [0, 0.5) and shrink every picture into the bottom-left quadrant of
     * the screen while leaving it otherwise perfectly correct. That is a
     * silent wrong-picture bug of exactly the class 8.4 of the Phase 1
     * report warns about, and it is caught only by looking at a rendered
     * frame, never by checking segmentation. The stated RANGE settles the
     * reading.
     */
    shift = FX_BITS - (nbits - 1);
    if (shift >= 0) return v << shift;
    return v >> (-shift);
}

static void unpack_multi(const naplps_decoder_t *d, const unsigned char *p,
                         long *ox, long *oy)
{
    unsigned long xr = 0, yr = 0;
    int i, per;

    per = (d->dims == 3) ? 2 : 3;

    for (i = 0; i < d->mv_len; i++) {
        unsigned int v = p[i] & 0x3F;          /* the six payload bits */
        if (d->dims == 3) {
            xr = (xr << 2) | ((v >> 4) & 0x03);
            yr = (yr << 2) | ((v >> 2) & 0x03);
            /* the low two bits are Z and are ignored */
        } else {
            xr = (xr << 3) | ((v >> 3) & 0x07);
            yr = (yr << 3) | (v & 0x07);
        }
    }

    *ox = unpack_frac(xr, per * d->mv_len);
    *oy = unpack_frac(yr, per * d->mv_len);
}

/*
 * A multi-value operand used as a COLOUR VALUE, Figures 12 and 23.
 *
 * "Each byte contains two three-tuples. Each three-tuple contains one bit
 * for each of the three primary colors. These are specified in the order
 * green, red, blue, which is the order of decreasing luminance." So each
 * byte carries G R B G R B from b6 down to b1, and an L-byte operand
 * gives each primary 2L bits, most significant first.
 */
static void unpack_color(const naplps_decoder_t *d, const unsigned char *p,
                         naplps_color_t *out)
{
    unsigned long g = 0, r = 0, b = 0;
    int i, nbits;

    for (i = 0; i < d->mv_len; i++) {
        unsigned int v = p[i] & 0x3F;
        g = (g << 2) | (((v >> 5) & 1) << 1) | ((v >> 2) & 1);
        r = (r << 2) | (((v >> 4) & 1) << 1) | ((v >> 1) & 1);
        b = (b << 2) | (((v >> 3) & 1) << 1) | (v & 1);
    }
    nbits = 2 * d->mv_len;

    /*
     * The map stores 3 bits per primary (M = 9). 5.3.2.5.1: if the
     * operand carries more bits than the map holds, "the operand is
     * truncated and only the most significant bits are used"; if fewer,
     * "trailing zero bits are supplied".
     */
    if (nbits >= 3) {
        out->g = (unsigned char)((g >> (nbits - 3)) & 7);
        out->r = (unsigned char)((r >> (nbits - 3)) & 7);
        out->b = (unsigned char)((b >> (nbits - 3)) & 7);
    } else {
        out->g = (unsigned char)((g << (3 - nbits)) & 7);
        out->r = (unsigned char)((r << (3 - nbits)) & 7);
        out->b = (unsigned char)((b << (3 - nbits)) & 7);
    }
}

/*
 * A single-value operand, Figure 10: the six payload bits of each byte
 * concatenated, b6 to b1, most significant byte first, as an unsigned
 * integer.
 */
static unsigned long unpack_single(const unsigned char *p, int len)
{
    unsigned long v = 0;
    int i;
    for (i = 0; i < len; i++) v = (v << 6) | (unsigned long)(p[i] & 0x3F);
    return v;
}

/*
 * A colour map ADDRESS out of a single-value operand.
 *
 * 5.3.2.6.1, and this is the rule that silently mis-addresses the whole
 * colour map if it is missed: "the number of bits required to specify the
 * color map address is left justified within the single-value operand.
 * For example, for the default single-value operand length of one byte
 * and a four-bit color map address (N=4), the receiving presentation
 * process responds to b6 through b3 and ignores b2 and b1". So it is the
 * TOP N bits of the 6*len payload bits, not the bottom N.
 */
static int unpack_addr(const unsigned char *p, int len)
{
    unsigned long v = unpack_single(p, len);
    int nbits = 6 * len;
    if (nbits >= NAPLPS_CMAP_N)
        return (int)((v >> (nbits - NAPLPS_CMAP_N)) & (NAPLPS_CMAP_SIZE - 1));
    return (int)((v << (NAPLPS_CMAP_N - nbits)) & (NAPLPS_CMAP_SIZE - 1));
}

/* ------------------------------------------------------------------ */
/* Colour                                                             */
/* ------------------------------------------------------------------ */

/*
 * Colour mode 0's SET COLOR, 5.3.2.5.1. Mode 0 names a colour directly,
 * but it "also uses the color map": the decoder must find or allocate an
 * entry holding that colour.
 *
 *   "If the color specified in the color mode 0 SET COLOR command has
 *    already been specified in the color map, then the address of the
 *    drawing color is set to the lowest address containing that color and
 *    the color map is not changed. If that color has not already been
 *    specified in the color map, color mode 0 makes use of the lowest
 *    address that has not been used ... since the last RESET command that
 *    reestablishes the default color map, and that is not the address of
 *    nominal black or nominal white. If no addresses are available, then
 *    the color map shall not be changed and the drawing color is
 *    established in an implementation-dependent manner."
 *
 * The implementation-dependent fallback here is to leave the drawing
 * colour where it was, which is the least surprising of the options: it
 * draws in the previous colour rather than in an arbitrary one.
 */
static void mode0_set_color(naplps_decoder_t *d, const naplps_color_t *c)
{
    int i;

    d->draw_value = *c;

    for (i = 0; i < NAPLPS_CMAP_SIZE; i++) {
        const naplps_color_t *e = &d->surf.cmap[i];
        if (e->g == c->g && e->r == c->r && e->b == c->b) {
            d->draw_addr = i;
            return;
        }
    }

    for (i = 0; i < NAPLPS_CMAP_SIZE; i++) {
        if (d->cmap_used[i]) continue;
        if (i == d->addr_black || i == d->addr_white) continue;
        d->surf.cmap[i] = *c;
        d->cmap_used[i] = 1;
        d->draw_addr = i;
        return;
    }

    d->st.pdi_skipped++;   /* no entry available; colour unchanged */
}

static unsigned char fg(const naplps_decoder_t *d)
{
    return (unsigned char)(d->draw_addr & (NAPLPS_CMAP_SIZE - 1));
}

/* ------------------------------------------------------------------ */
/* Geometry helpers                                                    */
/* ------------------------------------------------------------------ */

static int pelw(const naplps_decoder_t *d)
{
    long v = d->pel_dx < 0 ? -d->pel_dx : d->pel_dx;
    int p = FX_TO_PX(v);
    return p;
}

static int pelh(const naplps_decoder_t *d)
{
    long v = d->pel_dy < 0 ? -d->pel_dy : d->pel_dy;
    int p = FX_TO_PX(v);
    return p;
}

static void mark_drew(naplps_decoder_t *d) { d->ever_drew = 1; }
static void begin_text_presentation(naplps_decoder_t *d);

/*
 * The area mask for a fill, built at fill time rather than when TEXTURE
 * arrives, because the hatch geometry is the logical pel's (5.3.2.4.4)
 * and DOMAIN can change the pel between the two. "If the logical pel
 * size is (0,0), solid texture patterns will always be drawn": that test
 * is on the unrounded pel, since a pel smaller than one pixel is still
 * a pel and must hatch rather than silently go solid.
 */
static const naplps_mask_t *area_mask(naplps_decoder_t *d)
{
    int pattern = d->area_pattern;
    if (pattern >= 1 && pattern <= 3 && d->pel_dx == 0 && d->pel_dy == 0)
        pattern = 0;
    naplps_mask_pattern(&d->area_mask, pattern, pelw(d), pelh(d),
                        d->color_mode == 2 ? (int)d->bg_addr : -1);
    return &d->area_mask;
}

/*
 * 5.3.2.3.7 Move Attributes, Table 10. The two flags encode the four
 * cases:
 *
 *   00 move together      cursor_lead = 0, dp_lead = 0
 *   01 cursor leads       cursor_lead = 1, dp_lead = 0
 *   10 drawing pt leads   cursor_lead = 0, dp_lead = 1
 *   11 independent        cursor_lead = 1, dp_lead = 1
 *
 * "If the cursor is defined as leading (01), then every time the cursor
 * is moved, the drawing point will move along with it, but not vice
 * versa. If the drawing point is set to lead the cursor (10), then every
 * time the drawing point is moved, the cursor will move with it, but not
 * vice versa."
 *
 * So the drawing point follows the cursor unless the DRAWING POINT is
 * the leader, and the cursor follows the drawing point unless the CURSOR
 * is the leader. Independent is both leaders and neither follows.
 */
static void dp_moved(naplps_decoder_t *d)
{
    if (d->cursor_lead) return;      /* cursor leads, or independent */
    d->cx = d->dpx;
    d->cy = d->dpy;
}

static void cursor_moved(naplps_decoder_t *d)
{
    if (d->dp_lead) return;          /* drawing point leads, or independent */
    d->dpx = d->cx;
    d->dpy = d->cy;
}

/* ------------------------------------------------------------------ */
/* Text                                                               */
/* ------------------------------------------------------------------ */

static long cf_w(const naplps_decoder_t *d)
{ return d->cf_dx < 0 ? -d->cf_dx : d->cf_dx; }

static long cf_h(const naplps_decoder_t *d)
{ return d->cf_dy < 0 ? -d->cf_dy : d->cf_dy; }

/*
 * The rectangle that text flow is confined to.
 *
 * Every cursor-movement clause in the standard is phrased the same way:
 * the bound is the display area "or ... the active field, should the full
 * character field corresponding to the cursor lie entirely within the
 * active field immediately before the movement" (6.1.2.1, 6.1.2.3,
 * 6.1.2.7 and 5.3.2.3.6). So the active field only takes over while the
 * cursor is genuinely inside it, and text that has wandered out flows
 * against the screen again.
 *
 * ONE DISCREPANCY IN THE STANDARD'S OWN WORDING, recorded rather than
 * quietly resolved: 5.3.2.3.6 gives the automatic APR APD bound as the
 * UNIT SCREEN, while 6.1.2.3 and 6.1.2.7 give the APD and APR bound as
 * the DISPLAY AREA. They differ above Y = 0.78125, where the unit screen
 * continues and the display area does not. The display area is used here,
 * because it is what the two cursor-movement clauses say, it is what
 * makes the 40 by 20 NORMAL TEXT grid come out right, and flowing text
 * into a region the terminal cannot show would be a strange reading of a
 * feature whose purpose is to keep text on screen.
 */
static void text_bounds(const naplps_decoder_t *d,
                        long *x0, long *y0, long *x1, long *y1)
{
    long cw = cf_w(d), ch = cf_h(d);

    if (d->af_set &&
        d->cx >= d->af_x && d->cx + cw <= d->af_x + d->af_w &&
        d->cy >= d->af_y && d->cy + ch <= d->af_y + d->af_h) {
        *x0 = d->af_x;              *y0 = d->af_y;
        *x1 = d->af_x + d->af_w;    *y1 = d->af_y + d->af_h;
        return;
    }
    *x0 = 0; *y0 = 0; *x1 = FX_ONE; *y1 = VIS_Y_FX;
}

static void text_home(naplps_decoder_t *d)
{
    d->cx = 0;
    d->cy = VIS_Y_FX - cf_h(d);
    cursor_moved(d);
}

/* Scroll the display area by one interrow step, 6.2.7.13. */
static void scroll_up(naplps_decoder_t *d, int dy_px)
{
    int y;
    if (dy_px <= 0) return;
    if (dy_px >= NAPLPS_H) {
        naplps_surface_clear(&d->surf,
            (unsigned char)(d->color_mode == 2 ? d->bg_addr : d->addr_black));
        return;
    }
    /* Surface row 0 is the BOTTOM, so scrolling the picture UP off the
     * top means moving rows DOWN in the buffer. */
    for (y = NAPLPS_H - 1; y >= dy_px; y--)
        memcpy(&d->surf.pix[(y) * NAPLPS_W],
               &d->surf.pix[(y - dy_px) * NAPLPS_W], NAPLPS_W);
    for (y = 0; y < dy_px; y++)
        memset(&d->surf.pix[y * NAPLPS_W],
               (int)(d->color_mode == 2 ? d->bg_addr : d->addr_black),
               NAPLPS_W);
}

static void scroll_down(naplps_decoder_t *d, int dy_px)
{
    int y;
    if (dy_px <= 0) return;
    if (dy_px >= NAPLPS_H) {
        naplps_surface_clear(&d->surf,
            (unsigned char)(d->color_mode == 2 ? d->bg_addr : d->addr_black));
        return;
    }
    for (y = 0; y + dy_px < NAPLPS_H; y++)
        memcpy(&d->surf.pix[y * NAPLPS_W],
               &d->surf.pix[(y + dy_px) * NAPLPS_W], NAPLPS_W);
    for (y = NAPLPS_H - dy_px; y < NAPLPS_H; y++)
        memset(&d->surf.pix[y * NAPLPS_W],
               (int)(d->color_mode == 2 ? d->bg_addr : d->addr_black),
               NAPLPS_W);
}

/* APD, 6.1.2.3: one interrow step perpendicular to the path, -90 deg. */
static void text_lf(naplps_decoder_t *d)
{
    long step = (d->path < 2 ? cf_h(d) : cf_w(d));
    long x0, y0, x1, y1;

    step = step * d->irs_num / d->irs_den;
    text_bounds(d, &x0, &y0, &x1, &y1);

    if (d->path < 2) {                 /* path right or left: down */
        d->cy -= step;
        if (d->cy < y0) {
            if (d->scroll) { scroll_up(d, FX_TO_PX(step)); d->cy += step; }
            else           { d->cy = y1 - cf_h(d); }
        }
    } else {                            /* path up or down: leftward */
        d->cx -= step;
        if (d->cx < x0) d->cx = x1 - cf_w(d);
    }
    cursor_moved(d);
}

/* APU, 6.1.2.5: one interrow step at +90 degrees. */
static void text_rlf(naplps_decoder_t *d)
{
    long step = (d->path < 2 ? cf_h(d) : cf_w(d));
    long x0, y0, x1, y1;

    step = step * d->irs_num / d->irs_den;
    text_bounds(d, &x0, &y0, &x1, &y1);

    if (d->path < 2) {
        d->cy += step;
        if (d->cy + cf_h(d) > y1) {
            if (d->scroll) { scroll_down(d, FX_TO_PX(step)); d->cy -= step; }
            else           { d->cy = y0; }
        }
    } else {
        d->cx += step;
        if (d->cx + cf_w(d) > x1) d->cx = x0;
    }
    cursor_moved(d);
}

/* APR, 6.1.2.7: first character position along the character path. */
static void text_cr(naplps_decoder_t *d)
{
    long x0, y0, x1, y1;
    text_bounds(d, &x0, &y0, &x1, &y1);

    switch (d->path) {
    case 0: d->cx = x0;                break;
    case 1: d->cx = x1 - cf_w(d);      break;
    case 2: d->cy = y0;                break;
    default: d->cy = y1 - cf_h(d);     break;
    }
    cursor_moved(d);
}

/*
 * The proportional-spacing advance, 5.3.2.3.4.
 *
 * The standard makes the algorithm ours ("The proportional spacing
 * algorithm is implementation dependent") and imposes exactly two
 * constraints: "each character shall be completely contained within the
 * area defined by the current character field", and the line must hold
 * "at least as many characters per line as would be allowed by the
 * current character field dimensions".
 *
 * This one measures the glyph's own ink width in the 5x7 cell and
 * advances by that plus one column of gap, as a fraction of the six
 * columns a full-width character occupies. A full-width glyph therefore
 * advances by exactly the character field width, which is the fixed case,
 * and everything narrower advances by less. That satisfies both
 * constraints by construction: never more than the fixed advance, so
 * never fewer characters per line, and the glyph is still drawn inside
 * its own field.
 *
 * The clamp keeps SPACE and the thin punctuation from collapsing into an
 * unreadable line.
 */
static int glyph_ink_width(unsigned char ch)
{
    int gi, row, col, w = 0;

    if (ch < NAPLPS_GLYPH_FIRST ||
        ch >= (unsigned char)(NAPLPS_GLYPH_FIRST + NAPLPS_GLYPH_COUNT))
        return NAPLPS_GLYPH_W;
    gi = ch - NAPLPS_GLYPH_FIRST;

    for (row = 0; row < NAPLPS_GLYPH_H; row++)
        for (col = 0; col < NAPLPS_GLYPH_W; col++)
            if ((naplps_font5x7[gi][row] >> col) & 1)
                if (col + 1 > w) w = col + 1;

    if (w < 2) w = 2;
    return w;
}

/*
 * THE PROPORTIONAL METRIC, and why it has the constants it has.
 *
 * 5.3.2.3.4 hands the whole algorithm to the implementation ("The
 * proportional spacing algorithm is implementation dependent") and
 * imposes exactly one measurable requirement: a proportional line must
 * hold "at least as many characters per line as would be allowed by the
 * current character field dimensions". Anything at or below one cell per
 * character satisfies that, so the standard does not decide this and
 * cannot be made to.
 *
 * What DOES decide it is period content. Gate G6 measured
 * Personality+III laying out NAPLPS.011, whose lines are each placed by
 * an explicit POINT SET ABS and are therefore authored to fit exactly:
 * PP3's average advance across those lines runs from 0.670 to 0.747 of
 * the character field width. A metric outside that band does not merely
 * look different, it makes an authored line overflow, take an automatic
 * APR APD, and drop its tail onto the next line.
 *
 * So the glyph's own ink width w, in the 5 wide by 7 tall cell, is laid
 * out on an 18-unit em as 2w of ink plus 3 units of side bearing. That
 * gives 0.677 to 0.688 average on the same four lines, inside the
 * measured band, and every authored line fits. The widest possible
 * glyph, w = 5, advances 13/18 = 0.72 of a cell, so the standard's
 * lower bound on density holds by construction for every string.
 *
 * The first version of this used (w + 1) / 6, which reaches a full cell
 * for any glyph using all five columns. On a 5x7 bitmap face that is
 * most lowercase letters, so it was barely tighter than fixed spacing
 * and NAPLPS.011 wrapped on every line.
 */
static long prop_advance(long cell, unsigned char ch)
{
    return cell * (long)(2 * glyph_ink_width(ch) + 3) / 18;
}

/*
 * The width used to LAY OUT and DRAW one character.
 *
 * Note this is not the character FIELD width. 5.3.2.3.4 is explicit that
 * "The width of the character field does not change. For example, when a
 * character is displayed at the end of a line in color mode 2, the
 * background color is displayed for the full width of the character
 * field." So the background a character paints stays cf_w even in
 * proportional mode; only the glyph and the advance narrow.
 */
static long draw_cell_w(const naplps_decoder_t *d, unsigned char ch)
{
    if (!d->prop_spacing) return cf_w(d);
    return prop_advance(cf_w(d), ch);
}

/* Advance one intercharacter step along the character path. */
static void text_advance_ch(naplps_decoder_t *d, unsigned char ch, int have_ch)
{
    long stepx = 0, stepy = 0;
    long s;

    if (d->path < 2) {
        if (d->prop_spacing && have_ch) s = prop_advance(cf_w(d), ch);
        else                            s = cf_w(d) * d->ics_num / d->ics_den;
        stepx = (d->path == 0) ? s : -s;
    } else {
        if (d->prop_spacing && have_ch) s = prop_advance(cf_h(d), ch);
        else                            s = cf_h(d) * d->ics_num / d->ics_den;
        stepy = (d->path == 2) ? s : -s;
    }

    d->cx += stepx;
    d->cy += stepy;

    /*
     * 5.3.2.3.6 Automatic APR APD: if the move would put any part of the
     * character field outside the bound, an automatic APR and APD are
     * executed immediately. The bound is the active field when the cursor
     * is inside one, which is what makes columnated text column.
     */
    {
        long x0, y0, x1, y1, fitw, fith;
        text_bounds(d, &x0, &y0, &x1, &y1);

        /*
         * WHAT COUNTS AS "FITTING", in proportional mode.
         *
         * 5.3.2.3.6 phrases the automatic APR APD test as "any part of
         * the full corresponding character field" leaving the bound, and
         * in fixed spacing that is unambiguous: the field is cf_w wide.
         *
         * In PROPORTIONAL spacing it is not. 5.3.2.3.4 requires only that
         * "each character shall be completely contained within the area
         * defined by the current character field", and a proportionally
         * spaced character is drawn into the narrower box that
         * draw_cell_w gives it. Testing such a character against the full
         * cf_w would reserve room the character does not use, and would
         * throw away the last characters of a line that the author
         * measured as fitting.
         *
         * Gate G6 settled it. NAPLPS.011 places each of its lines with an
         * explicit POINT SET ABS, so every line is authored to fit
         * exactly, and Personality+III renders all six complete. With the
         * full-field test this decoder dropped the tail of two of them
         * onto the following line. The proportional width is the right
         * measure for a proportional character.
         */
        fitw = (d->prop_spacing && have_ch) ? prop_advance(cf_w(d), ch) : cf_w(d);
        fith = (d->prop_spacing && have_ch) ? prop_advance(cf_h(d), ch) : cf_h(d);

        if (d->path < 2) {
            if (d->cx + fitw > x1 || d->cx < x0) { text_cr(d); text_lf(d); }
        } else {
            if (d->cy + fith > y1 || d->cy < y0) { text_cr(d); text_lf(d); }
        }
    }
    cursor_moved(d);
}

static void text_advance(naplps_decoder_t *d)
{
    text_advance_ch(d, 0x20, 0);
}

/* APB, 6.1.2.1: one intercharacter step against the character path. */
static void text_bs(naplps_decoder_t *d)
{
    long s, x0, y0, x1, y1;
    text_bounds(d, &x0, &y0, &x1, &y1);
    if (d->path < 2) {
        s = cf_w(d) * d->ics_num / d->ics_den;
        d->cx -= (d->path == 0) ? s : -s;
        if (d->cx < x0) { d->cx = x1 - cf_w(d); text_rlf(d); }
    } else {
        s = cf_h(d) * d->ics_num / d->ics_den;
        d->cy -= (d->path == 2) ? s : -s;
        if (d->cy < y0) d->cy = y1 - cf_h(d);
    }
    cursor_moved(d);
}

/*
 * Draw one character of the primary set at the cursor.
 *
 * The glyph is a 5x7 pattern scaled by nearest neighbour into the
 * character field. The field is 6.4 x 10 pixels at NORMAL TEXT, so the
 * glyph box is taken as 5/6 of the field width and 7/10 of its height,
 * which reproduces exactly 5x7 in a 6x10 cell at the default size and
 * scales sensibly for the other four.
 *
 * Reverse video, 6.2.7.4: "the pixels surrounding the character shape in
 * the character field are drawn in the drawing color. The pixels of the
 * character shape are not drawn, except in color mode 2, when they are
 * drawn in the background color."
 *
 * Colour mode 2, 5.3.2.6.3: "Characters received while in color mode 2
 * will be drawn in the drawing color over the background color, which
 * occupies the remainder of the character field."
 */
static void draw_char(naplps_decoder_t *d, unsigned char ch)
{
    int fx0, fy0, fw, fh, dw, gw, gh, gx, gy, base;
    int glyph_i;
    unsigned char shape_addr, field_addr;
    int paint_field;

    if (ch < NAPLPS_GLYPH_FIRST ||
        ch >= (unsigned char)(NAPLPS_GLYPH_FIRST + NAPLPS_GLYPH_COUNT))
        return;
    glyph_i = ch - NAPLPS_GLYPH_FIRST;

    fx0 = FX_TO_PX(d->cx);
    fy0 = FX_TO_PX(d->cy);
    /* The character FIELD, which is what colour mode 2 and reverse video
     * paint, is always the full cf_w wide (5.3.2.3.4). */
    fw  = FX_TO_PX(d->cx + cf_w(d)) - fx0;
    fh  = FX_TO_PX(d->cy + cf_h(d)) - fy0;
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;

    /* The GLYPH box narrows with the proportional advance so that
     * adjacent characters cannot run into each other. */
    dw  = FX_TO_PX(d->cx + draw_cell_w(d, ch)) - fx0;
    if (dw < 1) dw = 1;

    gw = dw * NAPLPS_GLYPH_W / 6;   if (gw < 1) gw = 1;
    gh = fh * NAPLPS_GLYPH_H / 10;  if (gh < 1) gh = 1;

    /*
     * The visual font baseline, Appendix D Table D1: "The offset of the
     * visual font baseline, ie, the apparent bottom of upper case
     * alphabetic characters (see Figure D1) shall be 20% of the height
     * (dy) of the current character field size". So the glyph does NOT
     * stand on the field origin; it stands 20% of dy above it, 2 pixels
     * at NORMAL TEXT, and the 5x7 box then fills rows 2 to 8 of the 10.
     *
     * CORRECTED 2026-09-26. The glyph used to stand on the origin, which
     * put every line of text 2 pixels below Personality+III's and cut
     * the bottom row's text against the edge of the display area. The
     * real-client gate G7 showed it on every page; the offset is the
     * standard's, and PP3 was the one honouring it.
     */
    base = (fh * 2 + 5) / 10;
    if (base + gh > fh) base = fh - gh;
    if (base < 0) base = 0;

    if (d->reverse_video) {
        field_addr = fg(d);
        shape_addr = (unsigned char)d->bg_addr;
        paint_field = 1;
    } else {
        field_addr = (unsigned char)d->bg_addr;
        shape_addr = fg(d);
        paint_field = (d->color_mode == 2);
    }

    if (paint_field)
        naplps_fill_rect(&d->surf, fx0, fy0, fw, fh, field_addr, NULL);

    /* In reverse video outside colour mode 2 the shape is simply not
     * drawn: the field alone carries the character. */
    if (!(d->reverse_video && d->color_mode != 2)) {
        for (gy = 0; gy < gh; gy++) {
            int sy = gy * NAPLPS_GLYPH_H / gh;
            /* Glyph row 0 is the TOP row, and the surface's Y grows
             * upward, so the top glyph row lands at the top of the box. */
            int py = fy0 + base + (gh - 1 - gy);
            unsigned char bits = naplps_font5x7[glyph_i][sy];
            for (gx = 0; gx < gw; gx++) {
                int sx = gx * NAPLPS_GLYPH_W / gw;
                if ((bits >> sx) & 1)
                    naplps_plot(&d->surf, fx0 + gx, py, shape_addr);
            }
        }
    }

    /* UNDERLINE START, 6.2.7.15: a line at the character field origin
     * across the full dx of the field, thickness from the logical pel. */
    if (d->underline) {
        int t = pelh(d); if (t < 1) t = 1;
        naplps_fill_rect(&d->surf, fx0, fy0, fw, t, fg(d), NULL);
    }

    d->st.chars_drawn++;
    mark_drew(d);
    d->last_spacing_char = ch;
    d->have_last_spacing = 1;
    text_advance_ch(d, ch, 1);
}

/* ------------------------------------------------------------------ */
/* PDI dispatch                                                        */
/* ------------------------------------------------------------------ */

/*
 * WHAT IS IMPLEMENTED AND WHAT IS NOT.
 *
 * Implemented: RESET, DOMAIN, TEXT, TEXTURE, SET COLOR, SELECT COLOR,
 * WAIT, and the six geometric primitives POINT, LINE, ARC, RECTANGLE and
 * POLYGON in all four forms each.
 *
 * Recognised and deliberately skipped, per the Phase 1 scope and section
 * 8.3.2's requirement that an unimplemented feature be skipped SAFELY
 * rather than executed as garbage:
 *
 *   BLINK (0x3F)          excluded by scope
 *   FIELD (0x38)          the active field exists only to serve
 *                         unprotected fields and INCREMENTAL POINT, both
 *                         excluded; the operand is consumed
 *   INCREMENTAL 0x39-0x3B the compact string-operand encodings
 *
 * Each of those consumes its operands correctly and increments
 * st.pdi_skipped, so the stream stays in sync and the count is visible.
 *
 * REPEAT vs ACCUM. 5.3.2.2.5 says an operand longer than the declared
 * length "is taken as an indication to repeat the execution of the opcode
 * with the subsequent numeric data taken as new operands". That applies
 * to the opcodes with a fixed operand footprint per execution, marked
 * REPEAT: they execute as soon as a full set has arrived, which also
 * makes a live stream draw progressively. The others are marked ACCUM
 * because the command is not defined until the whole run is in hand: a
 * POLYGON's vertex count IS "the amount of data following the POLYGON
 * opcode" (5.3.3.5.1), an ARC becomes a spline when "more than three
 * points are specified" (5.3.3.3.1), and SELECT COLOR's very MODE is
 * chosen by how many operands it got (5.3.2.6). Those execute when the
 * sequence terminates.
 */
#define M_REPEAT 0
#define M_ACCUM  1

typedef struct {
    int nfixed;    /* fixed-format operand bytes            */
    int nmulti;    /* multi-value operand blocks per set    */
    int nsingle;   /* single-value operands per set         */
    int mode;      /* M_REPEAT or M_ACCUM                   */
} pdi_desc_t;

static const pdi_desc_t pdi_desc[32] = {
    /* 0x20 RESET            */ { 2, 0, 0, M_ACCUM  },
    /* 0x21 DOMAIN           */ { 1, 1, 0, M_ACCUM  },
    /* 0x22 TEXT             */ { 2, 1, 0, M_ACCUM  },
    /* 0x23 TEXTURE          */ { 1, 1, 0, M_ACCUM  },
    /* 0x24 POINT SET ABS    */ { 0, 1, 0, M_REPEAT },
    /* 0x25 POINT SET REL    */ { 0, 1, 0, M_REPEAT },
    /* 0x26 POINT ABS        */ { 0, 1, 0, M_REPEAT },
    /* 0x27 POINT REL        */ { 0, 1, 0, M_REPEAT },
    /* 0x28 LINE ABS         */ { 0, 1, 0, M_REPEAT },
    /* 0x29 LINE REL         */ { 0, 1, 0, M_REPEAT },
    /* 0x2A SET & LINE ABS   */ { 0, 2, 0, M_REPEAT },
    /* 0x2B SET & LINE REL   */ { 0, 2, 0, M_REPEAT },
    /* 0x2C ARC OUTLINED     */ { 0, 2, 0, M_ACCUM  },
    /* 0x2D ARC FILLED       */ { 0, 2, 0, M_ACCUM  },
    /* 0x2E SET & ARC OUT    */ { 0, 3, 0, M_ACCUM  },
    /* 0x2F SET & ARC FILL   */ { 0, 3, 0, M_ACCUM  },
    /* 0x30 RECT OUTLINED    */ { 0, 1, 0, M_REPEAT },
    /* 0x31 RECT FILLED      */ { 0, 1, 0, M_REPEAT },
    /* 0x32 SET & RECT OUT   */ { 0, 2, 0, M_REPEAT },
    /* 0x33 SET & RECT FILL  */ { 0, 2, 0, M_REPEAT },
    /* 0x34 POLY OUTLINED    */ { 0, 1, 0, M_ACCUM  },
    /* 0x35 POLY FILLED      */ { 0, 1, 0, M_ACCUM  },
    /* 0x36 SET & POLY OUT   */ { 0, 2, 0, M_ACCUM  },
    /* 0x37 SET & POLY FILL  */ { 0, 2, 0, M_ACCUM  },
    /* 0x38 FIELD            */ { 0, 2, 0, M_ACCUM  },
    /* 0x39 INCR POINT       */ { 1, 0, 0, M_ACCUM  },
    /* 0x3A INCR LINE        */ { 0, 1, 0, M_ACCUM  },
    /* 0x3B INCR POLY FILL   */ { 0, 1, 0, M_ACCUM  },
    /* 0x3C SET COLOR        */ { 0, 1, 0, M_REPEAT },
    /* 0x3D WAIT             */ { 2, 0, 0, M_ACCUM  },
    /* 0x3E SELECT COLOR     */ { 0, 0, 1, M_ACCUM  },
    /* 0x3F BLINK            */ { 1, 0, 1, M_ACCUM  }
};

static int set_bytes(const naplps_decoder_t *d, int op)
{
    const pdi_desc_t *k = &pdi_desc[op - 0x20];
    return k->nfixed + k->nmulti * d->mv_len + k->nsingle * d->sv_len;
}

/* ------------------------------------------------------------------ */
/* PDI executors                                                       */
/* ------------------------------------------------------------------ */

static void exec_reset(naplps_decoder_t *d, const unsigned char *p, int n)
{
    unsigned int b1 = (n > 0) ? (p[0] & 0x3F) : 0;
    unsigned int b2 = (n > 1) ? (p[1] & 0x3F) : 0;

    /* 5.3.2.9.3: "If the RESET command is received with no operands, it
     * is interpreted as if it had been sent with bits b6 to b1 in both
     * bytes set equal to 0", ie a bare RESET does nothing at all. */

    if (b1 & 0x01) reset_domain(d);

    switch ((b1 >> 1) & 0x03) {                 /* Table 14 */
    case 0: break;
    case 1: reset_color(d, 1); d->color_mode = 0; break;
    case 2: reset_color(d, 1); d->color_mode = 1; break;
    default: reset_color(d, 1); d->color_mode = 1; break;
    }

    switch ((b1 >> 3) & 0x07) {                 /* Table 15 */
    case 0: break;
    case 1: naplps_surface_clear(&d->surf, (unsigned char)d->addr_black);
            mark_drew(d); break;
    case 2: naplps_surface_clear(&d->surf, fg(d)); mark_drew(d); break;
    case 3: break;                              /* border only: no border here */
    case 4: break;
    case 5: naplps_surface_clear(&d->surf, fg(d)); mark_drew(d); break;
    case 6: naplps_surface_clear(&d->surf, fg(d)); mark_drew(d); break;
    default: naplps_surface_clear(&d->surf, (unsigned char)d->addr_black);
             mark_drew(d); break;
    }

    if (b2 & 0x01) { reset_text(d); text_home(d); }
    if (b2 & 0x02) { /* blink processes: none are created here */ }
    if (b2 & 0x04) { /* unprotected fields: none are created here */ }
    if (b2 & 0x08) reset_texture(d);
    if (b2 & 0x10) { /* macros: none are stored here */ }
    if (b2 & 0x20) { /* DRCS: none are stored here */ }
}

static void exec_domain(naplps_decoder_t *d, const unsigned char *p, int n)
{
    unsigned int b1;
    int sv_pat, mv_pat;

    if (n < 1) return;
    b1 = p[0] & 0x3F;

    /*
     * TRAP T1. Tables 4 and 5 map a BIT PATTERN to a BYTE COUNT and the
     * count is the pattern PLUS ONE. Two bits index a 1..4 range and
     * three bits index a 1..8 range, and the stated defaults (one byte
     * for single-value, three for multi-value) pin the offset: the
     * default DOMAIN state is all zero bits, so pattern 00 must be 1
     * byte, and the multi-value default of 3 bytes sits at pattern 010.
     *
     * Conflating the pattern with the count is invisible until a whole
     * file misparses, because 5.3.2.2.5 resynchronises by repeating the
     * opcode instead of raising an error: the result is extra shapes
     * drawn from misaligned data, never a parse failure.
     */
    sv_pat = (int)(b1 & 0x03);                   /* b2 b1 */
    mv_pat = (int)((b1 >> 2) & 0x07);            /* b5 b4 b3 */
    d->sv_len = sv_pat + 1;
    d->mv_len = mv_pat + 1;
    d->dims   = ((b1 >> 5) & 0x01) ? 3 : 2;      /* b6 */

    /* 5.3.2.2.6: "the new length of the multi-value operands, as set in
     * byte 1, applies to the multi-value logical pel size operand of
     * that DOMAIN command", and "If the logical pel size operand is
     * omitted, the size of the logical pel shall not be changed." */
    if (n - 1 >= d->mv_len)
        unpack_multi(d, p + 1, &d->pel_dx, &d->pel_dy);
}

static void exec_text(naplps_decoder_t *d, const unsigned char *p, int n)
{
    static const int sp_num[4] = { 1, 5, 3, 1 };
    static const int sp_den[4] = { 1, 4, 2, 1 };
    static const int ir_num[4] = { 1, 5, 3, 2 };
    static const int ir_den[4] = { 1, 4, 2, 1 };
    unsigned int b1, b2;
    int mv;

    if (n < 1) return;
    b1 = p[0] & 0x3F;
    b2 = (n > 1) ? (p[1] & 0x3F) : 0;

    d->rotation = (int)((b1 & 0x03) * 90);       /* Table 6  */
    d->path     = (int)((b1 >> 2) & 0x03);       /* Table 7  */
    {
        int k = (int)((b1 >> 4) & 0x03);         /* Table 8  */
        d->ics_num = sp_num[k];
        d->ics_den = sp_den[k];
        d->prop_spacing = (k == 3);
        /*
         * k == 3 is PROPORTIONAL SPACING, and it is not safe to treat it
         * as fixed spacing of 1. 5.3.2.3.4 leaves the algorithm
         * implementation-dependent but fixes the consequence: "the exact
         * number of characters per line is not known in proportional
         * spacing mode, but it is at least as many characters per line as
         * would be allowed by the current character field dimensions."
         *
         * A frame that asks for proportional spacing is relying on that
         * guarantee to fit its lines. Substituting fixed spacing makes
         * every such line overflow and take an automatic APR APD, and the
         * wrapped remainder then lands on top of whatever the frame put
         * on the next row. The result is legible text drawn over legible
         * text, which looks like a cursor-positioning bug and is not one.
         * See glyph_advance().
         */
    }
    {
        int k = (int)(b2 & 0x03);                /* Table 9  */
        d->irs_num = ir_num[k];
        d->irs_den = ir_den[k];
    }
    {
        int k = (int)((b2 >> 2) & 0x03);         /* Table 10 */
        d->cursor_lead = (k == 1) || (k == 3);
        d->dp_lead     = (k == 2) || (k == 3);
    }

    if (d->rotation != 0) d->st.pdi_skipped++;   /* see the note below  */

    mv = (n > 2) ? (n - 2) : 0;
    if (mv >= d->mv_len) {
        long dx, dy;
        unpack_multi(d, p + 2, &dx, &dy);
        /* 5.3.2.3.9: the character field dimensions. A zero dimension
         * would make text invisible and is almost certainly a stream
         * error, so it is ignored rather than obeyed. */
        if (dx != 0) d->cf_dx = dx;
        if (dy != 0) d->cf_dy = dy;
    }
}

static void exec_texture(naplps_decoder_t *d, const unsigned char *p, int n)
{
    unsigned int b1;

    if (n < 1) return;
    b1 = p[0] & 0x3F;

    /*
     * 5.3.2.4, byte 1. Bits b2 b1 are the line texture (Table 12), b3 the
     * highlight (5.3.2.4.3), b6 b5 b4 the area texture pattern (Table 13).
     *
     * CORRECTED 2026-09-26, two errors the real-client gate exposed:
     *
     * - The area patterns are FIXED by Table 13 and Figure 21, not a
     *   rendering decision as this used to claim. See naplps_mask_pattern.
     *   The pattern is stored and the mask built at fill time, because the
     *   hatch spacing is the logical pel's.
     *
     * - Table 12 reads 00 solid, 01 DOTTED, 10 DASHED, 11 dotted-dashed.
     *   The table here had dotted and dashed the wrong way round. The
     *   dot and dash lengths themselves are this renderer's; the order is
     *   the standard's.
     *
     * The highlight bit is read past and not executed: 5.3.2.4.3 outlines
     * filled figures in nominal black, and it is recorded as not yet
     * implemented rather than approximated.
     */
    {
        static const unsigned short lt[4] = {
            0xFFFF,     /* 00 solid          */
            0xCCCC,     /* 01 dotted         */
            0xF0F0,     /* 10 dashed         */
            0xFAFA      /* 11 dotted-dashed  */
        };
        d->line_texture = lt[b1 & 0x03];
    }
    d->area_pattern = (int)((b1 >> 3) & 0x07);
}

static void exec_wait(naplps_decoder_t *d, const unsigned char *p, int n)
{
    int i;

    /*
     * 5.3.2.8.2: the first operand byte must match the fixed pattern of
     * Figure 28, and "If any other bit combination follows the WAIT
     * opcode, the entire command is reserved for future standardization
     * and shall be executed as a null operation."
     *
     * Figure 28 is one of the bit diagrams the OCR of the reference scan
     * damages, and the reading taken is b6..b1 = 011100, ie 0x5C with
     * b7 set. It was marked PROVISIONAL when written, on the grounds
     * that a misreading is safe in this direction: a mismatch is a null
     * operation, which is exactly what the standard demands, so it would
     * degrade to "waits are ignored" and be counted rather than
     * desynchronising the stream.
     *
     * THE READING IS NOW CONFIRMED, empirically. Five of the twelve
     * Personality+III sample frames use WAIT, fifty commands between
     * them, and st.waits_malformed is ZERO across all twelve. Had the
     * fixed byte been read wrongly, every one of those fifty would have
     * been counted malformed instead. See section 8.1 of the Phase 2a
     * report for the per-frame counts.
     */
    if (n < 1) return;
    if ((p[0] & 0x3F) != 0x1C) { d->st.waits_malformed++; return; }

    for (i = 1; i < n; i++) {
        /* "The next byte of operand data gives the time delay in units
         * of 1/10 of a second... If any additional data bytes follow,
         * they are treated as additional periods of waiting time." */
        d->wait_tenths += (int)(p[i] & 0x3F);
        d->st.waits++;
    }
}

static void exec_select_color(naplps_decoder_t *d, const unsigned char *p, int n)
{
    int nops = (d->sv_len > 0) ? (n / d->sv_len) : 0;

    /*
     * 5.3.2.6: the OPERAND COUNT chooses the colour mode. Zero operands
     * means mode 0, one means mode 1, two means mode 2, and "Additional
     * numeric data bytes are reserved for future standardization and
     * shall be ignored" - so three or more is mode 2 with the rest
     * dropped, NOT a repeat.
     */
    if (nops <= 0) {
        d->color_mode = 0;
        /* Mode 0 keeps drawing in whatever colour VALUE it last had, and
         * that value has to be re-resolved into the map. */
        mode0_set_color(d, &d->draw_value);
        return;
    }

    if (nops == 1) {
        d->color_mode = 1;
        d->draw_addr = unpack_addr(p, d->sv_len);
        d->cmap_used[d->draw_addr] = 1;
        return;
    }

    {
        int a = unpack_addr(p, d->sv_len);
        int b = unpack_addr(p + d->sv_len, d->sv_len);
        d->color_mode = 2;
        /* 5.3.2.6.3: "For the special case in which the two operands are
         * identical... the drawing color is, instead, left at its current
         * value and only the background color is changed." */
        if (a == b) {
            d->bg_addr = b;
        } else {
            d->draw_addr = a;
            d->bg_addr = b;
        }
        d->cmap_used[d->draw_addr] = 1;
        d->cmap_used[d->bg_addr] = 1;
        if (nops > 2) d->st.pdi_skipped++;
    }
}

/*
 * SET COLOR, 5.3.2.5.
 *
 * In mode 0 it sets the drawing colour directly. In modes 1 and 2 it
 * LOADS the colour map at the address the drawing colour currently names,
 * and on an implicit repeat "the address of the color entry to be changed
 * is automatically incremented prior to the execution of the new opcode.
 * The algorithm for incrementing is to change the most significant zero
 * to a one and to change all ones to the left of it to zero."
 *
 * That is a reversed-bit counter, and it is not a typo in the standard:
 * the note explains it exists so that "the same presentation to occur on
 * a device with a greater number of color map entries than the sender has
 * assumed".
 */
static int cmap_next_addr(int a)
{
    int bit;
    for (bit = NAPLPS_CMAP_N - 1; bit >= 0; bit--) {
        if (!((a >> bit) & 1)) {
            a |= (1 << bit);                    /* most significant zero -> one */
            a &= ~((1 << NAPLPS_CMAP_N) - (1 << (bit + 1))); /* ones left of it -> zero */
            return a;
        }
    }
    return -1;    /* all ones: the physical limit, data is ignored */
}

static void exec_set_color(naplps_decoder_t *d, const unsigned char *p, int rep)
{
    naplps_color_t c;
    unpack_color(d, p, &c);

    if (d->color_mode == 0) {
        mode0_set_color(d, &c);
        return;
    }

    if (rep == 0) {
        d->surf.cmap[d->draw_addr & (NAPLPS_CMAP_SIZE - 1)] = c;
        d->cmap_used[d->draw_addr & (NAPLPS_CMAP_SIZE - 1)] = 1;
        find_bw(d);
        mark_drew(d);
        return;
    }

    /* A repeat: walk the reversed-bit counter without disturbing the
     * drawing colour, as the standard requires. */
    {
        int a = d->draw_addr;
        int i;
        for (i = 0; i < rep; i++) {
            a = cmap_next_addr(a);
            if (a < 0) { d->st.pdi_skipped++; return; }
        }
        d->surf.cmap[a & (NAPLPS_CMAP_SIZE - 1)] = c;
        d->cmap_used[a & (NAPLPS_CMAP_SIZE - 1)] = 1;
        find_bw(d);
        mark_drew(d);
    }
}

/*
 * FIELD, 5.3.3.6.2.
 *
 * "The FIELD command is used to define the active field used for
 * columnated text, unprotected fields and INCREMENTAL POINT." Only the
 * first of those three is implemented: columnated text is ordinary text
 * layout, and without it a frame that lays its text out in columns runs
 * every column to the full screen width and draws them over each other.
 * Unprotected fields (the input-form machinery of 6.2.6) and INCREMENTAL
 * POINT remain excluded, and PROTECT / UNPROTECT are still counted as
 * skipped.
 *
 * "The origin point of the field is specified in absolute coordinates
 * (X,Y) as the first block of coordinate data. The next block of
 * coordinate data gives the field dimensions, width and height (dx,dy)...
 * Note that dx or dy, or both, may be positive or negative, so that the
 * origin point may be placed in any of the four corners of the field. The
 * drawing point is set to the origin of the field after FIELD has been
 * executed... If no data bytes follow the FIELD opcode, the active field
 * is set to the full unit screen and the origin point is (0,0). If only
 * one operand follows the FIELD opcode, it specifies the field dimensions
 * and the origin point is the current drawing point."
 */
static void set_dp(naplps_decoder_t *d, long x, long y);

static void exec_field(naplps_decoder_t *d, const unsigned char *p, int n)
{
    int nblk = (d->mv_len > 0) ? (n / d->mv_len) : 0;
    long ox, oy, w, h;

    if (nblk <= 0) {
        reset_field(d);
        set_dp(d, 0, 0);
        return;
    }

    if (nblk == 1) {
        ox = d->dpx; oy = d->dpy;
        unpack_multi(d, p, &w, &h);
    } else {
        unpack_multi(d, p, &ox, &oy);
        unpack_multi(d, p + d->mv_len, &w, &h);
        if (nblk > 2) d->st.pdi_skipped++;   /* reserved, ignored */
    }

    /* Normalise so that af_x, af_y is the lower left and the extents are
     * positive, whichever corner the origin was given in. */
    if (w < 0) { ox += w; w = -w; }
    if (h < 0) { oy += h; h = -h; }

    d->af_x = ox; d->af_y = oy;
    d->af_w = w;  d->af_h = h;
    d->af_set = (w > 0 && h > 0);

    /* "The drawing point is set to the origin of the field after FIELD
     * has been executed." The origin is the one that was GIVEN, not the
     * normalised corner. */
    set_dp(d, (nblk == 1) ? d->dpx : ox, (nblk == 1) ? d->dpy : oy);
}

/* ---- geometric primitives ---- */

static void set_dp(naplps_decoder_t *d, long x, long y)
{
    d->dpx = x; d->dpy = y; dp_moved(d);
}

static void prim_point(naplps_decoder_t *d, int op, const unsigned char *p)
{
    long x, y;
    unpack_multi(d, p, &x, &y);

    if (op == 0x24 || op == 0x26) { /* absolute */
        x = x; y = y;
    } else {                        /* relative to the drawing point */
        x = d->dpx + x; y = d->dpy + y;
    }

    if (op == 0x26 || op == 0x27) { /* the visible forms */
        naplps_draw_point(&d->surf, FX_TO_PX(x), FX_TO_PX(y),
                          pelw(d), pelh(d), fg(d));
        mark_drew(d);
    }
    set_dp(d, x, y);
}

static void prim_line(naplps_decoder_t *d, int op, const unsigned char *p)
{
    long ax, ay, bx, by;

    if (op == 0x28 || op == 0x29) {
        /* 5.3.3.2.2/3: the start point IS the current drawing point, so
         * repeats chain into a polyline. */
        ax = d->dpx; ay = d->dpy;
        unpack_multi(d, p, &bx, &by);
        if (op == 0x29) { bx += ax; by += ay; }
    } else {
        /*
         * 5.3.3.2.4, and this is the trap: SET & LINE is NOT a polyline.
         * "If more than two operands are present, lines are drawn from
         * the first to the second point, then from the third to the
         * fourth point, etc." Each repeat is a fresh, disjoint segment,
         * which is exactly what taking two operands per set gives.
         */
        unpack_multi(d, p, &ax, &ay);
        unpack_multi(d, p + d->mv_len, &bx, &by);
        if (op == 0x2B) { bx += ax; by += ay; }
    }

    naplps_draw_line(&d->surf, FX_TO_PX(ax), FX_TO_PX(ay),
                     FX_TO_PX(bx), FX_TO_PX(by),
                     pelw(d), pelh(d), fg(d), d->line_texture);
    mark_drew(d);
    set_dp(d, bx, by);
}

static void prim_rect(naplps_decoder_t *d, int op, const unsigned char *p)
{
    long x, y, w, h;
    int filled = (op == 0x31 || op == 0x33);

    if (op == 0x30 || op == 0x31) {
        x = d->dpx; y = d->dpy;
        unpack_multi(d, p, &w, &h);
    } else {
        unpack_multi(d, p, &x, &y);
        unpack_multi(d, p + d->mv_len, &w, &h);
    }

    {
        int px = FX_TO_PX(x), py = FX_TO_PX(y);
        int pw = FX_TO_PX(x + w) - px, ph = FX_TO_PX(y + h) - py;

        if (filled) {
            naplps_fill_rect(&d->surf, px, py, pw, ph, fg(d), area_mask(d));
        } else {
            int pts[8];
            int x1 = px + (pw > 0 ? pw - 1 : pw + 1);
            int y1 = py + (ph > 0 ? ph - 1 : ph + 1);
            pts[0] = px; pts[1] = py;
            pts[2] = x1; pts[3] = py;
            pts[4] = x1; pts[5] = y1;
            pts[6] = px; pts[7] = y1;
            naplps_stroke_poly(&d->surf, pts, 4, 1, pelw(d), pelh(d),
                               fg(d), d->line_texture);
        }
        mark_drew(d);
    }

    /* 5.3.3.4.1: "At the completion of drawing a rectangle, the drawing
     * point is the start point altered in x only, by the amount of the
     * dx displacement." That asymmetry is what makes the histogram
     * idiom in the same paragraph work. */
    set_dp(d, x + w, y);
}

/*
 * ARC, 5.3.3.3.
 *
 * Three points: start, an intermediate given RELATIVE TO START, and an
 * end given RELATIVE TO THE INTERMEDIATE (not to start). With more than
 * three points the command becomes a curvilinear spline whose exact
 * shape the standard explicitly leaves open: "The minimum implementation
 * of the spline shall be a series of lines connecting the start,
 * intermediate, and end points of the spline. The display device may draw
 * a smoother spline, but the shape of this spline and the characteristics
 * of the algorithm used are implementation-dependent."
 *
 * This renderer draws the true circular arc through the three points, and
 * for the spline case draws the minimum implementation, the polyline. A
 * smoother spline would be legal and is not more correct.
 */
static void arc_points(long x0, long y0, long x1, long y1, long x2, long y2,
                       int *pts, int *npts, int maxpts)
{
    /* Circumcentre of the three points, in fx. Done in double because the
     * determinant of fx coordinates overflows 32 bits easily and the
     * result feeds pixel rounding either way. */
    double ax = (double)x0, ay = (double)y0;
    double bx = (double)x1, by = (double)y1;
    double cx = (double)x2, cy = (double)y2;
    double dd = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    double ux, uy, r, a0, a1, a2, span, t;
    int i, n;
    int circle = 0;

    *npts = 0;

    /*
     * THE CIRCLE CASE, AND IT MUST BE TAKEN BEFORE THE CIRCUMCENTRE.
     *
     * 5.3.3.3.1: "Drawing of a circle results when the start and end
     * points are coincident; the intermediate point defines the diameter
     * of the circle, and therefore is the midpoint on the arc between the
     * start and end points."
     *
     * A circumcentre needs three DISTINCT points. When the start and end
     * are the same point the determinant is not merely small, it is
     * exactly zero, so the colinear branch below would catch it and draw
     * a line from the start point to itself: nothing at all.
     *
     * That is not a hypothetical. Gate G6 found it. NAPLPS.008 draws its
     * grapes and barrel ends as filled circles in exactly this form, and
     * against Personality+III the outlines and highlights appeared while
     * every filled circle was missing. A synthetic test would not have
     * caught it either, because the segmentation is perfectly correct and
     * the command executes without error; it just draws a zero length
     * line. Real clients close gates.
     *
     * The centre is the midpoint of the start and the intermediate, and
     * the radius is half their separation, which is what "the
     * intermediate point defines the diameter" means.
     */
    if (fabs(ax - cx) < 0.5 && fabs(ay - cy) < 0.5) {
        ux = (ax + bx) * 0.5;
        uy = (ay + by) * 0.5;
        r  = sqrt((ax - ux) * (ax - ux) + (ay - uy) * (ay - uy));
        if (r < 1.0) return;              /* degenerate, nothing to draw */
        a0 = atan2(ay - uy, ax - ux);
        span = 6.283185307179586;         /* a full turn */
        circle = 1;
    } else if (dd > -1e-6 && dd < 1e-6) {
        /* Colinear: 5.3.3.3.1 says "If the three drawing points are
         * colinear, a line is drawn from the start point to the end
         * point". */
        if (maxpts >= 2) {
            pts[0] = FX_TO_PX((long)ax); pts[1] = FX_TO_PX((long)ay);
            pts[2] = FX_TO_PX((long)cx); pts[3] = FX_TO_PX((long)cy);
            *npts = 2;
        }
        return;
    }

    if (!circle) {
        ux = ((ax * ax + ay * ay) * (by - cy) +
              (bx * bx + by * by) * (cy - ay) +
              (cx * cx + cy * cy) * (ay - by)) / dd;
        uy = ((ax * ax + ay * ay) * (cx - bx) +
              (bx * bx + by * by) * (ax - cx) +
              (cx * cx + cy * cy) * (bx - ax)) / dd;

        r = ((ax - ux) * (ax - ux) + (ay - uy) * (ay - uy));
        r = (r > 0.0) ? sqrt(r) : 0.0;

        a0 = atan2(ay - uy, ax - ux);
        a1 = atan2(by - uy, bx - ux);
        a2 = atan2(cy - uy, cx - ux);

        /*
         * Walk from a0 to a2 the way round that passes through a1. The
         * intermediate point is what disambiguates the two arcs, which is
         * why the standard asks for it and advises putting it
         * "approximately midway between the start and end points".
         */
        {
            const double TWO_PI = 6.283185307179586;
            double d1 = a1 - a0, d2 = a2 - a0;
            while (d1 < 0) d1 += TWO_PI;
            while (d2 < 0) d2 += TWO_PI;
            if (d1 <= d2) span = d2;            /* counterclockwise */
            else          span = d2 - TWO_PI;   /* clockwise        */
            if (span == 0.0) span = TWO_PI;
        }
    }

    /* One vertex per pixel of arc length, clamped. */
    n = (int)(r * (span < 0 ? -span : span) / (double)(FX_ONE / NAPLPS_UNIT));
    if (n < 8) n = 8;
    if (n > maxpts - 1) n = maxpts - 1;

    for (i = 0; i <= n; i++) {
        t = a0 + span * (double)i / (double)n;
        pts[2 * i]     = FX_TO_PX((long)(ux + r * cos(t)));
        pts[2 * i + 1] = FX_TO_PX((long)(uy + r * sin(t)));
    }
    *npts = n + 1;
}

static void prim_arc(naplps_decoder_t *d, int op, const unsigned char *p, int n)
{
    long sx, sy, ix, iy, ex, ey;
    int nblk = (d->mv_len > 0) ? (n / d->mv_len) : 0;
    int filled = (op == 0x2D || op == 0x2F);
    int setform = (op == 0x2E || op == 0x2F);
    int need = setform ? 3 : 2;
    int pts[2 * MAX_PTS];
    int npts = 0;
    const unsigned char *q = p;

    if (nblk < (setform ? 2 : 1)) { d->st.operand_short++; return; }

    if (setform) {
        unpack_multi(d, q, &sx, &sy); q += d->mv_len;
    } else {
        sx = d->dpx; sy = d->dpy;
    }

    {
        long rx, ry;
        unpack_multi(d, q, &rx, &ry); q += d->mv_len;
        ix = sx + rx; iy = sy + ry;
    }

    if (nblk >= need) {
        long rx, ry;
        unpack_multi(d, q, &rx, &ry); q += d->mv_len;
        ex = ix + rx; ey = iy + ry;
    } else {
        /* 5.3.3.3.1: "If the end point is omitted, it is taken to be
         * coincident with the start point and a circle is drawn." */
        ex = sx; ey = sy;
    }

    if (nblk > need) {
        /*
         * More than three points: a curvilinear spline. The minimum
         * implementation the standard allows is the polyline through the
         * given points, and that is what is drawn. Counted as skipped
         * because a smoother spline was not attempted, so the number is
         * honest about what the renderer did.
         */
        int k = 0;
        const unsigned char *r2 = p;
        long px2 = 0, py2 = 0, acc_x, acc_y;
        int blk;

        if (setform) { unpack_multi(d, r2, &acc_x, &acc_y); r2 += d->mv_len; }
        else         { acc_x = d->dpx; acc_y = d->dpy; }
        pts[k++] = FX_TO_PX(acc_x); pts[k++] = FX_TO_PX(acc_y);

        for (blk = setform ? 1 : 0; blk < nblk && k < 2 * MAX_PTS - 2; blk++) {
            unpack_multi(d, r2, &px2, &py2); r2 += d->mv_len;
            acc_x += px2; acc_y += py2;
            pts[k++] = FX_TO_PX(acc_x); pts[k++] = FX_TO_PX(acc_y);
        }
        npts = k / 2;
        if (filled) naplps_fill_poly(&d->surf, pts, npts, fg(d), area_mask(d));
        else        naplps_stroke_poly(&d->surf, pts, npts, 0, pelw(d), pelh(d),
                                      fg(d), d->line_texture);
        mark_drew(d);
        d->st.pdi_skipped++;
        set_dp(d, acc_x, acc_y);
        return;
    }

    arc_points(sx, sy, ix, iy, ex, ey, pts, &npts, MAX_PTS);
    if (npts >= 2) {
        if (filled) {
            /* 5.3.3.3.3: the start and end points are joined by a chord
             * and the enclosed figure is filled. The chord is filled but
             * is not part of the outline. */
            naplps_fill_poly(&d->surf, pts, npts, fg(d), area_mask(d));
        } else {
            naplps_stroke_poly(&d->surf, pts, npts, 0, pelw(d), pelh(d),
                               fg(d), d->line_texture);
        }
        mark_drew(d);
    }
    set_dp(d, ex, ey);
}

static void prim_poly(naplps_decoder_t *d, int op, const unsigned char *p, int n)
{
    int nblk = (d->mv_len > 0) ? (n / d->mv_len) : 0;
    int filled = (op == 0x35 || op == 0x37);
    int setform = (op == 0x36 || op == 0x37);
    int pts[2 * MAX_PTS];
    int k = 0, blk;
    long ax, ay, sx, sy;
    const unsigned char *q = p;

    if (nblk < 1) { d->st.operand_short++; return; }

    /*
     * 5.3.3.5.1: each coordinate pair is "a relative displacement from
     * the last vertex (a relative displacement of magnitude 0 is
     * ignored). There is implicit closure between the start point and
     * the last vertex specified so that at the completion of drawing a
     * polygon, the drawing point is coincident with the start point."
     */
    if (setform) { unpack_multi(d, q, &ax, &ay); q += d->mv_len; blk = 1; }
    else         { ax = d->dpx; ay = d->dpy; blk = 0; }

    sx = ax; sy = ay;
    pts[k++] = FX_TO_PX(ax); pts[k++] = FX_TO_PX(ay);

    for (; blk < nblk && k < 2 * MAX_PTS - 2; blk++) {
        long dx, dy;
        unpack_multi(d, q, &dx, &dy); q += d->mv_len;
        if (dx == 0 && dy == 0) continue;       /* magnitude 0 is ignored */
        ax += dx; ay += dy;
        pts[k++] = FX_TO_PX(ax); pts[k++] = FX_TO_PX(ay);
    }

    if (k >= 4) {
        if (filled) naplps_fill_poly(&d->surf, pts, k / 2, fg(d), area_mask(d));
        else        naplps_stroke_poly(&d->surf, pts, k / 2, 1,
                                      pelw(d), pelh(d), fg(d), d->line_texture);
        mark_drew(d);
    }

    set_dp(d, sx, sy);      /* back to the START point, not the last vertex */
}

/* ------------------------------------------------------------------ */
/* PDI sequence machinery                                              */
/* ------------------------------------------------------------------ */

static void pdi_execute_set(naplps_decoder_t *d, int op,
                            const unsigned char *p, int n, int rep)
{
    if (d->trace) d->trace(d->trace_ctx, op, p, n);

    switch (op) {
    case 0x20: exec_reset(d, p, n);   break;
    case 0x21: exec_domain(d, p, n);  break;
    case 0x22: exec_text(d, p, n);    break;
    case 0x23: exec_texture(d, p, n); break;

    case 0x24: case 0x25: case 0x26: case 0x27:
        prim_point(d, op, p); break;

    case 0x28: case 0x29: case 0x2A: case 0x2B:
        prim_line(d, op, p); break;

    case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        prim_arc(d, op, p, n); break;

    case 0x30: case 0x31: case 0x32: case 0x33:
        prim_rect(d, op, p); break;

    case 0x34: case 0x35: case 0x36: case 0x37:
        prim_poly(d, op, p, n); break;

    case 0x38: exec_field(d, p, n); break;

    case 0x39:      /* INCREMENTAL POINT                                */
    case 0x3A:      /* INCREMENTAL LINE                                 */
    case 0x3B:      /* INCREMENTAL POLYGON (FILLED)                     */
    case 0x3F:      /* BLINK                                            */
        d->st.pdi_skipped++;
        return;

    case 0x3C: exec_set_color(d, p, rep);     break;
    case 0x3D: exec_wait(d, p, n);            break;
    case 0x3E: exec_select_color(d, p, n);    break;

    default:
        d->st.pdi_skipped++;
        return;
    }
    d->st.pdi_executed++;
}

/*
 * Close the open PDI sequence. 5.3.1.1: "A PDI sequence is terminated by
 * an opcode introducing the next PDI sequence or by any other
 * presentation layer code not from the numeric data section of the same
 * PDI set."
 */
static void pdi_terminate(naplps_decoder_t *d)
{
    int op = d->pdi_op;
    const pdi_desc_t *k;
    int need;

    if (op < 0) return;
    d->pdi_op = -1;
    d->reps = 0;

    k = &pdi_desc[op - 0x20];
    need = set_bytes(d, op);

    if (k->mode == M_ACCUM) {
        pdi_execute_set(d, op, d->opbuf, d->oplen, 0);
    } else if (d->oplen > 0) {
        /*
         * A short final operand. 5.3.2.2.5: "If an operand following an
         * opcode is shorter than the length previously specified by the
         * DOMAIN command... trailing zero bits are supplied by the
         * receiving presentation process." So it is padded and executed,
         * not discarded - but it IS counted, because a stream that keeps
         * producing short operands is a stream something is wrong with.
         */
        unsigned char tmp[64];
        int i;
        if (need > (int)sizeof(tmp)) need = (int)sizeof(tmp);
        for (i = 0; i < need; i++)
            tmp[i] = (i < d->oplen) ? d->opbuf[i] : 0x40;
        d->st.operand_short++;
        pdi_execute_set(d, op, tmp, need, 0);
    } else if (k->nfixed == 0 && k->nmulti == 0 && k->nsingle == 0) {
        pdi_execute_set(d, op, d->opbuf, 0, 0);
    } else if (op == 0x20 || op == 0x3E) {
        /* RESET and SELECT COLOR are both defined with no operands:
         * a bare RESET is a no-op and a bare SELECT COLOR means mode 0. */
        pdi_execute_set(d, op, d->opbuf, 0, 0);
    }

    d->oplen = 0;
}

static void pdi_opcode(naplps_decoder_t *d, unsigned char b)
{
    pdi_terminate(d);
    d->pdi_op = b;
    d->oplen = 0;
    d->reps = 0;
}

static void pdi_operand(naplps_decoder_t *d, unsigned char b)
{
    const pdi_desc_t *k;
    int need;

    if (d->pdi_op < 0) return;        /* stray numeric data: no opcode   */

    if (d->oplen < OPBUF_MAX) d->opbuf[d->oplen++] = b;
    else return;                      /* absurdly long run: stop growing */

    k = &pdi_desc[d->pdi_op - 0x20];
    if (k->mode != M_REPEAT) return;

    need = set_bytes(d, d->pdi_op);
    if (need <= 0) return;

    while (d->oplen >= need) {
        /*
         * rep counts how many times this opcode has already executed
         * within the current PDI sequence. Only SET COLOR cares, because
         * 5.3.2.5.1 makes its target colour map address advance on each
         * implicit repeat; every other repeating opcode is stateless
         * across repeats.
         */
        pdi_execute_set(d, d->pdi_op, d->opbuf, need, d->reps);
        d->reps++;
        /* Shift the consumed set out; what remains starts the repeat. */
        memmove(d->opbuf, d->opbuf + need, (size_t)(d->oplen - need));
        d->oplen -= need;
    }
}

/* ------------------------------------------------------------------ */
/* C1 controls                                                         */
/* ------------------------------------------------------------------ */

static int is_def_terminator(int row)
{
    /* DEF MACRO, DEFP MACRO, DEFT MACRO, DEF DRCS, DEF TEXTURE, END */
    return row >= 0 && row <= 5;
}

static void do_c1(naplps_decoder_t *d, int col, int row)
{
    if (col == 4) {
        switch (row) {
        case 0:  /* DEF MACRO   */
        case 2:  /* DEFT MACRO  */
        case 3:  /* DEF DRCS    */
        case 4:  /* DEF TEXTURE */
            /*
             * Excluded by scope. The body is presentation-layer code
             * that must NOT be executed, so it is swallowed until one of
             * the terminating C1 controls arrives (6.2.2.1). This is what
             * 8.3.2's "skipped safely" means for a definition: skipping
             * the introducer alone would execute the body as a picture.
             */
            d->swallow = 1;
            d->st.pdi_skipped++;
            return;
        case 1:  /* DEFP MACRO: defines AND executes, so executing the
                  * body and storing nothing is already correct. */
            d->st.pdi_skipped++;
            return;
        case 5:  /* END */
            d->swallow = 0;
            return;
        case 6:  /* REPEAT */
            d->pend = PEND_REPEAT;
            d->pendlen = 0;
            return;
        case 7: { /* REPEAT TO EOL */
            if (!d->have_last_spacing) { d->st.pdi_skipped++; return; }
            /* Repeat to the last character position along the path. */
            {
                int guard = 0;
                while (guard++ < 256) {
                    long before = (d->path < 2) ? d->cx : d->cy;
                    if (d->path == 0 && d->cx + 2 * cf_w(d) > FX_ONE) break;
                    if (d->path == 1 && d->cx - cf_w(d) < 0) break;
                    if (d->path == 2 && d->cy + 2 * cf_h(d) > VIS_Y_FX) break;
                    if (d->path == 3 && d->cy - cf_h(d) < 0) break;
                    draw_char(d, d->last_spacing_char);
                    if (((d->path < 2) ? d->cx : d->cy) == before) break;
                }
            }
            return;
        }
        case 8:  d->reverse_video = 1; return;
        case 9:  d->reverse_video = 0; return;
        case 10: /* SMALL TEXT. The videotex SRM permits a conforming
                  * terminal to EXECUTE this and not PRESENT it, which is
                  * why 80-column layouts are not safe. It is executed
                  * here, and at 3.2 pixels of cell width the result
                  * demonstrates the warning rather than contradicting it. */
                 d->cf_dx = CF_SMALL_DX;  d->cf_dy = CF_SMALL_DY;  return;
        case 11: d->cf_dx = CF_MEDIUM_DX; d->cf_dy = CF_MEDIUM_DY; return;
        case 12: d->cf_dx = CF_NORMAL_DX; d->cf_dy = CF_NORMAL_DY; return;
        case 13: d->cf_dx = CF_DBLHI_DX;  d->cf_dy = CF_DBLHI_DY;  return;
        case 14: d->st.pdi_skipped++; return;     /* BLINK START */
        case 15: d->cf_dx = CF_DBLSZ_DX;  d->cf_dy = CF_DBLSZ_DY;  return;
        default: return;
        }
    }

    /* Column 5 */
    switch (row) {
    case 0:  d->st.pdi_skipped++; return;   /* PROTECT   */
    case 1: case 2: case 3: case 4:
        /* 6.2.8.3: EDC1..EDC4 "are reserved for future standardization,
         * and are executed as null operations." Not a skip: doing
         * nothing IS the specified behaviour. */
        return;
    case 5:  d->word_wrap = 1; return;
    case 6:  d->word_wrap = 0; return;
    case 7:  d->scroll = 1;    return;
    case 8:  d->scroll = 0;    return;
    case 9:  d->underline = 1; return;
    case 10: d->underline = 0; return;
    case 11: case 12: case 13:
        /* FLASH / STEADY / CURSOR OFF. There is no interactive cursor to
         * show in a viewer, and CURSOR OFF is the default anyway. */
        return;
    case 14: return;                        /* BLINK STOP: nothing blinks */
    case 15: d->st.pdi_skipped++; return;   /* UNPROTECT */
    default: return;
    }
}

/* ------------------------------------------------------------------ */
/* ESC sequences                                                       */
/* ------------------------------------------------------------------ */

static void feed_byte(naplps_decoder_t *d, unsigned char b);

/*
 * 4.3.2, verbatim: "the syntax of an escape sequence is ESC I...I F,
 * where I...I is zero or more occurrences of intermediate characters in
 * the range 2/0 through 2/15 and F is one occurrence of a final
 * character in the range 3/0 through 7/14. The occurrence of any other
 * bit combination in an escape sequence shall cause the partial escape
 * sequence to be terminated and ignored, and that bit combination shall
 * be executed."
 *
 * THIS IS TRAP T2 AND IT IS THE REASON THIS FUNCTION EXISTS SEPARATELY.
 * The intermediate range 0x20-0x2F is byte-for-byte the low half of the
 * PDI opcode range, and the final range 0x30-0x7E covers the rest of the
 * opcodes and the whole operand range. There is NO way to tell an
 * intermediate from an opcode by looking at the byte. The only correct
 * structure is one where ESC takes precedence and consumes its own
 * intermediates and final BEFORE any PDI interpretation happens.
 *
 * Note the error branch: a malformed sequence is discarded and the
 * offending byte is then EXECUTED, not swallowed. That is what lets a
 * terminal resynchronise after a corrupted line instead of wedging.
 */
static void esc_byte(naplps_decoder_t *d, unsigned char b)
{
    if (b >= 0x20 && b <= 0x2F) {           /* intermediate 2/0 - 2/15 */
        if (d->esclen < (int)sizeof(d->escbuf)) d->escbuf[d->esclen++] = b;
        return;
    }

    if (b >= 0x30 && b <= 0x7E) {           /* final 3/0 - 7/14 */
        int i0 = (d->esclen > 0) ? d->escbuf[0] : -1;
        d->escstate = ESC_NONE;

        /* No intermediate: a C1 control, or a locking shift. */
        if (d->esclen == 0) {
            /*
             * Locking shifts, Table 2. LS2 and LS3 exist in BOTH
             * environments; the three "right" forms only make sense in
             * the 8-bit one, where GR exists. The footnote records that
             * "LS1R, LS2R, and LS3R are also coded as ESC 6/11, ESC 6/12,
             * and ESC 6/13, respectively. This dual coding may be removed
             * from this standard in a future revision", so both codings
             * are accepted.
             *
             * These finals do not collide with the G-set designations of
             * Table 1, which all carry an intermediate; this branch is
             * only reached when there is none.
             */
            switch (b) {
            case 0x6E: pdi_terminate(d); d->gl = 2; d->esclen = 0; return; /* LS2  */
            case 0x6F: pdi_terminate(d); d->gl = 3; d->esclen = 0; return; /* LS3  */
            case 0x7E: case 0x6B:
                       pdi_terminate(d); d->gr = 1; d->esclen = 0; return; /* LS1R */
            case 0x7D: case 0x6C:
                       pdi_terminate(d); d->gr = 2; d->esclen = 0; return; /* LS2R */
            case 0x7C: case 0x6D:
                       pdi_terminate(d); d->gr = 3; d->esclen = 0; return; /* LS3R */
            default: break;
            }

            if (b >= 0x40 && b <= 0x5F) {
                int col = (b >> 4) & 0x0F;      /* 4 or 5 */
                int row = b & 0x0F;
                if (d->swallow && !(col == 4 && is_def_terminator(row))) {
                    d->esclen = 0;
                    return;
                }
                pdi_terminate(d);
                do_c1(d, col, row);
            } else {
                d->st.esc_unknown++;
            }
            d->esclen = 0;
            return;
        }

        if (d->swallow) { d->esclen = 0; return; }

        /*
         * ESC 2/5 F: "the entire coding environment ... is to be
         * designated and invoked as a 'complete code'" (4.3.1.2). The
         * standard left F1 unassigned; README.PP3 section 9.0 records
         * what was registered, ESC % A to enter NAPLPS and ESC % @ to
         * return to ASCII text. Any other final is an unknown complete
         * code and is consumed without acting, which is safer than
         * guessing.
         */
        if (i0 == 0x25) {
            pdi_terminate(d);
            if (b == 0x41) {                /* 4/1 'A' */
                d->mode = NAPLPS_MODE_NAPLPS;
            } else if (b == 0x40) {         /* 4/0 '@' */
                /*
                 * Leaving NAPLPS for text starts a FRESH text
                 * presentation: clear to black, default text state,
                 * cursor home. The standard does not define the text
                 * presentation, it is outside NAPLPS; this follows the
                 * terminal. Personality+III under the Subset (ALEXtel)
                 * handshake switches to a separate text screen, and the
                 * real-client gate G7 showed why it matters: the
                 * service's goodbye arrives after a drawn page, and
                 * drawing it at the page's last cursor, in the page's
                 * colours, over the page, printed it through a headline.
                 * A switch while ALREADY in text changes nothing.
                 */
                if (d->mode == NAPLPS_MODE_NAPLPS) begin_text_presentation(d);
                d->mode = NAPLPS_MODE_ASCII;
            } else {
                d->st.esc_unknown++;
            }
            d->esclen = 0;
            return;
        }

        /*
         * Designation of a G-set, Table 1. The intermediate says which
         * slot, the final says which set. Getting these right matters
         * even in a viewer: a stream that redesignates G0 to the PDI set
         * and then relies on SI to select drawing would otherwise be
         * decoded as text.
         */
        {
            int slot = -1;
            gset_t which = GSET_NULL;
            int known = 1;

            switch (i0) {
            case 0x28: slot = 0; break;
            case 0x29: case 0x2D: slot = 1; break;
            case 0x2A: case 0x2E: slot = 2; break;
            case 0x2B: case 0x2F: slot = 3; break;
            case 0x21: /* ESC 2/1 F3: redesignate C0. "The redesignation
                        * of the CO set is not permitted in the context of
                        * this standard and such redesignating escape
                        * sequences shall be ignored." */
                       d->esclen = 0; return;
            case 0x22: /* ESC 2/2 F4: designate the C1 set. The standard
                        * left F4 unassigned; real files use 4/6. There is
                        * only one C1 set here, so this is a no-op, but it
                        * must be CONSUMED rather than executed. */
                       d->esclen = 0; return;
            default:   known = 0; break;
            }

            if (known && slot >= 0) {
                switch (b) {
                case 0x42: which = GSET_PRIMARY; break;   /* 4/2  */
                case 0x7C: which = GSET_SUPP;    break;   /* 7/12 */
                case 0x57: which = GSET_PDI;     break;   /* 5/7  */
                case 0x7D: which = GSET_MOSAIC;  break;   /* 7/13 */
                case 0x7A: which = GSET_MACRO;   break;   /* 7/10 */
                case 0x7B: which = GSET_DRCS;    break;   /* 7/11 */
                default:
                    /* "All other designation sequences shall designate
                     * either a null G- or a null C1-set. A null set is a
                     * set in which all code positions are executed as
                     * null operations." */
                    which = GSET_NULL;
                    break;
                }
                pdi_terminate(d);
                d->gset[slot] = which;
            } else {
                d->st.esc_unknown++;
            }
        }
        d->esclen = 0;
        return;
    }

    /* Out of range: abort the sequence and execute this byte. */
    d->escstate = ESC_NONE;
    d->esclen = 0;
    d->st.esc_malformed++;
    feed_byte(d, b);
}

/* ------------------------------------------------------------------ */
/* Byte dispatch                                                       */
/* ------------------------------------------------------------------ */

static void handle_graphic(naplps_decoder_t *d, unsigned char b, int slot)
{
    gset_t set = d->gset[slot & 3];

    d->single_shift = -1;

    switch (set) {
    case GSET_PRIMARY:
        /*
         * 4.3.1.2: a 94-position set does not include 2/0 and 7/15, and
         * "When such a set is invoked into columns 2 to 7, these two
         * positions shall have the meanings of SPACE and DELETE". The
         * primary set is a 94-character set (Table 1), so here 0x20 IS
         * space and 0x7F IS delete.
         */
        if (b == 0x7F) return;                  /* DELETE: no display  */
        if (b == 0x20) {
            d->last_spacing_char = 0x20;
            d->have_last_spacing = 1;
            /* SPACE in colour mode 2 or reverse video paints the field,
             * so it is drawn rather than merely skipped over. */
            if (d->color_mode == 2 || d->reverse_video || d->underline)
                draw_char(d, 0x20);
            else
                text_advance(d);
            return;
        }
        draw_char(d, b);
        return;

    case GSET_PDI:
        /*
         * THE PDI SET IS A 96-POSITION SET (Table 1). So 2/0 is NOT
         * space and 7/15 is NOT delete: 0x20 is the RESET opcode and
         * 0x7F is an ordinary operand byte. This is exactly where the
         * boom.nap tutorial goes wrong, reading `20 7F 4F` as
         * "Space, Delete, ???" when it is RESET with its two fixed
         * operand bytes. A decoder that attaches SPACE semantics to the
         * byte value rather than to the invoked SET will print a space
         * and then mis-segment everything after it.
         *
         * 5.3.1.1: b7 distinguishes them. "If b7 is set to 0, an opcode
         * is indicated. If b7 is set to 1, numeric data (ie, an operand)
         * is indicated." b7 in the standard's b1..b8 numbering is the
         * 0x40 bit.
         */
        if (b & 0x40) pdi_operand(d, b);
        else          pdi_opcode(d, b);
        return;

    case GSET_SUPP:
    case GSET_MOSAIC:
    case GSET_MACRO:
    case GSET_DRCS:
        /* All excluded by scope. Consumed without display, which is the
         * safe skip 8.3.2 asks for. */
        pdi_terminate(d);
        d->st.pdi_skipped++;
        return;

    default:
        pdi_terminate(d);
        return;     /* null set: every position is a null operation */
    }
}

static void handle_c0(naplps_decoder_t *d, unsigned char b)
{
    /*
     * 5.3.1.1 and 6.1.4/6.1.5/6.1.6.1: the transmission control
     * characters, the device control characters and NUL "have no effect
     * on the presentation layer and, therefore, do not terminate PDI
     * sequences". They are handled FIRST, before anything terminates.
     */
    switch (b) {
    case 0x00:                                  /* NUL   */
    case 0x01: case 0x02: case 0x03: case 0x04: /* SOH..EOT */
    case 0x05: case 0x06:                       /* ENQ ACK  */
    case 0x10:                                  /* DLE   */
    case 0x11: case 0x12: case 0x13: case 0x14: /* DC1..DC4 */
    case 0x15: case 0x16: case 0x17:            /* NAK SYN ETB */
        return;
    default: break;
    }

    if (b == 0x1B) {                            /* ESC, 1/11 */
        d->escstate = ESC_SEEN;
        d->esclen = 0;
        return;
    }

    /* Everything below terminates an open PDI sequence. */
    pdi_terminate(d);

    if (d->swallow) return;

    switch (b) {
    case 0x07: return;                          /* BEL: no sound here    */
    case 0x08: text_bs(d);   return;            /* APB */
    case 0x09: text_advance(d); return;         /* APF */
    case 0x0A: text_lf(d);   return;            /* APD */
    case 0x0B: text_rlf(d);  return;            /* APU */

    case 0x0C:                                  /* CS, 6.1.2.6 */
        /* "moves the cursor to the upper left character position...In
         * color modes 0 and 1, it clears the display area to nominal
         * black. In color mode 2, it clears the display area to the
         * background color." */
        naplps_surface_clear(&d->surf,
            (unsigned char)(d->color_mode == 2 ? d->bg_addr : d->addr_black));
        text_home(d);
        mark_drew(d);
        return;

    case 0x0D: text_cr(d); return;              /* APR */
    case 0x0E: d->gl = 1; return;               /* SO: G1 into GL */
    case 0x0F: d->gl = 0; return;               /* SI: G0 into GL */
    case 0x18: return;                          /* CAN: no macros run    */
    case 0x19: d->single_shift = 2; return;     /* SS2 */
    case 0x1A: return;                          /* SDC: null at this layer */
    case 0x1C: d->pend = PEND_APS; d->pendlen = 0; return;  /* APS */
    case 0x1D: d->single_shift = 3; return;     /* SS3 */
    case 0x1E: text_home(d); return;            /* APH */

    case 0x1F:                                  /* NSR, 6.1.6.5 */
        d->pend = PEND_NSR;
        d->pendlen = 0;
        return;

    default: return;
    }
}

/*
 * NON-SELECTIVE RESET, 6.1.6.5. Note what it does NOT touch: "The
 * programmable masks are not cleared" and "The color map is not
 * changed". That is what distinguishes it from the power-on reset, and
 * getting it wrong would throw away a colour map a frame had just spent
 * bytes loading.
 */
static void nsr_reset(naplps_decoder_t *d)
{
    d->gset[0] = GSET_PRIMARY;
    d->gset[1] = GSET_PDI;
    d->gset[2] = GSET_SUPP;
    d->gset[3] = GSET_MOSAIC;
    d->gl = 0;
    d->gr = 1;
    d->single_shift = -1;

    reset_domain(d);
    reset_text(d);
    reset_texture(d);

    /* "(5) The color mode is set to color mode [0] and the drawing color
     * is set to nominal white. The color map is not changed." The OCR of
     * the reference scan drops the mode number; 0 is the only reading
     * consistent with 5.3.2.6, where mode 0 is the state in which a
     * drawing colour is named directly rather than by map address. */
    d->color_mode = 0;
    d->draw_value.g = NAPLPS_PRIMARY_MAX;
    d->draw_value.r = NAPLPS_PRIMARY_MAX;
    d->draw_value.b = NAPLPS_PRIMARY_MAX;
    find_bw(d);
    d->draw_addr = d->addr_white;
    d->bg_addr = d->addr_black;

    text_home(d);
}

/* A clean text screen for ESC % @: the NSR defaults (which keep the
 * colour map, as NSR does) on a surface cleared to nominal black. */
static void begin_text_presentation(naplps_decoder_t *d)
{
    nsr_reset(d);
    naplps_surface_clear(&d->surf, (unsigned char)d->addr_black);
}

/* APS / NSR cursor addressing, and the REPEAT count byte. */
static void handle_pending(naplps_decoder_t *d, unsigned char b)
{
    /*
     * IN THE 8-BIT ENVIRONMENT THIS BYTE ARRIVES UNMASKED, and every test
     * in this function is defined by the standard on bits b7 to b1, not
     * on the whole byte. 6.2.7.2 says the REPEAT count character must lie
     * "in the range 4/0 through 7/15" reading bits b7 through b1;
     * 6.1.2.4 obtains the APS row "by taking the binary integer
     * comprising bits b7 through b1 with b7 being the MSB, masking out
     * b8". A test written against the raw byte therefore rejects every
     * 8-bit operand out of hand.
     *
     * Gate G6 found this one too. NAPLPS.003 is an 8-bit frame and ends
     * its title with "Presents" followed by a full stop and a REPEAT of
     * four more, encoded as 86 C4. The count byte C4 has bits b7 to b1 of
     * 4/4, which is in range, but as a raw byte it is above 7F, so the
     * command was discarded and the frame rendered "Presents." against
     * Personality+III's "Presents.....".
     *
     * A control still aborts the pending sequence, and in the 8-bit
     * environment a control is column 0 or 1 (C0) or column 8 or 9 (C1).
     */
    int  is_ctrl = (b < 0x20) || (d->eightbit && b >= 0x80 && b <= 0x9F);
    unsigned char v = (unsigned char)(b & 0x7F);

    if (d->pend == PEND_REPEAT) {
        d->pend = PEND_NONE;
        /*
         * 6.2.7.2: "Bits b6 to b1 of the character following the REPEAT
         * character shall be interpreted as the repeat count. Bits b7
         * through b1 of this repeat count character shall be in the range
         * 4/0 through 7/15; otherwise the command is in error and shall
         * be executed as a null operation, and the count character is
         * executed as a character from columns 0 to 3 or 8 to 11."
         */
        if (is_ctrl || v < 0x40) {
            d->st.pdi_skipped++;
            feed_byte(d, b);
            return;
        }
        if (!d->have_last_spacing) { d->st.pdi_skipped++; return; }
        {
            int count = (int)(v & 0x3F), i;
            for (i = 0; i < count; i++) draw_char(d, d->last_spacing_char);
        }
        return;
    }

    /*
     * APS (6.1.2.4) and NSR (6.1.6.5) both take two following bytes as a
     * row and a column, and they are NOT the same addressing scheme.
     * They differ in three ways, and a decoder that shares one routine
     * between them puts text in the wrong place on half the frames:
     *
     *   encoding  APS takes bits b7..b1 and SUBTRACTS 32, giving 0..95.
     *             NSR takes bits b6..b1 directly, giving 0..63.
     *   origin    APS numbers "row 0, column 0, in the lower leftmost
     *             character position". NSR numbers "row 0, column [0] in
     *             the UPPER leftmost character position" with "The top
     *             of the character field for row 0 coincid[ing] with the
     *             top boundary of the display area".
     *   cell size APS uses "the nominal screen format established by the
     *             current character field size". NSR uses "the default
     *             character size".
     *
     * Both bail out on a C0 or C1 control: for APS "the APS is ignored
     * and the CO or C1 control is executed"; for NSR "they terminate the
     * NSR sequence and they are executed", with the reset still done.
     */
    if (is_ctrl) {
        pend_t was = d->pend;
        d->pend = PEND_NONE;
        d->pendlen = 0;
        if (was == PEND_NSR) nsr_reset(d);
        feed_byte(d, b);
        return;
    }

    d->pendbuf[d->pendlen++] = v;
    if (d->pendlen < 2) return;

    {
        pend_t was = d->pend;
        unsigned char b0 = d->pendbuf[0], b1 = d->pendbuf[1];
        long cw, ch;
        int row, col, rows;

        d->pend = PEND_NONE;
        d->pendlen = 0;

        if (was == PEND_NSR) {
            /* The reset happens either way; only the repositioning is
             * conditional on the two bytes being in columns 4 to 7. */
            nsr_reset(d);
            if (b0 < 0x40 || b1 < 0x40) return;   /* cols 2-3: ignored */
            row = (int)(b0 & 0x3F);
            col = (int)(b1 & 0x3F);
            cw = CF_NORMAL_DX;                    /* the DEFAULT size  */
            ch = CF_NORMAL_DY;
            /* Row 0 is the TOP row for NSR. */
            rows = (int)(VIS_Y_FX / ch);
            if (row >= rows) row = rows - 1;
            if (row < 0) row = 0;
            d->cx = (long)col * cw;
            d->cy = VIS_Y_FX - (long)(row + 1) * ch;
        } else {
            if (b0 == 0x7F || b1 == 0x7F) return;
            row = (int)(b0 & 0x7F) - 32;
            col = (int)(b1 & 0x7F) - 32;
            if (row < 0 || col < 0) return;
            cw = cf_w(d);
            ch = cf_h(d);
            /* Row 0 is the BOTTOM row for APS. */
            d->cx = (long)col * cw;
            d->cy = (long)row * ch;
        }

        if (cw > 0 && d->cx + cw > FX_ONE)   d->cx = FX_ONE - cw;
        if (ch > 0 && d->cy + ch > VIS_Y_FX) d->cy = VIS_Y_FX - ch;
        if (d->cx < 0) d->cx = 0;
        if (d->cy < 0) d->cy = 0;
        cursor_moved(d);
    }
}

static void feed_byte(naplps_decoder_t *d, unsigned char b)
{
    int slot;

    /*
     * Environment selection, 4.3.1.4 and the note on d->eightbit. The
     * first byte with bit 8 set puts the decoder into the 8-bit
     * environment for good. Until then the stream is treated as 7-bit,
     * which is what the ALEXtel's Even/7/1 frame delivers.
     */
    if (b & 0x80) d->eightbit = 1;
    if (!d->eightbit) b = (unsigned char)(b & 0x7F);

    if (d->escstate == ESC_SEEN) { esc_byte(d, (unsigned char)(b & 0x7F)); return; }
    if (d->pend != PEND_NONE)    { handle_pending(d, b); return; }

    if (d->mode == NAPLPS_MODE_ASCII) {
        /*
         * ESC % @ handed the line back to plain ASCII text
         * (README.PP3 9.0). Only ESC can get us out, plus the ordinary
         * format effectors, so the viewer still shows text sensibly.
         */
        if (b == 0x1B) { d->escstate = ESC_SEEN; d->esclen = 0; return; }
        if (b < 0x20) { handle_c0(d, b); return; }
        if (b == 0x7F || b >= 0x80) return;
        draw_char(d, b);
        return;
    }

    /* C0, columns 0 and 1, in both environments. */
    if (b < 0x20) { handle_c0(d, b); return; }

    /*
     * The 8-bit in-use table, 4.3.3 and Figure 5: "The in-use table
     * contains the CO set in columns 0 and 1 and the Cl set in columns 8
     * and 9... Columns 2 through 7, which by convention will be called
     * the GL (G-left) area... can accommodate any of the four invoked
     * G-sets. Columns 10 through 15, which will be called the GR
     * (G-right) area, can accommodate the G1, G2, or G3 sets."
     */
    if (d->eightbit && b >= 0x80 && b <= 0x9F) {
        /* A C1 control given directly rather than as ESC Fe. Figure 65's
         * own rule for reading its two columns is that the 8-bit form
         * takes A = 8 and B = 9 where the 7-bit form takes 4 and 5. */
        int col = ((b >> 4) & 0x0F) - 4;        /* 8 -> 4, 9 -> 5 */
        int row = b & 0x0F;
        if (d->swallow && !(col == 4 && is_def_terminator(row))) return;
        pdi_terminate(d);
        do_c1(d, col, row);
        return;
    }

    if (d->swallow) return;

    if (d->eightbit && b >= 0xA0) {
        /* GR. The set lookup uses the low seven bits: the column pair
         * 10-15 maps onto 2-7 of the same set. */
        slot = (d->single_shift >= 0) ? d->single_shift : d->gr;
        handle_graphic(d, (unsigned char)(b & 0x7F), slot);
        return;
    }

    /* GL, columns 2 to 7, in both environments. */
    slot = (d->single_shift >= 0) ? d->single_shift : d->gl;
    handle_graphic(d, b, slot);
}

void naplps_feed(naplps_decoder_t *d, const unsigned char *buf, size_t len)
{
    size_t i;
    if (!d || !buf) return;
    for (i = 0; i < len; i++) {
        d->st.bytes_in++;
        feed_byte(d, buf[i]);
    }
}

void naplps_flush(naplps_decoder_t *d)
{
    if (!d) return;
    pdi_terminate(d);
}
