/*
 * minitel_screen.h - render Minitel page memory to a 320 x 250 raster.
 *                    A shared Suite service, no Win32 in it.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from STUM1B (provenance: minitel_decode.h). Two consumers
 * drive it, as with the NAPLPS surface: the Videotex tab, which blits the
 * raster, and tools/minitel_selftest.c, which writes it to a BMP with no
 * window anywhere. That second consumer is what keeps this file honest.
 */

#ifndef MINITEL_SCREEN_H_INCLUDED
#define MINITEL_SCREEN_H_INCLUDED

#include "minitel_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Render options. */
#define MT_R_MONO     0x01   /* grey levels of STUM1B p32, as the 1B tube */
#define MT_R_FLASH    0x02   /* the "off" phase of flashing (about 0.5 Hz) */
#define MT_R_CURSOR   0x04   /* the cursor's complementary phase           */

/* out: MINITEL_W * MINITEL_H pixels, 4 bytes each, B G R 0, top row
 * first. */
void minitel_render_bgra(const minitel_decoder_t *d, unsigned char *out,
                         int opts);

/* The colour of code c (MT_BLACK..MT_WHITE) as 0x00RRGGBB, in colour or
 * through the grey table. Exposed so the harness and the tab agree. */
unsigned long minitel_rgb(int c, int mono);

/*
 * Present the 320 x 250 raster at an integer scale, optionally with CRT
 * scan lines. src is minitel_render_bgra's output; dst holds
 * (MINITEL_W * scale) x (MINITEL_H * scale) pixels, B G R 0, top first.
 *
 * The scan-line effect was reproduced by OBSERVATION of MiniPavi's
 * hosted emulator's rendered pixels, its Scanlines switch on and off on
 * the same page, and nothing else; the measurement and the fit are in
 * docs/2026-09-27_SUITE_MINITEL_MODE.md section 9. No emulator source
 * was read.
 */
#define MT_P_SCANLINES 0x01
#define MT_P_MONO      0x02   /* a monochrome tube: rows only, no RGB triad */
void minitel_present_bgra(const unsigned char *src, int scale,
                          unsigned char *dst, int opts);

#ifdef __cplusplus
}
#endif

#endif /* MINITEL_SCREEN_H_INCLUDED */
