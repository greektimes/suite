/*
 * mt_web.c - the WebAssembly face of the Suite's Minitel engine.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * ONE SOURCE OF TRUTH. The web terminal decodes and draws with exactly
 * the files the Suite's Videotex tab links: src/minitel_decode.c,
 * src/minitel_screen.c, src/minitel_font_data.c and
 * src/naplps_font_data.c, compiled UNCHANGED (provenance in
 * src/minitel_decode.h: clean-room from STUM1B). This file adds nothing
 * to decoding or drawing. It only holds the buffers a browser needs and
 * turns the engine's B G R 0 pixels into the R G B A a canvas takes.
 *
 * The JS side (web/minitel/site/minitel.js) owns the socket, the keys and
 * the canvas, as minitel_view.c does for the tab.
 */

#include <emscripten/emscripten.h>
#include <stdlib.h>
#include <string.h>

#include "minitel_decode.h"
#include "minitel_screen.h"

#define MTW_IN_MAX     65536
#define MTW_REPLY_MAX  256
#define MTW_SCALE_MAX  8

static minitel_decoder_t *g_dec;
static unsigned char g_in[MTW_IN_MAX];
static unsigned char g_raster[MINITEL_W * MINITEL_H * 4];
static unsigned char *g_big;           /* presented, then R G B A in place */
static int g_big_scale;
static unsigned char g_reply[MTW_REPLY_MAX];
static size_t g_reply_len;
static int g_reply_on;

/* The decoder's answers (ESC 0x61, PRO1 ENQROM) are queued here; the JS
 * side sends them up the socket after each feed, and only while a
 * session is up, as minitel_view.c's reply_to_line does. */
static void on_reply(void *ctx, const unsigned char *b, size_t n)
{
    (void)ctx;
    if (!g_reply_on) return;
    if (n > MTW_REPLY_MAX - g_reply_len) n = MTW_REPLY_MAX - g_reply_len;
    memcpy(g_reply + g_reply_len, b, n);
    g_reply_len += n;
}

EMSCRIPTEN_KEEPALIVE int mtw_init(void)
{
    if (!g_dec) g_dec = minitel_create();
    if (!g_dec) return 0;
    minitel_set_reply(g_dec, on_reply, NULL);
    minitel_set_status_letter(g_dec, 'F');
    return 1;
}

/* Power-on state (p87-88). minitel_reset keeps the status letter and the
 * reply hook, as the tab relies on. */
EMSCRIPTEN_KEEPALIVE void mtw_reset(void)
{
    if (!g_dec) return;
    minitel_reset(g_dec);
    g_reply_len = 0;
}

EMSCRIPTEN_KEEPALIVE void mtw_set_reply(int on)
{
    g_reply_on = on;
    if (!on) g_reply_len = 0;
}

EMSCRIPTEN_KEEPALIVE void mtw_set_status(int letter)
{
    if (g_dec) minitel_set_status_letter(g_dec, (char)letter);
}

/* JS copies up to MTW_IN_MAX bytes here, then calls mtw_feed(n). */
EMSCRIPTEN_KEEPALIVE unsigned char *mtw_inbuf(void) { return g_in; }
EMSCRIPTEN_KEEPALIVE int mtw_inbuf_size(void) { return MTW_IN_MAX; }

EMSCRIPTEN_KEEPALIVE void mtw_feed(int n)
{
    if (!g_dec || n <= 0) return;
    if (n > MTW_IN_MAX) n = MTW_IN_MAX;
    minitel_feed(g_dec, g_in, (size_t)n);
}

EMSCRIPTEN_KEEPALIVE unsigned char *mtw_reply_ptr(void) { return g_reply; }

/* Length of the queued reply; the queue is emptied by the call. */
EMSCRIPTEN_KEEPALIVE int mtw_reply_take(void)
{
    int n = (int)g_reply_len;
    g_reply_len = 0;
    return n;
}

/*
 * Render the page memory and present it at an integer scale, as the
 * tab's canvas_paint does: minitel_render_bgra with the MT_R_* options,
 * then minitel_present_bgra with the MT_P_* options. Returns the R G B A
 * pixels ((320 * scale) x (250 * scale), alpha 255), or 0.
 */
EMSCRIPTEN_KEEPALIVE unsigned char *mtw_frame(int scale, int ropts, int popts)
{
    size_t n, i;
    if (!g_dec) return 0;
    if (scale < 1) scale = 1;
    if (scale > MTW_SCALE_MAX) scale = MTW_SCALE_MAX;
    if (scale != g_big_scale) {
        free(g_big);
        g_big = (unsigned char *)malloc((size_t)MINITEL_W * scale * MINITEL_H * scale * 4);
        g_big_scale = g_big ? scale : 0;
        if (!g_big) return 0;
    }
    minitel_render_bgra(g_dec, g_raster, ropts);
    minitel_present_bgra(g_raster, scale, g_big, popts);
    n = (size_t)MINITEL_W * scale * MINITEL_H * scale * 4;
    for (i = 0; i < n; i += 4) {
        unsigned char b = g_big[i];
        g_big[i] = g_big[i + 2];
        g_big[i + 2] = b;
        g_big[i + 3] = 255;
    }
    return g_big;
}

EMSCRIPTEN_KEEPALIVE int mtw_scale_max(void) { return MTW_SCALE_MAX; }

EMSCRIPTEN_KEEPALIVE int mtw_ever_drew(void)
{
    return g_dec ? minitel_ever_drew(g_dec) : 0;
}

EMSCRIPTEN_KEEPALIVE int mtw_screen_masked(void)
{
    return g_dec ? minitel_screen_masked(g_dec) : 0;
}

EMSCRIPTEN_KEEPALIVE int mtw_scroll_mode(void)
{
    return g_dec ? minitel_scroll_mode(g_dec) : 0;
}

/* One page-memory cell packed as kind | glyph<<8 | fg<<16 | bg<<20 |
 * flags<<24, and its size bits apart; for the parity gate (W-G1). */
EMSCRIPTEN_KEEPALIVE int mtw_cell(int row, int col)
{
    const minitel_cell_t *c;
    if (!g_dec) return -1;
    c = minitel_cell(g_dec, row, col);
    if (!c) return -1;
    return (int)((unsigned)c->kind | ((unsigned)c->glyph << 8)
                 | ((unsigned)(c->fg & 15) << 16) | ((unsigned)(c->bg & 15) << 20)
                 | ((unsigned)c->flags << 24));
}

EMSCRIPTEN_KEEPALIVE int mtw_cell_size(int row, int col)
{
    const minitel_cell_t *c;
    if (!g_dec) return -1;
    c = minitel_cell(g_dec, row, col);
    return c ? c->size : -1;
}

/* The counters, in minitel_stats_t order (11 values). */
EMSCRIPTEN_KEEPALIVE unsigned long mtw_stat(int i)
{
    const minitel_stats_t *s;
    if (!g_dec) return 0;
    s = minitel_get_stats(g_dec);
    switch (i) {
    case 0:  return s->bytes_in;
    case 1:  return s->chars;
    case 2:  return s->filtered;
    case 3:  return s->resync;
    case 4:  return s->errors;
    case 5:  return s->row0;
    case 6:  return s->protocol;
    case 7:  return s->unrepresent;
    case 8:  return s->bit8;
    case 9:  return s->bells;
    case 10: return s->us_ignored;
    default: return 0;
    }
}
