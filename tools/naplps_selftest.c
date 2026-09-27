/*
 * naplps_selftest.c - offline gate harness for the NAPLPS decoder.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * This is the counterpart of tools/rip_render.c: it drives the same pure
 * decoder and surface the Suite tab drives, with no Win32 involved, so a
 * gate can run without a window.
 *
 *   naplps_selftest              run the G1 self-checks
 *   naplps_selftest FILE.nap OUT.bmp
 *                                decode a file, write a BMP, print a trace
 *   naplps_selftest --boom DIR   run the G3 segmentation gate against
 *                                boom.nap in DIR
 *
 * The G3 fixture lives OUTSIDE this repository and is unlicensed material
 * of unstated provenance (see the Phase 2a report). It is referred to by
 * path and the gate skips itself if the file is absent, so nothing in the
 * build depends on it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "naplps_decode.h"
#include "naplps_surface.h"

static int g_fail = 0;
static int g_pass = 0;

static void ok(int cond, const char *what)
{
    if (cond) { g_pass++; printf("  PASS  %s\n", what); }
    else      { g_fail++; printf("  FAIL  %s\n", what); }
}

/* ------------------------------------------------------------------ */
/* Trace capture                                                       */
/* ------------------------------------------------------------------ */

/*
 * Large enough for the biggest sample frame. It was 256, and the
 * widened gate G2 caught that: nine of the thirteen comparison files
 * execute more than 256 PDI commands, so the trace silently stopped
 * at the prefix while the executed= summary went on reporting the
 * true total. That is a harness defect, not a decoder one, but it
 * made a cross-check look complete when it had compared half a file.
 */
#define TRACE_MAX 4096
typedef struct {
    int op[TRACE_MAX];
    int nb[TRACE_MAX];
    unsigned char ob[TRACE_MAX][12];
    int n;
} trace_t;

static void trace_cb(void *ctx, int opcode, const unsigned char *operands,
                     int nbytes)
{
    trace_t *t = (trace_t *)ctx;
    if (t->n < TRACE_MAX) {
        int k = nbytes < 12 ? nbytes : 12, i;
        t->op[t->n] = opcode; t->nb[t->n] = nbytes;
        for (i = 0; i < k; i++) t->ob[t->n][i] = operands[i];
        for (; i < 12; i++) t->ob[t->n][i] = 0;
        t->n++;
    }
}

static const char *opname(int op)
{
    static const char *n[32] = {
        "RESET","DOMAIN","TEXT","TEXTURE",
        "POINT SET ABS","POINT SET REL","POINT ABS","POINT REL",
        "LINE ABS","LINE REL","SET&LINE ABS","SET&LINE REL",
        "ARC OUT","ARC FILL","SET&ARC OUT","SET&ARC FILL",
        "RECT OUT","RECT FILL","SET&RECT OUT","SET&RECT FILL",
        "POLY OUT","POLY FILL","SET&POLY OUT","SET&POLY FILL",
        "FIELD","INCR POINT","INCR LINE","INCR POLY FILL",
        "SET COLOR","WAIT","SELECT COLOR","BLINK"
    };
    if (op < 0x20 || op > 0x3F) return "?";
    return n[op - 0x20];
}

/* ------------------------------------------------------------------ */
/* BMP out                                                             */
/* ------------------------------------------------------------------ */

static int write_bmp(const char *path, const naplps_surface_t *s, int mono)
{
    static unsigned char bgra[NAPLPS_W * NAPLPS_H * 4];
    unsigned char hdr[54];
    FILE *f;
    long datasz = (long)NAPLPS_W * NAPLPS_H * 4;
    long filesz = 54 + datasz;
    int y;

    naplps_surface_to_bgra(s, bgra, mono);

    memset(hdr, 0, sizeof(hdr));
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=(unsigned char)(filesz); hdr[3]=(unsigned char)(filesz>>8);
    hdr[4]=(unsigned char)(filesz>>16); hdr[5]=(unsigned char)(filesz>>24);
    hdr[10]=54;
    hdr[14]=40;
    hdr[18]=(unsigned char)(NAPLPS_W); hdr[19]=(unsigned char)(NAPLPS_W>>8);
    /* Positive height = bottom-up BMP, which is what our surface already
     * is; to_bgra has flipped to top-down, so write the rows in reverse. */
    hdr[22]=(unsigned char)(NAPLPS_H); hdr[23]=(unsigned char)(NAPLPS_H>>8);
    hdr[26]=1;
    hdr[28]=32;
    hdr[34]=(unsigned char)(datasz); hdr[35]=(unsigned char)(datasz>>8);
    hdr[36]=(unsigned char)(datasz>>16); hdr[37]=(unsigned char)(datasz>>24);

    f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(hdr, 1, 54, f);
    for (y = NAPLPS_H - 1; y >= 0; y--)
        fwrite(bgra + (size_t)y * NAPLPS_W * 4, 1, NAPLPS_W * 4, f);
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------------ */
/* G1: decoder self-checks                                             */
/* ------------------------------------------------------------------ */

/*
 * T1. The DOMAIN length fields are BIT PATTERNS and the count is the
 * pattern PLUS ONE (Tables 4 and 5). All four single-value patterns and
 * all eight multi-value patterns are walked, because the one that will
 * be got wrong is the DEFAULT, pattern 010 for three bytes, and a test
 * that only checks the defaults would pass with the bug present.
 *
 * The count is not exposed directly, so it is measured the way it
 * actually bites: a POINT SET ABS followed by exactly one multi-value
 * operand must execute exactly once and consume exactly that many bytes.
 */
static void t1_domain_lengths(void)
{
    int mv_pat, sv_pat;

    printf("\nG1.T1  DOMAIN length fields are bit patterns, count = pattern + 1\n");

    for (mv_pat = 0; mv_pat < 8; mv_pat++) {
        naplps_decoder_t *d = naplps_create();
        trace_t tr; unsigned char buf[32]; int n = 0, i;
        int expect = mv_pat + 1;
        char msg[96];

        memset(&tr, 0, sizeof(tr));
        naplps_set_trace(d, trace_cb, &tr);

        buf[n++] = 0x0E;                          /* SO: invoke the PDI set */
        buf[n++] = 0x21;                          /* DOMAIN                 */
        buf[n++] = (unsigned char)(0x40 | (mv_pat << 2));
        buf[n++] = 0x24;                          /* POINT SET ABS          */
        for (i = 0; i < expect; i++) buf[n++] = 0x60;
        buf[n++] = 0x0F;                          /* SI terminates it       */

        naplps_feed(d, buf, (size_t)n);
        naplps_flush(d);

        sprintf(msg, "multi-value pattern %d%d%d -> %d bytes",
                (mv_pat>>2)&1, (mv_pat>>1)&1, mv_pat&1, expect);
        {
            int found = 0, j;
            for (j = 0; j < tr.n; j++)
                if (tr.op[j] == 0x24 && tr.nb[j] == expect) found++;
            ok(found == 1 && tr.n == 2, msg);
        }
        naplps_destroy(d);
    }

    for (sv_pat = 0; sv_pat < 4; sv_pat++) {
        naplps_decoder_t *d = naplps_create();
        trace_t tr; unsigned char buf[32]; int n = 0, i;
        int expect = sv_pat + 1;
        char msg[96];

        memset(&tr, 0, sizeof(tr));
        naplps_set_trace(d, trace_cb, &tr);

        buf[n++] = 0x0E;
        buf[n++] = 0x21;                          /* DOMAIN */
        buf[n++] = (unsigned char)(0x40 | sv_pat);
        buf[n++] = 0x3E;                          /* SELECT COLOR */
        for (i = 0; i < expect; i++) buf[n++] = 0x7C;
        buf[n++] = 0x0F;

        naplps_feed(d, buf, (size_t)n);
        naplps_flush(d);

        sprintf(msg, "single-value pattern %d%d -> %d bytes (one operand = mode 1)",
                (sv_pat>>1)&1, sv_pat&1, expect);
        {
            int j, found = 0;
            for (j = 0; j < tr.n; j++)
                if (tr.op[j] == 0x3E && tr.nb[j] == expect) found = 1;
            ok(found, msg);
        }
        naplps_destroy(d);
    }

    /* The specific cruelty of the trap: the DEFAULT multi-value length is
     * three bytes and its bit pattern is 010. An implementation that
     * conflates the two reads 010 as 2 and is wrong on a file that only
     * ever uses the defaults. */
    {
        naplps_decoder_t *d = naplps_create();
        trace_t tr;
        unsigned char buf[] = { 0x0E, 0x24, 0x60, 0x60, 0x60, 0x0F };
        memset(&tr, 0, sizeof(tr));
        naplps_set_trace(d, trace_cb, &tr);
        naplps_feed(d, buf, sizeof(buf));
        naplps_flush(d);
        ok(tr.n == 1 && tr.op[0] == 0x24 && tr.nb[0] == 3,
           "default multi-value length with no DOMAIN at all is 3 bytes");
        naplps_destroy(d);
    }
}

/*
 * T2. An ESC sequence must be consumed as a unit BEFORE any PDI
 * interpretation, because the ISO 2022 intermediate range 2/0 to 2/15 is
 * byte-for-byte the low half of the PDI opcode range and the final range
 * covers the rest.
 *
 * The decisive case: an ESC whose FINAL character is a valid PDI opcode
 * byte. ESC 2/8 4/2 designates the primary set into G0. Its final, 0x42,
 * is not an opcode, so the sharper case is built deliberately: ESC 2/9
 * 5/7 designates the PDI set into G1, and 0x57 is in the operand range.
 * And ESC 2/0 ... uses 0x20, which IS the RESET opcode, as an
 * intermediate.
 */
static void t2_esc_before_pdi(void)
{
    printf("\nG1.T2  ESC sequences are consumed as units before PDI interpretation\n");

    /* 0x20 as an ESC INTERMEDIATE must not execute RESET. The screen is
     * pre-dirtied so that a wrongly executed RESET-to-black is visible. */
    {
        naplps_decoder_t *d = naplps_create();
        trace_t tr;
        /* SO, SET&RECT FILLED covering the screen, then ESC 2/0 4/0 */
        unsigned char buf[] = {
            0x0E,
            0x33, 0x40,0x40,0x40, 0x7F,0x7F,0x7F,   /* fill something */
            0x1B, 0x20, 0x40                        /* ESC 2/0 4/0    */
        };
        memset(&tr, 0, sizeof(tr));
        naplps_set_trace(d, trace_cb, &tr);
        naplps_feed(d, buf, sizeof(buf));
        naplps_flush(d);
        {
            int j, reset_seen = 0;
            for (j = 0; j < tr.n; j++) if (tr.op[j] == 0x20) reset_seen = 1;
            ok(!reset_seen,
               "0x20 as an ESC intermediate does NOT execute as RESET");
        }
        naplps_destroy(d);
    }

    /* A final that is a valid opcode byte: ESC 2/1 3/0. 0x30 is the
     * RECT OUTLINED opcode. It must be eaten as the final of a C0
     * redesignation, which the standard says to ignore. */
    {
        naplps_decoder_t *d = naplps_create();
        trace_t tr;
        unsigned char buf[] = { 0x0E, 0x1B, 0x21, 0x30, 0x0F };
        memset(&tr, 0, sizeof(tr));
        naplps_set_trace(d, trace_cb, &tr);
        naplps_feed(d, buf, sizeof(buf));
        naplps_flush(d);
        ok(tr.n == 0,
           "0x30 as an ESC final does NOT execute as RECT OUTLINED");
        naplps_destroy(d);
    }

    /* A C1 control reached as ESC 4/x must not be read as PDI data:
     * ESC 4/12 is NORMAL TEXT, and 0x4C is inside the operand range. */
    {
        naplps_decoder_t *d = naplps_create();
        trace_t tr;
        unsigned char buf[] = { 0x0E, 0x24, 0x60,0x60,0x60, 0x1B, 0x4C, 0x0F };
        memset(&tr, 0, sizeof(tr));
        naplps_set_trace(d, trace_cb, &tr);
        naplps_feed(d, buf, sizeof(buf));
        naplps_flush(d);
        ok(tr.n == 1 && tr.op[0] == 0x24 && tr.nb[0] == 3,
           "ESC 4/12 (NORMAL TEXT) is not swallowed as POINT SET operand data");
        naplps_destroy(d);
    }

    /*
     * The error branch of 4.3.2: "The occurrence of any other bit
     * combination in an escape sequence shall cause the partial escape
     * sequence to be terminated and ignored, and that bit combination
     * shall be executed." A C0 control inside an ESC sequence must
     * therefore be EXECUTED, not eaten.
     */
    {
        naplps_decoder_t *d = naplps_create();
        const naplps_stats_t *st;
        /* ESC 2/0 then SO: SO is 0x0E, out of both ranges. */
        unsigned char buf[] = { 0x1B, 0x20, 0x0E, 0x24, 0x60,0x60,0x60 };
        trace_t tr;
        memset(&tr, 0, sizeof(tr));
        naplps_set_trace(d, trace_cb, &tr);
        naplps_feed(d, buf, sizeof(buf));
        naplps_flush(d);
        st = naplps_get_stats(d);
        ok(st->esc_malformed == 1,
           "a malformed ESC sequence is counted");
        ok(tr.n == 1 && tr.op[0] == 0x24,
           "the byte that aborted the ESC sequence is then EXECUTED (the SO "
           "took effect, so the POINT SET that followed was seen)");
        naplps_destroy(d);
    }
}

/*
 * The PDI set is a 96-CHARACTER set, so 2/0 is the RESET opcode and not
 * SPACE while it is invoked. This is the reading that the boom.nap
 * tutorial gets wrong, and getting it wrong mis-segments the rest of the
 * stream, so it is asserted directly.
 */
static void t_96set(void)
{
    naplps_decoder_t *d = naplps_create();
    trace_t tr;
    unsigned char buf[] = { 0x0E, 0x20, 0x7F, 0x4F, 0x0F };

    printf("\nG1     the PDI set is a 96-character set: 2/0 is RESET, not SPACE\n");
    memset(&tr, 0, sizeof(tr));
    naplps_set_trace(d, trace_cb, &tr);
    naplps_feed(d, buf, sizeof(buf));
    naplps_flush(d);
    ok(tr.n == 1 && tr.op[0] == 0x20 && tr.nb[0] == 2,
       "SO 20 7F 4F decodes as RESET with two fixed operand bytes");
    ok(naplps_get_stats(d)->chars_drawn == 0,
       "no SPACE character was drawn");
    naplps_destroy(d);

    /* The same byte WITH the primary set invoked really is SPACE. */
    {
        naplps_decoder_t *e = naplps_create();
        unsigned char b2[] = { 0x0F, 0x20, 0x41 };
        naplps_feed(e, b2, sizeof(b2));
        naplps_flush(e);
        ok(naplps_get_stats(e)->chars_drawn >= 1,
           "with the primary (94-character) set invoked, 2/0 is SPACE");
        naplps_destroy(e);
    }
}

/* T5 and the colour model. */
static void t_colour(void)
{
    static const unsigned char expect[16][3] = {   /* G, R, B */
        {0,0,0},{1,1,1},{2,2,2},{3,3,3},{4,4,4},{5,5,5},{6,6,6},{7,7,7},
        {0,0,7},{0,5,7},{0,7,4},{2,7,0},{7,7,0},{7,2,0},{7,0,4},{5,0,7}
    };
    naplps_decoder_t *d = naplps_create();
    const naplps_surface_t *s = naplps_get_surface(d);
    int i, allok = 1;

    printf("\nG1.T5  the default colour map matches Figure 25 (N=4, M=9)\n");

    for (i = 0; i < NAPLPS_CMAP_SIZE; i++)
        if (s->cmap[i].g != expect[i][0] ||
            s->cmap[i].r != expect[i][1] ||
            s->cmap[i].b != expect[i][2]) {
            printf("        entry %2d: got %d,%d,%d want %d,%d,%d\n", i,
                   s->cmap[i].g, s->cmap[i].r, s->cmap[i].b,
                   expect[i][0], expect[i][1], expect[i][2]);
            allok = 0;
        }
    ok(allok, "all 16 default colour map entries");

    /* The grey half is a monotonic luminance ramp; the hue half is not.
     * That non-monotonicity is the whole reason the monochrome view
     * exists, so it is asserted rather than assumed. */
    {
        int mono_ok = 1, hue_monotonic = 1;
        for (i = 1; i < 8; i++)
            if (naplps_color_luma(&s->cmap[i]) <=
                naplps_color_luma(&s->cmap[i-1])) mono_ok = 0;
        for (i = 9; i < 16; i++)
            if (naplps_color_luma(&s->cmap[i]) <=
                naplps_color_luma(&s->cmap[i-1])) hue_monotonic = 0;
        ok(mono_ok, "grey half 0..7 is a strictly increasing luminance ramp");
        ok(!hue_monotonic,
           "hue half 8..15 is NOT luminance-ordered, which is what the "
           "monochrome view exists to show");
    }
    naplps_destroy(d);

    /*
     * SELECT COLOR's map address is LEFT JUSTIFIED in the single-value
     * operand (5.3.2.6.1): with the default one-byte operand and N = 4
     * the decoder "responds to b6 through b3 and ignores b2 and b1". The
     * naive read of the low four bits would give a different answer for
     * any operand whose low bits are set, so the test picks one.
     */
    {
        naplps_decoder_t *e = naplps_create();
        const naplps_surface_t *es;
        /*
         * 0x73 -> payload 110011. Top four bits = 1100 = 12, which is
         * yellow in the default map. The low four bits would be 0011 = 3,
         * a mid grey, so the two readings are easy to tell apart.
         *
         * Then RECT FILLED with a 0.25 x 0.25 extent from the drawing
         * point at the origin. The extent is encoded by hand: with the
         * default 3-byte multi-value operand each axis gets 9 bits, and
         * 0.25 is raw 010000000, which splits three bits at a time into
         * the b6 b5 b4 (X) and b3 b2 b1 (Y) halves of the three bytes.
         * A zero extent would fill nothing and the test would pass or
         * fail for the wrong reason.
         */
        unsigned char buf[] = { 0x0E, 0x3E, 0x73,
                                0x31, 0x52, 0x40, 0x40, 0x0F };
        int px;
        naplps_feed(e, buf, sizeof(buf));
        naplps_flush(e);
        es = naplps_get_surface(e);
        px = es->pix[0];
        ok(px == 12,
           "SELECT COLOR address is the TOP N bits (0x73 -> 12, not 3)");
        naplps_destroy(e);
    }
}

/* Structural invariants the decoder should never violate. */
static void t_structural(void)
{
    naplps_decoder_t *d = naplps_create();
    const naplps_surface_t *s;
    int i, inrange = 1;
    unsigned char buf[512];
    int n = 0;

    printf("\nG1     structural invariants\n");

    /* Feed every byte value 0x00..0xFF twice through, in graphics mode
     * and in text mode, and assert nothing escapes the surface or the
     * colour map. This is the cheap fuzz that catches an unclamped
     * index. */
    buf[n++] = 0x0E;
    for (i = 0; i < 256; i++) buf[n++] = (unsigned char)i;
    buf[n++] = 0x0F;
    for (i = 0; i < 200; i++) buf[n++] = (unsigned char)i;

    naplps_feed(d, buf, (size_t)n);
    naplps_flush(d);

    s = naplps_get_surface(d);
    for (i = 0; i < NAPLPS_W * NAPLPS_H; i++)
        if (s->pix[i] >= NAPLPS_CMAP_SIZE) inrange = 0;
    ok(inrange, "every pixel holds a valid colour map address after a "
                "full-byte-range fuzz");

    for (i = 0; i < NAPLPS_CMAP_SIZE; i++)
        if (s->cmap[i].g > 7 || s->cmap[i].r > 7 || s->cmap[i].b > 7)
            inrange = 0;
    ok(inrange, "every colour map component stays within 3 bits");

    naplps_destroy(d);

    /* Definition bodies must be swallowed, not executed. DEF MACRO is
     * ESC 4/0; the body here would otherwise fill the screen. */
    {
        naplps_decoder_t *e = naplps_create();
        trace_t tr;
        unsigned char b[] = {
            0x0E,
            0x1B, 0x40, 0x41,                      /* DEF MACRO, name 'A'  */
            0x33, 0x40,0x40,0x40, 0x7F,0x7F,0x7F,  /* body: a big fill     */
            0x1B, 0x45,                            /* END                  */
            0x24, 0x60,0x60,0x60                   /* and a real command   */
        };
        memset(&tr, 0, sizeof(tr));
        naplps_set_trace(e, trace_cb, &tr);
        naplps_feed(e, b, sizeof(b));
        naplps_flush(e);
        {
            int j, body_ran = 0;
            for (j = 0; j < tr.n; j++) if (tr.op[j] == 0x33) body_ran = 1;
            ok(!body_ran,
               "a DEF MACRO body is swallowed, not executed as a picture");
            ok(tr.n == 1 && tr.op[0] == 0x24,
               "decoding resumes normally after END");
        }
        naplps_destroy(e);
    }
}

/* ------------------------------------------------------------------ */
/* G3: boom.nap segmentation                                           */
/* ------------------------------------------------------------------ */

/*
 * The expected segmentation, taken from the standard and cross-checked
 * against boom_cmds.txt, the repository author's independent annotation.
 * Operand byte counts are included because the whole point of the gate is
 * that a wrong DOMAIN length produces a wrong SEGMENTATION, not an error.
 */
typedef struct { int op; int nb; } expect_t;

static const expect_t boom_expect[] = {
    { 0x20,  2 },   /* RESET,  two fixed operand bytes                   */
    { 0x21,  5 },   /* DOMAIN, 1 fixed + a 4-byte logical pel            */
    { 0x23,  5 },   /* TEXTURE, 1 fixed + one 4-byte multi-value operand */
    { 0x3E,  4 },   /* SELECT COLOR, two 2-byte operands -> colour mode 2*/
    { 0x37, 88 },   /* SET & POLY FILLED, 22 coordinates of 4 bytes      */
    { 0x3E,  4 },
    { 0x37, 80 },   /* 20 coordinates                                    */
    { 0x3E,  4 },
    { 0x37, 76 },   /* 19 coordinates                                    */
    { 0x3E,  4 },
    { 0x22,  6 },   /* TEXT, 2 fixed + one 4-byte multi-value operand    */
    { 0x24,  4 }    /* POINT SET (ABS), one 4-byte coordinate            */
};
#define BOOM_N ((int)(sizeof(boom_expect)/sizeof(boom_expect[0])))

static int gate_boom(const char *dir)
{
    char path[1024];
    FILE *f;
    static unsigned char data[8192];
    size_t len;
    naplps_decoder_t *d;
    trace_t tr;
    int i, seg_ok = 1;

    sprintf(path, "%s/boom.nap", dir);
    f = fopen(path, "rb");
    if (!f) {
        printf("\nG3     SKIPPED: %s not present\n", path);
        printf("       The fixture lives outside the repository on purpose.\n");
        return 0;
    }
    len = fread(data, 1, sizeof(data), f);
    fclose(f);

    printf("\nG3     boom.nap segmentation (%lu bytes)\n", (unsigned long)len);
    ok(len == 310, "boom.nap is 310 bytes, as Phase 1 records");

    d = naplps_create();
    memset(&tr, 0, sizeof(tr));
    naplps_set_trace(d, trace_cb, &tr);
    naplps_feed(d, data, len);
    naplps_flush(d);

    printf("       decoder produced %d PDI executions:\n", tr.n);
    for (i = 0; i < tr.n; i++)
        printf("         %2d  0x%02X %-16s %3d operand bytes\n",
               i, tr.op[i], opname(tr.op[i]), tr.nb[i]);

    if (tr.n != BOOM_N) seg_ok = 0;
    for (i = 0; i < BOOM_N && i < tr.n; i++)
        if (tr.op[i] != boom_expect[i].op || tr.nb[i] != boom_expect[i].nb) {
            printf("       MISMATCH at %d: got 0x%02X/%d want 0x%02X/%d\n",
                   i, tr.op[i], tr.nb[i],
                   boom_expect[i].op, boom_expect[i].nb);
            seg_ok = 0;
        }
    ok(seg_ok, "segmentation matches the annotation in boom_cmds.txt");

    ok(naplps_get_stats(d)->chars_drawn == 4,
       "the four characters of \"BOOM\" were drawn after SI");

    {
        char out[1024];
        sprintf(out, "%s/boom_suite.bmp", dir);
        if (write_bmp(out, naplps_get_surface(d), 0))
            printf("       wrote %s\n", out);
        sprintf(out, "%s/boom_suite_mono.bmp", dir);
        if (write_bmp(out, naplps_get_surface(d), 1))
            printf("       wrote %s\n", out);
    }

    naplps_destroy(d);
    return 1;
}

/* ------------------------------------------------------------------ */

static int decode_file(const char *in, const char *out, int mono)
{
    FILE *f;
    static unsigned char data[1 << 20];
    size_t len;
    naplps_decoder_t *d;
    trace_t tr;
    int i;
    const naplps_stats_t *st;

    f = fopen(in, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", in); return 1; }
    len = fread(data, 1, sizeof(data), f);
    fclose(f);

    d = naplps_create();
    memset(&tr, 0, sizeof(tr));
    naplps_set_trace(d, trace_cb, &tr);
    naplps_feed(d, data, len);
    naplps_flush(d);

    printf("%s: %lu bytes, %d PDI executions\n", in, (unsigned long)len, tr.n);
    for (i = 0; i < tr.n; i++) {
        int k = tr.nb[i] < 12 ? tr.nb[i] : 12, j;
        printf("  %3d  0x%02X %-16s %4d operand bytes :",
               i, tr.op[i], opname(tr.op[i]), tr.nb[i]);
        for (j = 0; j < k; j++) printf(" %02X", tr.ob[i][j]);
        printf("\n");
    }

    st = naplps_get_stats(d);
    printf("  chars=%lu executed=%lu skipped=%lu esc_unknown=%lu "
           "esc_malformed=%lu short=%lu waits=%lu wait_bad=%lu\n",
           st->chars_drawn, st->pdi_executed, st->pdi_skipped,
           st->esc_unknown, st->esc_malformed, st->operand_short,
           st->waits, st->waits_malformed);

    if (!write_bmp(out, naplps_get_surface(d), mono)) {
        fprintf(stderr, "cannot write %s\n", out);
        naplps_destroy(d);
        return 1;
    }
    printf("  wrote %s\n", out);
    naplps_destroy(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Findings of the 2026-09-26 real-client gates                        */
/* ------------------------------------------------------------------ */

/* Is surface pixel (x, y), y counted from the BOTTOM, anything but black? */
static int lit(const naplps_surface_t *s, int x, int y)
{
    return s->pix[y * NAPLPS_W + x] != 0;
}

/* A 32x32 filled rectangle at the origin under TEXTURE byte tex, with the
 * logical pel either left at its default (0,0) or set to one pixel. */
static naplps_decoder_t *hatch_rect(unsigned char tex, int one_pixel_pel)
{
    naplps_decoder_t *d = naplps_create();
    unsigned char b[32];
    int n = 0;
    b[n++] = 0x0E;                                   /* SO: PDI set       */
    if (one_pixel_pel) {
        /* DOMAIN, default 0x48, logical pel (1/256, 1/256): with the
         * 3-byte operand each axis is 9 bits over 2^8, so k = 1. */
        b[n++] = 0x21; b[n++] = 0x48;
        b[n++] = 0x40; b[n++] = 0x40; b[n++] = 0x49;
    }
    b[n++] = 0x23; b[n++] = tex;                     /* TEXTURE           */
    b[n++] = 0x33;                                   /* SET & RECT FILLED */
    b[n++] = 0x40; b[n++] = 0x40; b[n++] = 0x40;     /* at (0, 0)         */
    b[n++] = 0x40; b[n++] = 0x64; b[n++] = 0x40;     /* 32/256 square     */
    b[n++] = 0x0F;
    naplps_feed(d, b, (size_t)n);
    naplps_flush(d);
    return d;
}

static void t_gates_20260926(void)
{
    naplps_decoder_t *d;
    const naplps_surface_t *s;

    printf("\nG7     Table 13 texture patterns (found against Personality+III)\n");

    d = hatch_rect(0x48, 1);     /* b6 b5 b4 = 001 */
    s = naplps_get_surface(d);
    ok(lit(s, 0, 0) && !lit(s, 1, 0) && lit(s, 2, 0) && lit(s, 0, 1) && !lit(s, 1, 1),
       "pattern 001 is VERTICAL hatching, one pel on, one pel off");
    naplps_destroy(d);

    d = hatch_rect(0x50, 1);     /* 010 */
    s = naplps_get_surface(d);
    ok(lit(s, 0, 0) && lit(s, 1, 0) && !lit(s, 0, 1) && lit(s, 0, 2),
       "pattern 010 is HORIZONTAL hatching");
    naplps_destroy(d);

    d = hatch_rect(0x58, 1);     /* 011 */
    s = naplps_get_surface(d);
    ok(lit(s, 1, 0) && lit(s, 0, 1) && !lit(s, 1, 1) && lit(s, 2, 2),
       "pattern 011 is CROSS-hatching, both sets of lines");
    naplps_destroy(d);

    d = hatch_rect(0x60, 1);     /* 100, mask A, never defined */
    s = naplps_get_surface(d);
    ok(!lit(s, 0, 0) && !lit(s, 5, 5),
       "an undefined programmable mask is a null pattern: no fill in colour mode 0");
    naplps_destroy(d);

    d = hatch_rect(0x48, 0);     /* 001 at the default (0,0) pel */
    s = naplps_get_surface(d);
    ok(lit(s, 0, 0) && lit(s, 1, 0) && lit(s, 1, 1),
       "a hatch under a (0,0) logical pel draws SOLID (5.3.2.4.4)");
    naplps_destroy(d);

    printf("\nG7     Table D1 visual font baseline\n");
    {
        /* POINT SET ABS (0,0), SI, 'H' at NORMAL TEXT, a 10-pixel field. */
        unsigned char b[] = { 0x0E, 0x24, 0x40, 0x40, 0x40, 0x0F, 'H' };
        d = naplps_create();
        naplps_feed(d, b, sizeof b);
        naplps_flush(d);
        s = naplps_get_surface(d);
        ok(!lit(s, 0, 0) && !lit(s, 0, 1) && lit(s, 0, 2) && lit(s, 0, 8) && !lit(s, 0, 9),
           "an upper case glyph stands 20% of dy (2 px) above the field origin");
        naplps_destroy(d);
    }

    printf("\nG7     ESC %% @ begins a fresh text presentation\n");
    {
        unsigned char pic[] = { 0x0E, 0x33, 0x40, 0x40, 0x40, 0x7F, 0x7F, 0x7F,
                                0x0F, 0x1B, 0x25, 0x40 };
        unsigned char txt[] = { 0x1B, 0x25, 0x40, 'H', 0x1B, 0x25, 0x40 };
        int any = 0, i;

        d = naplps_create();
        naplps_feed(d, pic, sizeof pic);
        s = naplps_get_surface(d);
        for (i = 0; i < NAPLPS_W * NAPLPS_H; i++) if (s->pix[i]) any = 1;
        ok(!any && naplps_get_mode(d) == NAPLPS_MODE_ASCII,
           "leaving NAPLPS clears the picture instead of printing over it");
        naplps_destroy(d);

        d = naplps_create();
        naplps_feed(d, txt, sizeof txt);
        s = naplps_get_surface(d);
        for (i = 0, any = 0; i < NAPLPS_W * NAPLPS_H; i++) if (s->pix[i]) any = 1;
        ok(any, "ESC % @ while already in text does not wipe the text");
        naplps_destroy(d);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--boom") == 0) {
        gate_boom(argv[2]);
    } else if (argc >= 3) {
        int mono = (argc >= 4 && strcmp(argv[3], "--mono") == 0);
        return decode_file(argv[1], argv[2], mono);
    } else {
        printf("NAPLPS decoder self-check (gate G1)\n");
        t1_domain_lengths();
        t2_esc_before_pdi();
        t_96set();
        t_colour();
        t_structural();
        t_gates_20260926();
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
