/*
 * rip_render.c - Offline RIPscrip renderer for the oracle diff harness.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Reads a captured RIP payload stream (the bytes after telnet IAC
 * removal and after the RIPSCRIP handshake), renders it through exactly
 * the same rip_ega + rip_parser code the Suite tab uses, and writes a
 * 24-bit BMP plus a statistics line. The PowerShell harness
 * (verify_ripscrip_diff.ps1) converts the BMP to PNG and pixel-diffs it
 * against a reference capture from a black-box oracle.
 *
 * Usage:  rip_render <capture.rip> <out.bmp> [--aspect]
 *
 *   --aspect  scale 640x350 up to 640x480 with nearest-neighbour, which
 *             is the authentic EGA display aspect. Without it the BMP is
 *             the raw 640x350 surface.
 *
 * Build:  gcc -O2 -Isrc -o tools/rip_render.exe tools/rip_render.c \
 *             src/rip_ega.c src/rip_parser.c
 */

#include "rip_ega.h"
#include "rip_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Write a bottom-up 24-bit BMP from a top-down BGRA buffer. */
static int write_bmp(const char *path, const unsigned char *bgra,
                     int w, int h)
{
    FILE *f;
    int row_bytes = w * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    int stride = row_bytes + pad;
    unsigned long pixels = (unsigned long)stride * h;
    unsigned long filesz = 54 + pixels;
    unsigned char hdr[54];
    unsigned char zero[3] = { 0, 0, 0 };
    int y, x;

    f = fopen(path, "wb");
    if (!f) return 0;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (unsigned char)(filesz      ); hdr[3] = (unsigned char)(filesz >>  8);
    hdr[4] = (unsigned char)(filesz >> 16); hdr[5] = (unsigned char)(filesz >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (unsigned char)(w      ); hdr[19] = (unsigned char)(w >>  8);
    hdr[20] = (unsigned char)(w >> 16); hdr[21] = (unsigned char)(w >> 24);
    hdr[22] = (unsigned char)(h      ); hdr[23] = (unsigned char)(h >>  8);
    hdr[24] = (unsigned char)(h >> 16); hdr[25] = (unsigned char)(h >> 24);
    hdr[26] = 1;
    hdr[28] = 24;
    hdr[34] = (unsigned char)(pixels      ); hdr[35] = (unsigned char)(pixels >>  8);
    hdr[36] = (unsigned char)(pixels >> 16); hdr[37] = (unsigned char)(pixels >> 24);
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = h - 1; y >= 0; y--) {
        for (x = 0; x < w; x++) {
            const unsigned char *px = bgra + ((size_t)y * w + x) * 4;
            fwrite(px, 1, 3, f);      /* BGR, already in that order */
        }
        if (pad) fwrite(zero, 1, (size_t)pad, f);
    }
    fclose(f);
    return 1;
}

/* Nearest-neighbour vertical scale to the 4:3 EGA display aspect. */
static unsigned char *aspect_correct(const unsigned char *src, int *out_h)
{
    const int dst_h = 480;
    unsigned char *dst = (unsigned char *)malloc((size_t)RIP_EGA_W * dst_h * 4);
    int y;
    if (!dst) return NULL;
    for (y = 0; y < dst_h; y++) {
        int sy = (int)((long)y * RIP_EGA_H / dst_h);
        if (sy >= RIP_EGA_H) sy = RIP_EGA_H - 1;
        memcpy(dst + (size_t)y * RIP_EGA_W * 4,
               src + (size_t)sy * RIP_EGA_W * 4,
               (size_t)RIP_EGA_W * 4);
    }
    *out_h = dst_h;
    return dst;
}

int main(int argc, char **argv)
{
    const char *in_path, *out_path;
    int do_aspect = 0, i;
    FILE *f;
    long size;
    unsigned char *buf, *bgra, *img;
    int img_h = RIP_EGA_H;
    static RipEga surf;
    static RipParser parser;

    if (argc < 3) {
        fprintf(stderr,
                "usage: rip_render <capture.rip> <out.bmp> [--aspect]\n");
        return 2;
    }
    in_path  = argv[1];
    out_path = argv[2];
    for (i = 3; i < argc; i++)
        if (strcmp(argv[i], "--aspect") == 0) do_aspect = 1;

    f = fopen(in_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", in_path); return 1; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); fprintf(stderr, "empty capture\n"); return 1; }
    buf = (unsigned char *)malloc((size_t)size);
    if (!buf) { fclose(f); fprintf(stderr, "out of memory\n"); return 1; }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f); free(buf); fprintf(stderr, "short read\n"); return 1;
    }
    fclose(f);

    rip_parser_init(&parser, &surf);
    rip_parser_feed(&parser, buf, (int)size);
    free(buf);

    bgra = (unsigned char *)malloc((size_t)RIP_EGA_W * RIP_EGA_H * 4);
    if (!bgra) { fprintf(stderr, "out of memory\n"); return 1; }
    rip_ega_to_bgra(&surf, bgra);

    img = bgra;
    if (do_aspect) {
        unsigned char *scaled = aspect_correct(bgra, &img_h);
        if (!scaled) { fprintf(stderr, "out of memory\n"); return 1; }
        img = scaled;
    }

    if (!write_bmp(out_path, img, RIP_EGA_W, img_h)) {
        fprintf(stderr, "cannot write %s\n", out_path);
        return 1;
    }
    if (img != bgra) free(img);
    free(bgra);

    printf("rendered %s -> %s (%dx%d)\n", in_path, out_path,
           RIP_EGA_W, img_h);
    printf("commands=%lu drawn=%lu text=%lu font=%lu curves=%lu\n",
           parser.stats.commands, parser.stats.drawn,
           parser.stats.text_cmds, parser.stats.font_cmds,
           parser.stats.arc_cmds);
    printf("buttons=%lu regions=%lu\n",
           parser.stats.button_cmds, parser.stats.region_cmds);
    printf("deferred=%lu (level1=%lu tty=%lu flood=%lu bezier=%lu "
           "btn_undrawn=%lu dl_blocked=%lu)\n",
           rip_stats_deferred(&parser.stats),
           parser.stats.level1_cmds, parser.stats.tty_cmds,
           parser.stats.flood_cmds, parser.stats.bezier_cmds,
           parser.stats.button_undrawn, parser.stats.download_blocked);
    printf("unknown=%lu rawtext=%lu fillpat=%lu linestyle=%lu "
           "malformed=%lu\n",
           parser.stats.unknown_cmds, parser.stats.raw_text_lines,
           parser.stats.unsup_fill_pat, parser.stats.unsup_line_style,
           parser.stats.malformed);
    return 0;
}
