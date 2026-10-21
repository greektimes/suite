/*
 * gwb_machine.c - a small IBM PC that runs Microsoft's GW-BASIC, in process.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See gwb_machine.h for what this is. The layout of this file:
 *
 *     memory and ports          the memio callback libx86emu calls
 *     the ROM                   vector stubs, wait loops, 8x8 font
 *     BIOS                      INT 10h video, 16h keyboard, 1Ah clock...
 *     DOS                       INT 21h: console, memory, handles, FCBs
 *     the loader and the tick   MZ EXE + PSP, and gwb_run_tick
 *
 * The rule for adding a service: GW-BASIC's own source in
 * firmware/gwbasic/ is the specification. Anything the guest asks for
 * that is not here is logged (gwb_set_log) and answered with the
 * "unsupported" result a PC of the period would give, never guessed at.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "x86emu.h"

#include "gwb_machine.h"
#include "gwb_font8x8.h"

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

#define MEM_SIZE      0x100000u
#define ROM_SEG       0xF000u
#define ROM_BASE      0xF0000u
#define VRAM_BASE     0xB8000u
#define BDA           0x400u

#define ENV_SEG       0x0080u           /* environment block */
#define PSP_SEG       0x00A0u           /* program segment prefix */
#define LOL_SEG       0x0070u           /* fake DOS List of Lists, all FFh */
#define MEM_TOP_SEG   0xA000u           /* 640 KB */

#define STUB_END      0x400u            /* F000:0000..03FF: vector stubs */
#define WAIT_BASE     0x400u            /* F000:0400..0BFF: 8-byte wait loops */
#define FONT_HI_OFS   0xE000u           /* INT 1Fh: glyphs 128..255 */
#define FONT_LO_OFS   0xFA6Eu           /* the PC BIOS's own 8x8 table */
#define PARAMS_OFS    0xF0A4u           /* INT 1Dh / 1Eh tables: zeros */

#define TICKS_PER_DAY 0x1800B0u

#define MAX_HANDLES   20
#define MAX_SEARCHES  4

/* service results */
#define SVC_DONE  0
#define SVC_WAIT  1
#define SVC_EXIT  2

typedef struct dirent83 {
    char     name[13];                  /* "NAME.EXT" */
    uint8_t  fcb[11];                   /* "NAME    EXT" */
    uint8_t  attr;
    uint32_t size;
    uint16_t date, time;
} dirent83;

typedef struct search {
    int       used;
    dirent83 *ents;
    int       count, next;
    uint8_t   pattern[11];
    uint8_t   attr;
} search;

struct gwb_machine {
    x86emu_t *emu;
    uint8_t  *mem;

    /* time. GUEST time is instructions executed plus the instructions the
     * CPU would have executed while it sat in HLT (idle_skip), so a guest
     * waiting for an interrupt still sees time pass. */
    uint64_t ticks;
    double   budget_carry;
    unsigned tick_budget;
    uint64_t idle_skip;
    double   next_irq0;                 /* guest time of the next IRQ0 */
    int      irq0_pending;
    int      exited;

    /* the PC speaker: events of the current tick, and its PCM */
    struct { uint32_t at; uint8_t on; uint16_t div; } spk_ev[512];
    int      spk_nev;
    uint64_t tick_t0;                   /* guest time at the start of the tick */
    uint8_t  spk_on;                    /* state at the start of the tick */
    uint16_t spk_div;
    double   spk_phase;
    double   pcm_carry;
    int16_t  pcm[4096];
    int      pcm_n;

    /* keyboard: written by any thread, read by the machine thread */
    CRITICAL_SECTION kq_lock;
    uint16_t kq[64];
    unsigned kq_head, kq_tail;
    unsigned kq_visible;                /* keys "through IRQ1": what INT 16h can see */
    volatile LONG shift_flags;
    volatile LONG break_req;
    int      break_pending;
    int      dos_ext_scan;              /* second half of an extended key, DOS 06/07/08 */

    /* ports */
    uint8_t  port61;
    uint8_t  pic_mask;
    uint8_t  crtc_index;
    uint8_t  crtc[32];
    int      crtc_cursor_live;          /* last cursor update came through the CRTC */
    uint8_t  cga_mode, cga_color;
    unsigned retrace;
    uint16_t pit_reload[3];
    uint16_t pit_latch[3];
    int      pit_latched[3];
    uint8_t  pit_access[3];
    int      pit_hi_next[3];
    int      pit_read_hi[3];

    /* DOS */
    uint16_t heap_seg;
    uint16_t dta_seg, dta_off;
    wchar_t  root[MAX_PATH];
    char     cwd[80];                   /* "" or "DIR\SUB", no leading '\' */
    HANDLE   fh[MAX_HANDLES];
    uint8_t  fh_written[MAX_HANDLES];   /* FCB file written since open */
    search   searches[MAX_SEARCHES];
    unsigned next_search;
    uint8_t  verify;

    int      trace;                     /* GWB_TRACE set: log every service call */
    void   (*log)(void *, const char *);
    void    *log_ctx;
    unsigned log_count;
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void logf_(gwb_machine *m, const char *fmt, ...)
{
    char line[256];
    va_list ap;

    if (!m->log || m->log_count > 20000) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    m->log_count++;
    m->log(m->log_ctx, line);
}

static inline uint32_t lin(uint32_t seg, uint32_t off)
{
    return ((seg << 4) + (off & 0xFFFFu)) & 0xFFFFFu;
}

static inline uint8_t  rb(gwb_machine *m, uint32_t a) { return m->mem[a & 0xFFFFFu]; }
static inline uint16_t rw(gwb_machine *m, uint32_t a) { return (uint16_t)(rb(m, a) | (rb(m, a + 1) << 8)); }
static inline void     wb(gwb_machine *m, uint32_t a, uint8_t v) { m->mem[a & 0xFFFFFu] = v; }
static inline void     ww(gwb_machine *m, uint32_t a, uint16_t v) { wb(m, a, (uint8_t)v); wb(m, a + 1, (uint8_t)(v >> 8)); }
static inline void     wd(gwb_machine *m, uint32_t a, uint32_t v) { ww(m, a, (uint16_t)v); ww(m, a + 2, (uint16_t)(v >> 16)); }

#define R(m) ((m)->emu->x86)

/* Results go back through the FLAGS image the INT pushed (IP, CS, FLAGS
 * at SS:SP), because the stub's IRET will reload FLAGS from there. The
 * live flags are set too, for the wait loops. */
static void ret_flag(gwb_machine *m, unsigned mask, int on)
{
    uint32_t a = lin(R(m).R_SS, (uint16_t)(R(m).R_SP + 4));
    uint16_t f = rw(m, a);

    f = on ? (uint16_t)(f | mask) : (uint16_t)(f & ~mask);
    ww(m, a, f);
    if (on) R(m).R_EFLG |= mask; else R(m).R_EFLG &= ~mask;
}

static void ok(gwb_machine *m)               { ret_flag(m, F_CF, 0); }
static void fail(gwb_machine *m, uint16_t e) { R(m).R_AX = e; ret_flag(m, F_CF, 1); }

static void set_seg(gwb_machine *m, int idx, uint16_t v)
{
    x86emu_set_seg_register(m->emu, m->emu->x86.seg + idx, v);
}

/* The queue holds what the user has typed ahead; only the first
 * kq_visible keys have "arrived" through the keyboard interrupt and can
 * be seen by INT 16h. A real PC delivers keys one IRQ1 at a time, and
 * GW-BASIC depends on that: it copies arrivals into its own small buffer
 * and HLTs between characters, expecting the next keystroke's IRQ to wake
 * it. Handing it a whole burst at once overflows that buffer and drops
 * keys. So one key arrives per guest HLT (the IRQ1 that would have woken
 * it) and one per timer tick (for programs that poll without halting). */
static int key_count(gwb_machine *m)
{
    int n;
    EnterCriticalSection(&m->kq_lock);
    n = (int)m->kq_visible;
    LeaveCriticalSection(&m->kq_lock);
    return n;
}

/* one IRQ1: returns 1 if a key arrived */
static int key_deliver(gwb_machine *m)
{
    int did = 0;
    EnterCriticalSection(&m->kq_lock);
    if (m->kq_visible < ((m->kq_tail - m->kq_head) & 63u)) {
        m->kq_visible++;
        did = 1;
    }
    LeaveCriticalSection(&m->kq_lock);
    return did;
}

static int key_peek(gwb_machine *m, uint16_t *k)
{
    int have;
    EnterCriticalSection(&m->kq_lock);
    have = m->kq_visible > 0;
    if (have) *k = m->kq[m->kq_head];
    LeaveCriticalSection(&m->kq_lock);
    return have;
}

static int key_pop(gwb_machine *m, uint16_t *k)
{
    int have;
    EnterCriticalSection(&m->kq_lock);
    have = m->kq_visible > 0;
    if (have) {
        *k = m->kq[m->kq_head];
        m->kq_head = (m->kq_head + 1) & 63u;
        m->kq_visible--;
    }
    LeaveCriticalSection(&m->kq_lock);
    return have;
}

static void key_clear(gwb_machine *m)
{
    EnterCriticalSection(&m->kq_lock);
    m->kq_head = m->kq_tail = 0;
    m->kq_visible = 0;
    LeaveCriticalSection(&m->kq_lock);
}

/* ------------------------------------------------------------------ */
/* Memory and ports                                                    */
/* ------------------------------------------------------------------ */

/* Guest time, in instructions: see the struct. */
static uint64_t gnow(gwb_machine *m)
{
    return m->emu->x86.R_TSC + m->idle_skip;
}

static uint8_t pit_read(gwb_machine *m, int ch)
{
    uint16_t v;
    uint32_t reload = m->pit_reload[ch] ? m->pit_reload[ch] : 65536u;
    uint64_t n = gnow(m);

    if (m->pit_latched[ch]) {
        v = m->pit_latch[ch];
    } else {
        /* the 1.19318 MHz counter, derived from guest instructions */
        uint64_t clocks = n * 1193182u / GWB_IPS;
        v = (uint16_t)(reload - (uint32_t)(clocks % reload));
    }
    if (m->pit_access[ch] == 3) {
        uint8_t r = m->pit_read_hi[ch] ? (uint8_t)(v >> 8) : (uint8_t)v;
        if (m->pit_read_hi[ch]) m->pit_latched[ch] = 0;
        m->pit_read_hi[ch] ^= 1;
        return r;
    }
    m->pit_latched[ch] = 0;
    return m->pit_access[ch] == 2 ? (uint8_t)(v >> 8) : (uint8_t)v;
}

static uint8_t port_in(gwb_machine *m, unsigned port)
{
    switch (port) {
    case 0x20: return 0;
    case 0x21: return m->pic_mask;
    case 0x40: case 0x41: case 0x42: return pit_read(m, (int)(port - 0x40));
    case 0x60: return 0;
    case 0x61: return m->port61;
    case 0x62: return 0;
    case 0x201: return 0xF0;            /* no joystick: buttons up, no axis pulse */
    case 0x3D4: return m->crtc_index;
    case 0x3D5: return m->crtc[m->crtc_index & 31];
    case 0x3D8: return m->cga_mode;
    case 0x3D9: return m->cga_color;
    case 0x3DA:
        /* alternate "in retrace" and "displaying" so any wait-for-edge
         * loop sees both edges */
        m->retrace++;
        return (m->retrace & 1) ? 0x09 : 0x00;
    default:
        return 0xFF;
    }
}

/* Note a speaker change at the current point of the tick, for the PCM. */
static void spk_event(gwb_machine *m)
{
    int n = m->spk_nev;
    uint64_t at = gnow(m) - m->tick_t0;

    if (n >= (int)(sizeof m->spk_ev / sizeof m->spk_ev[0])) return;
    m->spk_ev[n].at = (uint32_t)at;
    m->spk_ev[n].on = (uint8_t)((m->port61 & 3) == 3);
    m->spk_ev[n].div = m->pit_reload[2];
    m->spk_nev = n + 1;
}

static void port_out(gwb_machine *m, unsigned port, uint8_t v)
{
    int ch;

    switch (port) {
    case 0x21: m->pic_mask = v; break;
    case 0x40: case 0x41: case 0x42:
        ch = (int)(port - 0x40);
        if (m->pit_access[ch] == 1) {
            m->pit_reload[ch] = (uint16_t)((m->pit_reload[ch] & 0xFF00) | v);
        } else if (m->pit_access[ch] == 2) {
            m->pit_reload[ch] = (uint16_t)((m->pit_reload[ch] & 0x00FF) | (v << 8));
        } else {
            if (m->pit_hi_next[ch])
                m->pit_reload[ch] = (uint16_t)((m->pit_reload[ch] & 0x00FF) | (v << 8));
            else
                m->pit_reload[ch] = (uint16_t)((m->pit_reload[ch] & 0xFF00) | v);
            m->pit_hi_next[ch] ^= 1;
        }
        if (ch == 2) spk_event(m);
        break;
    case 0x43:
        ch = (v >> 6) & 3;
        if (ch == 3) break;             /* read-back: 8254 only */
        if (((v >> 4) & 3) == 0) {      /* counter latch */
            uint8_t save = m->pit_access[ch];
            m->pit_latched[ch] = 0;
            m->pit_access[ch] = 1;
            m->pit_latch[ch] = (uint16_t)(pit_read(m, ch));
            m->pit_access[ch] = 2;
            m->pit_latch[ch] |= (uint16_t)(pit_read(m, ch) << 8);
            m->pit_access[ch] = save;
            m->pit_latched[ch] = 1;
            m->pit_read_hi[ch] = 0;
        } else {
            m->pit_access[ch] = (uint8_t)((v >> 4) & 3);
            m->pit_hi_next[ch] = 0;
            m->pit_read_hi[ch] = 0;
        }
        break;
    case 0x61: m->port61 = v; spk_event(m); break;
    case 0x3D4: m->crtc_index = v; break;
    case 0x3D5:
        m->crtc[m->crtc_index & 31] = v;
        if ((m->crtc_index & 31) >= 0x0A && (m->crtc_index & 31) <= 0x0F)
            m->crtc_cursor_live = 1;
        break;
    case 0x3D8: m->cga_mode = v; break;
    case 0x3D9: m->cga_color = v; break;
    default: break;                     /* PIC EOI, POST code, and the rest */
    }
}

static unsigned memio(x86emu_t *emu, u32 addr, u32 *val, unsigned type)
{
    gwb_machine *m = (gwb_machine *)emu->_private;
    unsigned bits = type & 0xFF, n, i;
    uint32_t v;

    n = bits == X86EMU_MEMIO_16 ? 2 : bits == X86EMU_MEMIO_32 ? 4 : 1;

    switch (type & ~0xFFu) {
    case X86EMU_MEMIO_R:
    case X86EMU_MEMIO_X:
        v = 0;
        for (i = 0; i < n; i++) {
            uint32_t a = (addr + i) & 0xFFFFFu;
            if (a >= 0xBC000u && a < 0xC0000u) a -= 0x4000u;   /* CGA mirror */
            v |= (uint32_t)m->mem[a] << (8 * i);
        }
        *val = v;
        break;
    case X86EMU_MEMIO_W:
        v = *val;
        for (i = 0; i < n; i++) {
            uint32_t a = (addr + i) & 0xFFFFFu;
            if (a >= 0xBC000u && a < 0xC0000u) a -= 0x4000u;
            if (a < 0xC0000u) m->mem[a] = (uint8_t)(v >> (8 * i));
            /* writes to adapter ROM space and the BIOS ROM are dropped */
        }
        break;
    case X86EMU_MEMIO_I:
        v = 0;
        for (i = 0; i < n; i++) v |= (uint32_t)port_in(m, (addr + i) & 0xFFFFu) << (8 * i);
        *val = v;
        break;
    case X86EMU_MEMIO_O:
        for (i = 0; i < n; i++) port_out(m, (addr + i) & 0xFFFFu, (uint8_t)(*val >> (8 * i)));
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The ROM                                                             */
/* ------------------------------------------------------------------ */

static void build_rom(gwb_machine *m)
{
    unsigned n, i;
    uint8_t *rom = m->mem + ROM_BASE;

    memset(rom, 0xCF, 0x10000);         /* IRET everywhere is a safe default */

    for (n = 0; n < 256; n++) {
        uint8_t *stub = rom + n * 4;
        uint8_t *wait = rom + WAIT_BASE + n * 8;
        uint16_t back;

        stub[0] = 0xCF; stub[1] = 0x90; stub[2] = 0x90; stub[3] = 0x90;

        /* STI; HLT; JMP back to the vector's stub, which re-enters the
         * service: the shape of a BIOS keyboard wait with interrupts on */
        back = (uint16_t)(n * 4 - (WAIT_BASE + n * 8 + 5));
        wait[0] = 0xFB;
        wait[1] = 0xF4;
        wait[2] = 0xE9;
        wait[3] = (uint8_t)back;
        wait[4] = (uint8_t)(back >> 8);

        ww(m, n * 4, (uint16_t)(n * 4));
        ww(m, n * 4 + 2, ROM_SEG);
    }

    /* INT 08h: count the tick in C, then INT 1Ch, then IRET */
    rom[0x08 * 4 + 0] = 0xCD;
    rom[0x08 * 4 + 1] = 0x1C;
    rom[0x08 * 4 + 2] = 0xCF;

    /* data vectors */
    memset(rom + PARAMS_OFS, 0, 64);
    ww(m, 0x1D * 4, PARAMS_OFS);
    ww(m, 0x1E * 4, PARAMS_OFS + 32);
    ww(m, 0x1F * 4, FONT_HI_OFS);

    for (i = 0; i < 128; i++) {
        memcpy(rom + FONT_LO_OFS + i * 8, gwb_font8x8[i], 8);
        memcpy(rom + FONT_HI_OFS + i * 8, gwb_font8x8[128 + i], 8);
    }

    memcpy(rom + 0xFFF5, "10/20/26", 8);
    rom[0xFFFE] = 0xFE;                 /* model byte: PC/XT */
}

/* ------------------------------------------------------------------ */
/* BIOS: video                                                         */
/* ------------------------------------------------------------------ */

static uint8_t  vmode(gwb_machine *m)      { return rb(m, BDA + 0x49); }
static unsigned vcols(gwb_machine *m)      { return rb(m, BDA + 0x4A) ? rb(m, BDA + 0x4A) : 80; }
static int      vtext(gwb_machine *m)      { return vmode(m) <= 3 || vmode(m) == 7; }
static unsigned vpage_size(gwb_machine *m) { return rw(m, BDA + 0x4C); }

static uint32_t text_addr(gwb_machine *m, unsigned page, unsigned row, unsigned col)
{
    return VRAM_BASE + ((page * vpage_size(m) + (row * vcols(m) + col) * 2) & 0x3FFFu);
}

static void cursor_get(gwb_machine *m, unsigned page, unsigned *row, unsigned *col)
{
    *col = rb(m, BDA + 0x50 + (page & 7) * 2);
    *row = rb(m, BDA + 0x51 + (page & 7) * 2);
}

static void cursor_set(gwb_machine *m, unsigned page, unsigned row, unsigned col)
{
    wb(m, BDA + 0x50 + (page & 7) * 2, (uint8_t)col);
    wb(m, BDA + 0x51 + (page & 7) * 2, (uint8_t)row);
    if ((page & 7) == rb(m, BDA + 0x62)) m->crtc_cursor_live = 0;
}

/* graphics modes 4/5 (320x200x4) and 6 (640x200x2) */
static uint32_t gfx_line(unsigned y) { return VRAM_BASE + (y & 1) * 0x2000u + (y >> 1) * 80u; }

static void gfx_char(gwb_machine *m, unsigned row, unsigned col, uint8_t ch, uint8_t attr)
{
    unsigned y;
    int x4 = vmode(m) != 6;

    if (row >= 25 || col >= vcols(m)) return;
    for (y = 0; y < 8; y++) {
        uint8_t bits = gwb_font8x8[ch][y];
        uint32_t a = gfx_line(row * 8 + y);
        if (x4) {
            uint16_t w = 0;
            unsigned b;
            for (b = 0; b < 8; b++)
                if (bits & (0x80 >> b)) w |= (uint16_t)((attr & 3) << (14 - 2 * b));
            a += col * 2;
            if (attr & 0x80) { wb(m, a, (uint8_t)(rb(m, a) ^ (w >> 8))); wb(m, a + 1, (uint8_t)(rb(m, a + 1) ^ w)); }
            else             { wb(m, a, (uint8_t)(w >> 8)); wb(m, a + 1, (uint8_t)w); }
        } else {
            a += col;
            wb(m, a, (attr & 0x80) ? (uint8_t)(rb(m, a) ^ bits) : bits);
        }
    }
}

static uint8_t gfx_read_char(gwb_machine *m, unsigned row, unsigned col)
{
    uint8_t cell[8];
    unsigned y, c;
    int x4 = vmode(m) != 6;

    for (y = 0; y < 8; y++) {
        uint32_t a = gfx_line(row * 8 + y);
        if (x4) {
            uint16_t w = (uint16_t)((rb(m, a + col * 2) << 8) | rb(m, a + col * 2 + 1));
            uint8_t b = 0;
            unsigned i;
            for (i = 0; i < 8; i++) if (w & (3u << (14 - 2 * i))) b |= (uint8_t)(0x80 >> i);
            cell[y] = b;
        } else {
            cell[y] = rb(m, a + col);
        }
    }
    for (c = 0; c < 256; c++) if (!memcmp(cell, gwb_font8x8[c], 8)) return (uint8_t)c;
    return 0;
}

static void put_cell(gwb_machine *m, unsigned page, unsigned row, unsigned col,
                     uint8_t ch, uint8_t attr, int with_attr)
{
    if (vtext(m)) {
        uint32_t a = text_addr(m, page, row, col);
        wb(m, a, ch);
        if (with_attr) wb(m, a + 1, attr);
    } else {
        gfx_char(m, row, col, ch, attr);
    }
}

static void scroll(gwb_machine *m, int up, unsigned lines, uint8_t attr,
                   unsigned r0, unsigned c0, unsigned r1, unsigned c1)
{
    unsigned cols = vcols(m), page = rb(m, BDA + 0x62), r, c, h;

    if (r1 > 24) r1 = 24;
    if (c1 >= cols) c1 = cols - 1;
    if (r0 > r1 || c0 > c1) return;
    h = r1 - r0 + 1;
    if (lines == 0 || lines > h) lines = h;

    if (vtext(m)) {
        for (r = 0; r < h; r++) {
            unsigned dst = up ? r0 + r : r1 - r;
            unsigned src = up ? dst + lines : dst - lines;
            int clear = r >= h - lines;
            for (c = c0; c <= c1; c++) {
                uint32_t d = text_addr(m, page, dst, c);
                if (clear) { wb(m, d, ' '); wb(m, d + 1, attr); }
                else { uint32_t s = text_addr(m, page, src, c); ww(m, d, rw(m, s)); }
            }
        }
    } else {
        unsigned bpc = vmode(m) == 6 ? 1 : 2, y;
        uint8_t fill = vmode(m) == 6 ? (attr ? 0xFF : 0) : (uint8_t)((attr & 3) * 0x55);
        for (r = 0; r < h; r++) {
            unsigned dst = up ? r0 + r : r1 - r;
            unsigned src = up ? dst + lines : dst - lines;
            int clear = r >= h - lines;
            for (y = 0; y < 8; y++) {
                uint32_t d = gfx_line(dst * 8 + y) + c0 * bpc;
                if (clear) memset(m->mem + d, fill, (c1 - c0 + 1) * bpc);
                else memmove(m->mem + d, m->mem + gfx_line(src * 8 + y) + c0 * bpc, (c1 - c0 + 1) * bpc);
            }
        }
    }
}

static void set_mode(gwb_machine *m, uint8_t al)
{
    static const uint8_t ctl[8] = { 0x2C, 0x28, 0x2D, 0x29, 0x2A, 0x2E, 0x1E, 0x29 };
    uint8_t mode = al & 0x7F;
    unsigned i;

    if (mode > 6) {
        logf_(m, "INT 10h: video mode %u not on a CGA, using 3", mode);
        mode = 3;
    }
    wb(m, BDA + 0x49, mode);
    wb(m, BDA + 0x4A, (mode == 0 || mode == 1 || mode == 4 || mode == 5) ? 40 : 80);
    ww(m, BDA + 0x4C, mode <= 1 ? 0x800 : mode <= 3 ? 0x1000 : 0x4000);
    ww(m, BDA + 0x4E, 0);
    for (i = 0; i < 16; i++) wb(m, BDA + 0x50 + i, 0);
    wb(m, BDA + 0x60, 7);
    wb(m, BDA + 0x61, 6);
    wb(m, BDA + 0x62, 0);
    ww(m, BDA + 0x63, 0x3D4);
    wb(m, BDA + 0x65, ctl[mode]);
    wb(m, BDA + 0x66, mode == 6 ? 0x3F : 0x30);
    wb(m, BDA + 0x84, 24);
    m->cga_mode = ctl[mode];
    m->cga_color = mode == 6 ? 0x3F : 0x30;
    m->crtc_cursor_live = 0;
    if (!(al & 0x80)) {
        if (mode <= 3)
            for (i = 0; i < GWB_VRAM_BYTES; i += 2) ww(m, VRAM_BASE + i, 0x0720);
        else
            memset(m->mem + VRAM_BASE, 0, GWB_VRAM_BYTES);
    }
}

/* teletype output: shared by INT 10h AH=0Eh/13h and the DOS console.
 * In text modes the cell keeps its attribute unless with_attr is set; in
 * the graphics modes attr is the foreground colour. */
static void tty_attr(gwb_machine *m, uint8_t ch, uint8_t attr, int with_attr)
{
    unsigned page = rb(m, BDA + 0x62), row, col, cols = vcols(m);

    cursor_get(m, page, &row, &col);
    switch (ch) {
    case 7:  return;                    /* BEL: no speaker yet */
    case 8:  if (col) col--; break;
    case 10: row++; break;
    case 13: col = 0; break;
    default:
        put_cell(m, page, row, col, ch, attr, with_attr || !vtext(m));
        if (++col >= cols) { col = 0; row++; }
        break;
    }
    if (row > 24) {
        uint8_t attr = vtext(m) ? rb(m, text_addr(m, page, 24, col) + 1) : 0;
        scroll(m, 1, 1, attr, 0, 0, 24, cols - 1);
        row = 24;
    }
    cursor_set(m, page, row, col);
}

static void tty(gwb_machine *m, uint8_t ch, uint8_t gfx_color)
{
    tty_attr(m, ch, gfx_color, 0);
}

static int bios_video(gwb_machine *m)
{
    x86emu_regs_t *r = &R(m);
    unsigned row, col, i, page;

    switch (r->R_AH) {
    case 0x00: set_mode(m, r->R_AL); break;
    case 0x01:
        wb(m, BDA + 0x61, r->R_CH);
        wb(m, BDA + 0x60, r->R_CL);
        m->crtc_cursor_live = 0;
        break;
    case 0x02: cursor_set(m, r->R_BH, r->R_DH, r->R_DL); break;
    case 0x03:
        cursor_get(m, r->R_BH, &row, &col);
        r->R_DH = (u8)row; r->R_DL = (u8)col;
        r->R_CH = rb(m, BDA + 0x61); r->R_CL = rb(m, BDA + 0x60);
        break;
    case 0x04: r->R_AH = 0; break;      /* no light pen triggered */
    case 0x05:
        if (vtext(m)) {
            wb(m, BDA + 0x62, r->R_AL & 7);
            ww(m, BDA + 0x4E, (uint16_t)((r->R_AL & 7) * vpage_size(m)));
            m->crtc_cursor_live = 0;
        }
        break;
    case 0x06:
    case 0x07:
        scroll(m, r->R_AH == 0x06, r->R_AL, r->R_BH, r->R_CH, r->R_CL, r->R_DH, r->R_DL);
        break;
    case 0x08:
        cursor_get(m, r->R_BH, &row, &col);
        if (vtext(m)) {
            r->R_AX = rw(m, text_addr(m, r->R_BH, row, col));
        } else {
            r->R_AL = gfx_read_char(m, row, col);
            r->R_AH = 0;
        }
        break;
    case 0x09:
    case 0x0A:
        page = vtext(m) ? r->R_BH : 0;
        cursor_get(m, page, &row, &col);
        for (i = 0; i < r->R_CX; i++) {
            unsigned c = col + i, rr = row + c / vcols(m);
            if (rr > 24) break;
            put_cell(m, page, rr, c % vcols(m), r->R_AL, r->R_BL, r->R_AH == 0x09 || !vtext(m));
        }
        break;
    case 0x0B:
        if (r->R_BH == 0) m->cga_color = (uint8_t)((m->cga_color & 0x20) | (r->R_BL & 0x1F));
        else              m->cga_color = (uint8_t)((m->cga_color & 0x1F) | ((r->R_BL & 1) << 5));
        wb(m, BDA + 0x66, m->cga_color);
        break;
    case 0x0C:
    case 0x0D:
        if (!vtext(m) && r->R_DX < 200) {
            unsigned x = r->R_CX, y = r->R_DX;
            uint32_t a;
            if (vmode(m) == 6) {
                if (x >= 640) break;
                a = gfx_line(y) + x / 8;
                if (r->R_AH == 0x0D) { r->R_AL = (rb(m, a) >> (7 - (x & 7))) & 1; break; }
                if (r->R_AL & 0x80) wb(m, a, (uint8_t)(rb(m, a) ^ ((r->R_AL & 1) << (7 - (x & 7)))));
                else wb(m, a, (uint8_t)((rb(m, a) & ~(1 << (7 - (x & 7)))) | ((r->R_AL & 1) << (7 - (x & 7)))));
            } else {
                unsigned sh = (3 - (x & 3)) * 2;
                if (x >= 320) break;
                a = gfx_line(y) + x / 4;
                if (r->R_AH == 0x0D) { r->R_AL = (rb(m, a) >> sh) & 3; break; }
                if (r->R_AL & 0x80) wb(m, a, (uint8_t)(rb(m, a) ^ ((r->R_AL & 3) << sh)));
                else wb(m, a, (uint8_t)((rb(m, a) & ~(3 << sh)) | ((r->R_AL & 3) << sh)));
            }
        }
        break;
    case 0x0E: tty(m, r->R_AL, r->R_BL); break;
    case 0x0F:
        r->R_AL = vmode(m);
        r->R_AH = (u8)vcols(m);
        r->R_BH = rb(m, BDA + 0x62);
        break;
    case 0x13: {
        uint32_t s = lin(r->R_ES, r->R_BP);
        unsigned save_r, save_c, n;
        page = r->R_BH;
        cursor_get(m, page, &save_r, &save_c);
        cursor_set(m, page, r->R_DH, r->R_DL);
        for (n = 0; n < r->R_CX; n++) {
            uint8_t ch = rb(m, s++), at = r->R_BL;
            if (r->R_AL & 2) at = rb(m, s++);
            tty_attr(m, ch, at, 1);
        }
        if (!(r->R_AL & 1)) cursor_set(m, page, save_r, save_c);
        break;
    }
    case 0x10: case 0x11: case 0x12: case 0x1A: case 0x1B:
        break;                          /* EGA/VGA calls: a CGA ignores them */
    default:
        logf_(m, "INT 10h AH=%02Xh not implemented", r->R_AH);
        break;
    }
    return SVC_DONE;
}

/* ------------------------------------------------------------------ */
/* BIOS: keyboard, clock and the rest                                  */
/* ------------------------------------------------------------------ */

static int bios_keyboard(gwb_machine *m)
{
    x86emu_regs_t *r = &R(m);
    uint16_t k;

    wb(m, BDA + 0x17, (uint8_t)m->shift_flags);
    switch (r->R_AH) {
    case 0x00:
    case 0x10:
        if (!key_pop(m, &k)) return SVC_WAIT;
        r->R_AX = k;
        break;
    case 0x01:
    case 0x11:
        if (key_peek(m, &k)) { r->R_AX = k; ret_flag(m, F_ZF, 0); }
        else ret_flag(m, F_ZF, 1);
        break;
    case 0x02:
    case 0x12:
        r->R_AL = (u8)m->shift_flags;
        if (r->R_AH == 0x12) r->R_AH = 0;
        break;
    case 0x03: break;                   /* typematic rate */
    case 0x05:
        gwb_push_key(m, r->R_CX);
        r->R_AL = 0;
        break;
    default:
        logf_(m, "INT 16h AH=%02Xh not implemented", r->R_AH);
        break;
    }
    return SVC_DONE;
}

static void timer_tick(gwb_machine *m)
{
    uint32_t t = rw(m, BDA + 0x6C) | ((uint32_t)rw(m, BDA + 0x6E) << 16);

    if (++t >= TICKS_PER_DAY) { t = 0; wb(m, BDA + 0x70, 1); }
    wd(m, BDA + 0x6C, t);
}

static int bios_clock(gwb_machine *m)
{
    x86emu_regs_t *r = &R(m);
    uint32_t t;

    switch (r->R_AH) {
    case 0x00:
        t = rw(m, BDA + 0x6C) | ((uint32_t)rw(m, BDA + 0x6E) << 16);
        r->R_CX = (u16)(t >> 16);
        r->R_DX = (u16)t;
        r->R_AL = rb(m, BDA + 0x70);
        wb(m, BDA + 0x70, 0);
        break;
    case 0x01:
        wd(m, BDA + 0x6C, ((uint32_t)r->R_CX << 16) | r->R_DX);
        wb(m, BDA + 0x70, 0);
        break;
    default:
        ret_flag(m, F_CF, 1);           /* an XT has no real-time clock */
        break;
    }
    return SVC_DONE;
}

/* ------------------------------------------------------------------ */
/* DOS: names and paths                                                */
/* ------------------------------------------------------------------ */

static int dos_char_ok(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && strchr("!#$%&'()-@^_`{}~", c) != NULL);
}

/* "name.ext" (any case) to the 11-byte FCB form; '*' becomes '?'.
 * Returns 0 if the name cannot be a DOS name. */
static int to_fcb11(const char *name, uint8_t out[11], int allow_wild)
{
    int i = 0, j;
    const char *p = name;

    memset(out, ' ', 11);
    if (!*p) return 0;
    for (j = 0; *p && *p != '.'; p++) {
        int c = (unsigned char)*p;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == '*' && allow_wild) { while (j < 8) out[j++] = '?'; continue; }
        if (c == '?' && allow_wild) { if (j < 8) out[j++] = '?'; continue; }
        if (!dos_char_ok(c)) return 0;
        if (j < 8) out[j++] = (uint8_t)c;      /* DOS truncates, as here */
    }
    if (j == 0) return 0;
    if (*p == '.') {
        p++;
        for (i = 8; *p; p++) {
            int c = (unsigned char)*p;
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == '*' && allow_wild) { while (i < 11) out[i++] = '?'; continue; }
            if (c == '?' && allow_wild) { if (i < 11) out[i++] = '?'; continue; }
            if (c == '.' || !dos_char_ok(c)) return 0;
            if (i < 11) out[i++] = (uint8_t)c;
        }
    }
    return 1;
}

static void fcb11_to_name(const uint8_t f[11], char *out)
{
    int i, n = 0;

    for (i = 0; i < 8 && f[i] != ' '; i++) out[n++] = (char)f[i];
    if (f[8] != ' ') {
        out[n++] = '.';
        for (i = 8; i < 11 && f[i] != ' '; i++) out[n++] = (char)f[i];
    }
    out[n] = 0;
}

static int fcb11_match(const uint8_t pat[11], const uint8_t name[11])
{
    int i;
    for (i = 0; i < 11; i++) if (pat[i] != '?' && pat[i] != name[i]) return 0;
    return 1;
}

/* A host name is shown to the guest only if it is already a valid,
 * upper-case-able 8.3 name; long names are simply not there. */
static int host_name_to_dos(const wchar_t *w, char out[13], uint8_t fcb[11])
{
    char a[16];
    int i;

    for (i = 0; w[i]; i++) {
        if (i >= 12 || w[i] > 126 || w[i] <= 32) return 0;
        a[i] = (char)w[i];
    }
    a[i] = 0;
    if (!to_fcb11(a, fcb, 0)) return 0;
    {
        /* reject names that to_fcb11 would have truncated */
        const char *dot = strchr(a, '.');
        size_t base = dot ? (size_t)(dot - a) : strlen(a);
        if (base > 8 || (dot && strlen(dot + 1) > 3)) return 0;
    }
    fcb11_to_name(fcb, out);
    return 1;
}

/* Resolve a guest path to a host path under root. The last component is
 * returned separately (upper-cased, as typed) for callers that want it.
 * Returns 0, or a DOS error (3 = path not found). */
static int resolve(gwb_machine *m, const char *dos, wchar_t *out, size_t outn,
                   char *last, int allow_wild_last)
{
    char comps[16][13];
    int n = 0, i;
    char buf[128], *p, *tok;

    if (dos[0] && dos[1] == ':') dos += 2;
    if (dos[0] != '\\' && dos[0] != '/' && m->cwd[0]) {
        snprintf(buf, sizeof buf, "%s\\%s", m->cwd, dos);
    } else {
        snprintf(buf, sizeof buf, "%s", dos);
    }

    p = buf;
    while ((tok = strtok(p, "\\/")) != NULL) {
        uint8_t f[11];
        p = NULL;
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (n) n--; continue; }
        if (n >= 16) return 3;
        if (!to_fcb11(tok, f, allow_wild_last)) return 3;
        fcb11_to_name(f, comps[n]);
        if (allow_wild_last) {
            /* keep the '?' form for the caller's pattern */
            memcpy(comps[n], tok, strlen(tok) < 12 ? strlen(tok) + 1 : 13);
            comps[n][12] = 0;
        }
        n++;
    }

    if (last) {
        if (n) snprintf(last, 13, "%s", comps[n - 1]);
        else last[0] = 0;
    }

    _snwprintf(out, outn, L"%ls", m->root);
    out[outn - 1] = 0;
    for (i = 0; i < n; i++) {
        size_t len = wcslen(out);
        int k;
        if (len + 14 >= outn) return 3;
        out[len++] = L'\\';
        for (k = 0; comps[i][k]; k++) {
            char c = comps[i][k];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[len++] = (wchar_t)(unsigned char)c;
        }
        out[len] = 0;
    }
    return 0;
}

static void read_asciiz(gwb_machine *m, uint16_t seg, uint16_t off, char *out, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n; i++) {
        out[i] = (char)rb(m, lin(seg, (uint16_t)(off + i)));
        if (!out[i]) break;
    }
    out[n - 1] = 0;
}

static uint16_t win_err(void)
{
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:   return 2;
    case ERROR_PATH_NOT_FOUND:   return 3;
    case ERROR_TOO_MANY_OPEN_FILES: return 4;
    case ERROR_ACCESS_DENIED:    return 5;
    case ERROR_SHARING_VIOLATION: return 5;
    case ERROR_ALREADY_EXISTS:   return 5;
    case ERROR_FILE_EXISTS:      return 5;
    case ERROR_DIR_NOT_EMPTY:    return 5;
    case ERROR_DISK_FULL:        return 5;
    default:                     return 5;
    }
}

static void file_times(const FILETIME *ft, uint16_t *date, uint16_t *time_)
{
    FILETIME lt;
    WORD d = 0, t = 0;
    FileTimeToLocalFileTime(ft, &lt);
    FileTimeToDosDateTime(&lt, &d, &t);
    *date = d; *time_ = t;
}

/* List a host directory as the guest would see it. */
static int list_dir(gwb_machine *m, const wchar_t *dir, int is_root,
                    dirent83 **out, int *count)
{
    wchar_t pat[MAX_PATH + 4];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0, cap = 32;
    dirent83 *v = malloc(sizeof *v * cap);

    (void)m;
    if (!v) return 8;
    if (!is_root) {
        /* DOS lists "." and ".." in subdirectories */
        memset(v, 0, sizeof *v * 2);
        strcpy(v[0].name, ".");  memcpy(v[0].fcb, ".          ", 11); v[0].attr = 0x10;
        strcpy(v[1].name, ".."); memcpy(v[1].fcb, "..         ", 11); v[1].attr = 0x10;
        n = 2;
    }
    _snwprintf(pat, MAX_PATH + 4, L"%ls\\*", dir);
    pat[MAX_PATH + 3] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        free(v);
        return 3;
    }
    do {
        dirent83 e;
        memset(&e, 0, sizeof e);
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (!host_name_to_dos(fd.cFileName, e.name, e.fcb)) continue;
        e.attr = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x20;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) e.attr |= 0x01;
        e.size = fd.nFileSizeHigh ? 0xFFFFFFFFu : fd.nFileSizeLow;
        file_times(&fd.ftLastWriteTime, &e.date, &e.time);
        if (n == cap) {
            dirent83 *nv = realloc(v, sizeof *v * (cap *= 2));
            if (!nv) break;
            v = nv;
        }
        v[n++] = e;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    *out = v;
    *count = n;
    return 0;
}

/* Split "DIR\PAT" into the resolved directory and an 11-byte pattern. */
static int prepare_search(gwb_machine *m, const char *spec, wchar_t *dir, size_t dirn,
                          uint8_t pat[11], int *is_root)
{
    char tmp[128], *slash;
    const char *name;
    int rc;

    snprintf(tmp, sizeof tmp, "%s", spec);
    slash = strrchr(tmp, '\\');
    if (!slash) slash = strrchr(tmp, '/');
    if (!slash && tmp[0] && tmp[1] == ':') slash = tmp + 1;
    if (slash) {
        name = slash + 1;
        if (slash == tmp || (slash == tmp + 2 && tmp[1] == ':')) {
            /* "\PAT" or "C:\PAT": the root */
            char d[4] = "\\";
            rc = resolve(m, d, dir, dirn, NULL, 0);
            *is_root = 1;
        } else if (slash == tmp + 1 && tmp[1] == ':') {
            rc = resolve(m, "", dir, dirn, NULL, 0);
            *is_root = m->cwd[0] == 0;
        } else {
            *slash = 0;
            rc = resolve(m, tmp, dir, dirn, NULL, 0);
            *is_root = wcslen(dir) == wcslen(m->root);
        }
    } else {
        name = tmp;
        rc = resolve(m, "", dir, dirn, NULL, 0);
        *is_root = m->cwd[0] == 0;
    }
    if (rc) return rc;
    if (!*name) name = "*.*";
    if (!to_fcb11(name, pat, 1)) return 3;
    return 0;
}

static search *search_new(gwb_machine *m)
{
    search *s = &m->searches[m->next_search++ % MAX_SEARCHES];
    free(s->ents);
    memset(s, 0, sizeof *s);
    s->used = 1;
    return s;
}

static const dirent83 *search_next(search *s)
{
    while (s->used && s->next < s->count) {
        const dirent83 *e = &s->ents[s->next++];
        int is_dir = (e->attr & 0x10) != 0;
        if (is_dir && !(s->attr & 0x10)) continue;
        if (!fcb11_match(s->pattern, e->fcb)) continue;
        return e;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* DOS: handles                                                        */
/* ------------------------------------------------------------------ */

static int handle_new(gwb_machine *m, HANDLE h)
{
    int i;
    for (i = 5; i < MAX_HANDLES; i++)
        if (!m->fh[i]) { m->fh[i] = h; m->fh_written[i] = 0; return i; }
    return -1;
}

static HANDLE handle_get(gwb_machine *m, unsigned i)
{
    return (i >= 5 && i < MAX_HANDLES) ? m->fh[i] : NULL;
}

static HANDLE open_host(const wchar_t *path, int mode, int create)
{
    DWORD acc = mode == 0 ? GENERIC_READ : mode == 1 ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;
    DWORD attr;

    if (!create) {
        attr = GetFileAttributesW(path);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
    }
    return CreateFileW(path, acc, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       create ? CREATE_ALWAYS : OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

static void dos_putc(gwb_machine *m, uint8_t c) { tty(m, c, 7); }

/* ------------------------------------------------------------------ */
/* DOS: FCBs                                                           */
/* ------------------------------------------------------------------ */

static uint32_t fcb_addr(gwb_machine *m, uint8_t *attr)
{
    uint32_t a = lin(R(m).R_DS, R(m).R_DX);
    if (rb(m, a) == 0xFF) {
        if (attr) *attr = rb(m, a + 6);
        return a + 7;
    }
    if (attr) *attr = 0;
    return a;
}

static void fcb_name(gwb_machine *m, uint32_t f, char *out)
{
    uint8_t n[11];
    unsigned i;
    for (i = 0; i < 11; i++) {
        uint8_t c = rb(m, f + 1 + i);
        n[i] = (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32) : c;
    }
    fcb11_to_name(n, out);
}

static HANDLE fcb_handle(gwb_machine *m, uint32_t f)
{
    if (rb(m, f + 0x19) != 'G') return NULL;
    return handle_get(m, rb(m, f + 0x18));
}

static void fcb_opened(gwb_machine *m, uint32_t f, int slot, HANDLE h)
{
    BY_HANDLE_FILE_INFORMATION bi;
    uint16_t d = 0, t = 0;

    if (GetFileInformationByHandle(h, &bi)) file_times(&bi.ftLastWriteTime, &d, &t);
    if (rb(m, f) == 0) wb(m, f, 3);     /* default drive becomes C: */
    ww(m, f + 0x0C, 0);
    ww(m, f + 0x0E, 128);
    wd(m, f + 0x10, GetFileSize(h, NULL));
    ww(m, f + 0x14, d);
    ww(m, f + 0x16, t);
    wb(m, f + 0x18, (uint8_t)slot);
    wb(m, f + 0x19, 'G');
    wb(m, f + 0x20, 0);
}

/* transfer one or more records between the file and the DTA;
 * returns the DOS FCB result code */
static uint8_t fcb_io(gwb_machine *m, uint32_t f, uint32_t rec, unsigned nrec,
                      int write, unsigned *done)
{
    HANDLE h = fcb_handle(m, f);
    unsigned size = rw(m, f + 0x0E), i, b;
    uint32_t dta = lin(m->dta_seg, m->dta_off);
    LARGE_INTEGER pos;
    uint8_t buf[512];

    *done = 0;
    if (!h) return 1;
    if (!size) size = 128;
    for (i = 0; i < nrec; i++) {
        DWORD got = 0;
        pos.QuadPart = (LONGLONG)(rec + i) * size;
        SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
        for (b = 0; b < size; b += sizeof buf) {
            DWORD chunk = size - b < sizeof buf ? size - b : sizeof buf, k, x = 0;
            uint32_t at = dta + i * size + b;
            if (write) {
                for (k = 0; k < chunk; k++) buf[k] = rb(m, at + k);
                if (!WriteFile(h, buf, chunk, &x, NULL) || x != chunk) return 1;
                m->fh_written[rb(m, f + 0x18)] = 1;
                got += x;
            } else {
                if (!ReadFile(h, buf, chunk, &x, NULL)) x = 0;
                for (k = 0; k < x; k++) wb(m, at + k, buf[k]);
                for (; k < chunk; k++) wb(m, at + k, 0);
                got += x;
                if (x < chunk) break;
            }
        }
        if (!write && got == 0) return 1;           /* EOF, nothing read */
        (*done)++;
        if (!write && got < size) return 3;         /* partial record */
    }
    if (write) {
        uint32_t end = (rec + nrec) * size;
        if (end > (uint32_t)rw(m, f + 0x10) + ((uint32_t)rw(m, f + 0x12) << 16)) wd(m, f + 0x10, end);
    }
    return 0;
}

/* INT 21h AH=29h: parse a file name into an FCB */
static void dos_parse(gwb_machine *m)
{
    x86emu_regs_t *r = &R(m);
    uint32_t s = lin(r->R_DS, r->R_SI), f = lin(r->R_ES, r->R_DI);
    uint8_t flags = r->R_AL, name[11];
    unsigned i, j, wild = 0, off = 0;
    static const char seps[] = ":.;,=+ \t";
    static const char terms[] = ":.;,=+ \t/\"[]<>|\r";

#define CH(k) rb(m, s + (k))
    if (flags & 1) { while (strchr(seps, CH(off)) && CH(off)) off++; }
    else { while (CH(off) == ' ' || CH(off) == '\t') off++; }

    if (CH(off) && CH(off + 1) == ':') {
        uint8_t d = CH(off);
        if (d >= 'a' && d <= 'z') d = (uint8_t)(d - 32);
        wb(m, f, (uint8_t)(d - 'A' + 1));
        off += 2;
    } else if (!(flags & 2)) {
        wb(m, f, 0);
    }

    for (i = 0; i < 11; i++) name[i] = rb(m, f + 1 + i);
    if (!(flags & 4)) memset(name, ' ', 8);
    if (!(flags & 8)) memset(name + 8, ' ', 3);

    for (i = 0, j = 0; CH(off) && !strchr(terms, CH(off)) && CH(off) > ' '; off++) {
        uint8_t c = CH(off);
        if (j == 0) { memset(name, ' ', 8); j = 1; }
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
        if (c == '*') { while (i < 8) name[i++] = '?'; wild = 1; continue; }
        if (c == '?') wild = 1;
        if (i < 8) name[i++] = c;
    }
    if (CH(off) == '.') {
        off++;
        for (i = 8, j = 0; CH(off) && !strchr(terms, CH(off)) && CH(off) > ' '; off++) {
            uint8_t c = CH(off);
            if (j == 0) { memset(name + 8, ' ', 3); j = 1; }
            if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
            if (c == '*') { while (i < 11) name[i++] = '?'; wild = 1; continue; }
            if (c == '?') wild = 1;
            if (i < 11) name[i++] = c;
        }
    }
#undef CH
    for (i = 0; i < 11; i++) wb(m, f + 1 + i, name[i]);
    r->R_SI = (u16)(r->R_SI + off);
    r->R_AL = wild ? 1 : 0;
}

static void put_dirent_fcb(gwb_machine *m, const dirent83 *e, int extended, uint8_t attr)
{
    uint32_t d = lin(m->dta_seg, m->dta_off);
    unsigned i;

    if (extended) {
        wb(m, d, 0xFF);
        for (i = 1; i < 6; i++) wb(m, d + i, 0);
        wb(m, d + 6, attr);
        d += 7;
    }
    wb(m, d, 3);
    for (i = 0; i < 11; i++) wb(m, d + 1 + i, e->fcb[i]);
    wb(m, d + 0x0C, e->attr);
    for (i = 0x0D; i < 0x16; i++) wb(m, d + i, 0);
    ww(m, d + 0x16, e->time);
    ww(m, d + 0x18, e->date);
    ww(m, d + 0x1A, 0);
    wd(m, d + 0x1C, e->size);
}

static int dos_fcb(gwb_machine *m)
{
    x86emu_regs_t *r = &R(m);
    uint8_t attr = 0;
    uint32_t f = fcb_addr(m, &attr);
    int extended = rb(m, lin(r->R_DS, r->R_DX)) == 0xFF;
    char name[13];
    wchar_t path[MAX_PATH];
    HANDLE h;
    int slot;
    unsigned done;

    switch (r->R_AH) {
    case 0x0F:                          /* open */
    case 0x16:                          /* create */
        fcb_name(m, f, name);
        if (resolve(m, name, path, MAX_PATH, NULL, 0)) { r->R_AL = 0xFF; break; }
        h = open_host(path, 2, r->R_AH == 0x16);
        if (h == INVALID_HANDLE_VALUE && r->R_AH == 0x0F) h = open_host(path, 0, 0);
        if (h == INVALID_HANDLE_VALUE) { r->R_AL = 0xFF; break; }
        if ((slot = handle_new(m, h)) < 0) { CloseHandle(h); r->R_AL = 0xFF; break; }
        fcb_opened(m, f, slot, h);
        r->R_AL = 0;
        break;
    case 0x10:                          /* close */
        h = fcb_handle(m, f);
        if (h) {
            /* as DOS does: the directory size becomes the FCB's size
             * field, which GW-BASIC maintains itself, so a file written
             * in 128-byte records ends at its true length */
            unsigned sl = rb(m, f + 0x18);
            if (m->fh_written[sl]) {
                LARGE_INTEGER sz;
                sz.QuadPart = (LONGLONG)(rw(m, f + 0x10) | ((uint32_t)rw(m, f + 0x12) << 16));
                if (SetFilePointerEx(h, sz, NULL, FILE_BEGIN)) SetEndOfFile(h);
                m->fh_written[sl] = 0;
            }
            CloseHandle(h);
            m->fh[rb(m, f + 0x18)] = NULL;
            wb(m, f + 0x19, 0);
        }
        r->R_AL = 0;
        break;
    case 0x11:                          /* search first */
    case 0x12: {                        /* search next */
        search *s;
        const dirent83 *e;
        if (r->R_AH == 0x11) {
            wchar_t dir[MAX_PATH];
            uint8_t pat[11];
            unsigned i;
            for (i = 0; i < 11; i++) {
                uint8_t c = rb(m, f + 1 + i);
                pat[i] = (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32) : c;
            }
            if (resolve(m, "", dir, MAX_PATH, NULL, 0)) { r->R_AL = 0xFF; break; }
            s = search_new(m);
            memcpy(s->pattern, pat, 11);
            s->attr = extended ? attr : 0;
            if (list_dir(m, dir, m->cwd[0] == 0, &s->ents, &s->count)) { s->used = 0; r->R_AL = 0xFF; break; }
            wb(m, f + 0x0C, (uint8_t)(s - m->searches));   /* remember which search */
        } else {
            unsigned idx = rb(m, f + 0x0C);
            s = idx < MAX_SEARCHES ? &m->searches[idx] : NULL;
        }
        e = s ? search_next(s) : NULL;
        if (!e) { r->R_AL = 0xFF; break; }
        put_dirent_fcb(m, e, extended, attr);
        r->R_AL = 0;
        break;
    }
    case 0x13: {                        /* delete, wildcards allowed */
        wchar_t dir[MAX_PATH], one[MAX_PATH];
        dirent83 *ents = NULL;
        int count = 0, i, any = 0;
        uint8_t pat[11];
        unsigned k;
        for (k = 0; k < 11; k++) {
            uint8_t c = rb(m, f + 1 + k);
            pat[k] = (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32) : c;
        }
        if (resolve(m, "", dir, MAX_PATH, NULL, 0) ||
            list_dir(m, dir, 1, &ents, &count)) { r->R_AL = 0xFF; break; }
        for (i = 0; i < count; i++) {
            if ((ents[i].attr & 0x10) || !fcb11_match(pat, ents[i].fcb)) continue;
            _snwprintf(one, MAX_PATH, L"%ls\\%hs", dir, ents[i].name);
            one[MAX_PATH - 1] = 0;
            if (DeleteFileW(one)) any = 1;
        }
        free(ents);
        r->R_AL = any ? 0 : 0xFF;
        break;
    }
    case 0x14:                          /* sequential read */
    case 0x15: {                        /* sequential write */
        uint32_t rec = (uint32_t)rw(m, f + 0x0C) * 128 + rb(m, f + 0x20);
        r->R_AL = fcb_io(m, f, rec, 1, r->R_AH == 0x15, &done);
        if (done) {
            rec++;
            ww(m, f + 0x0C, (uint16_t)(rec / 128));
            wb(m, f + 0x20, (uint8_t)(rec % 128));
        }
        break;
    }
    case 0x17: {                        /* rename: new name at FCB+11h */
        char newname[13];
        uint8_t nn[11];
        wchar_t np[MAX_PATH];
        unsigned k;
        fcb_name(m, f, name);
        for (k = 0; k < 11; k++) {
            uint8_t c = rb(m, f + 0x11 + k);
            nn[k] = (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32) : c;
        }
        fcb11_to_name(nn, newname);
        if (resolve(m, name, path, MAX_PATH, NULL, 0) ||
            resolve(m, newname, np, MAX_PATH, NULL, 0) ||
            !MoveFileW(path, np)) r->R_AL = 0xFF;
        else r->R_AL = 0;
        break;
    }
    case 0x21:                          /* random read */
    case 0x22: {                        /* random write */
        uint32_t rec = rw(m, f + 0x21) | ((uint32_t)rb(m, f + 0x23) << 16);
        ww(m, f + 0x0C, (uint16_t)(rec / 128));
        wb(m, f + 0x20, (uint8_t)(rec % 128));
        r->R_AL = fcb_io(m, f, rec, 1, r->R_AH == 0x22, &done);
        break;
    }
    case 0x23:                          /* file size in records */
        fcb_name(m, f, name);
        if (resolve(m, name, path, MAX_PATH, NULL, 0)) { r->R_AL = 0xFF; break; }
        {
            WIN32_FILE_ATTRIBUTE_DATA ad;
            unsigned size = rw(m, f + 0x0E) ? rw(m, f + 0x0E) : 128;
            if (!GetFileAttributesExW(path, GetFileExInfoStandard, &ad)) { r->R_AL = 0xFF; break; }
            wd(m, f + 0x21, (ad.nFileSizeLow + size - 1) / size);
            r->R_AL = 0;
        }
        break;
    case 0x24: {                        /* set random record */
        uint32_t rec = (uint32_t)rw(m, f + 0x0C) * 128 + rb(m, f + 0x20);
        wd(m, f + 0x21, rec);
        break;
    }
    case 0x27:                          /* random block read */
    case 0x28: {                        /* random block write */
        uint32_t rec = rw(m, f + 0x21) | ((uint32_t)rb(m, f + 0x23) << 16);
        if (r->R_AH == 0x28 && r->R_CX == 0) {
            /* truncate or extend to the random record */
            HANDLE fhh = fcb_handle(m, f);
            unsigned size = rw(m, f + 0x0E) ? rw(m, f + 0x0E) : 128;
            LARGE_INTEGER pos;
            pos.QuadPart = (LONGLONG)rec * size;
            if (fhh && SetFilePointerEx(fhh, pos, NULL, FILE_BEGIN) && SetEndOfFile(fhh)) {
                wd(m, f + 0x10, rec * size);
                m->fh_written[rb(m, f + 0x18)] = 1;
                r->R_AL = 0;
            } else {
                r->R_AL = 1;
            }
            break;
        }
        r->R_AL = fcb_io(m, f, rec, r->R_CX, r->R_AH == 0x28, &done);
        r->R_CX = (u16)done;
        rec += done;
        wd(m, f + 0x21, rec);
        ww(m, f + 0x0C, (uint16_t)(rec / 128));
        wb(m, f + 0x20, (uint8_t)(rec % 128));
        break;
    }
    }
    return SVC_DONE;
}

/* ------------------------------------------------------------------ */
/* DOS: INT 21h                                                        */
/* ------------------------------------------------------------------ */

static int dos_read_key(gwb_machine *m, int echo)
{
    uint16_t k;

    if (m->dos_ext_scan >= 0) {
        R(m).R_AL = (u8)m->dos_ext_scan;
        m->dos_ext_scan = -1;
        return SVC_DONE;
    }
    if (!key_pop(m, &k)) return SVC_WAIT;
    if ((k & 0xFF) == 0 || (k & 0xFF) == 0xE0) m->dos_ext_scan = k >> 8;
    R(m).R_AL = (u8)((k & 0xFF) == 0xE0 ? 0 : k & 0xFF);
    if (echo && R(m).R_AL) dos_putc(m, R(m).R_AL);
    return SVC_DONE;
}

static void put_find(gwb_machine *m, search *s, const dirent83 *e)
{
    uint32_t d = lin(m->dta_seg, m->dta_off);
    unsigned i;

    for (i = 0; i < 0x15; i++) wb(m, d + i, 0);
    wb(m, d, (uint8_t)(s - m->searches));
    wb(m, d + 1, 'G');
    wb(m, d + 0x15, e->attr);
    ww(m, d + 0x16, e->time);
    ww(m, d + 0x18, e->date);
    wd(m, d + 0x1A, e->size);
    for (i = 0; i < 13; i++) wb(m, d + 0x1E + i, (uint8_t)e->name[i]);
}

static int dos(gwb_machine *m)
{
    x86emu_regs_t *r = &R(m);
    char name[128], last[13];
    wchar_t path[MAX_PATH], path2[MAX_PATH];
    HANDLE h;
    int slot, rc;
    uint32_t a;

    switch (r->R_AH) {
    case 0x00:
        return SVC_EXIT;
    case 0x01: return dos_read_key(m, 1);
    case 0x02: dos_putc(m, r->R_DL); break;
    case 0x03: r->R_AL = 0; break;      /* AUX in: nothing there */
    case 0x04: case 0x05: break;        /* AUX / PRN out: dropped */
    case 0x06:
        if (r->R_DL != 0xFF) { dos_putc(m, r->R_DL); break; }
        if (m->dos_ext_scan < 0 && !key_count(m)) { r->R_AL = 0; ret_flag(m, F_ZF, 1); break; }
        dos_read_key(m, 0);
        ret_flag(m, F_ZF, 0);
        break;
    case 0x07: case 0x08: return dos_read_key(m, 0);
    case 0x09:
        for (a = lin(r->R_DS, r->R_DX); rb(m, a) != '$'; a++) dos_putc(m, rb(m, a));
        r->R_AL = '$';
        break;
    case 0x0B: r->R_AL = (m->dos_ext_scan >= 0 || key_count(m)) ? 0xFF : 0; break;
    case 0x0C:
        key_clear(m);
        m->dos_ext_scan = -1;
        if (r->R_AL == 0x01 || r->R_AL == 0x06 || r->R_AL == 0x07 || r->R_AL == 0x08) {
            r->R_AH = r->R_AL;
            return dos(m);
        }
        break;
    case 0x0D: break;
    case 0x0E: r->R_AL = 3; break;      /* three drives, C: is ours */
    case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
    case 0x15: case 0x16: case 0x17: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x27: case 0x28:
        return dos_fcb(m);
    case 0x19: r->R_AL = 2; break;      /* current drive C: */
    case 0x1A: m->dta_seg = r->R_DS; m->dta_off = r->R_DX; break;
    case 0x25:
        ww(m, r->R_AL * 4u, r->R_DX);
        ww(m, r->R_AL * 4u + 2, r->R_DS);
        break;
    case 0x26: {
        /* create PSP at DX: GW-BASIC's MAPINI makes its data segment its
         * PSP. Copy the current one and refresh what DOS refreshes. */
        uint32_t to = (uint32_t)r->R_DX << 4, from = (uint32_t)PSP_SEG << 4;
        unsigned i;
        for (i = 0; i < 256; i++) wb(m, to + i, rb(m, from + i));
        ww(m, to + 2, MEM_TOP_SEG);
        for (i = 0; i < 3; i++) {
            ww(m, to + 0x0A + i * 4, rw(m, (0x22 + i) * 4u));
            ww(m, to + 0x0C + i * 4, rw(m, (0x22 + i) * 4u + 2));
        }
        break;
    }
    case 0x29: dos_parse(m); break;
    case 0x2A: {
        SYSTEMTIME st;
        GetLocalTime(&st);
        r->R_CX = st.wYear; r->R_DH = (u8)st.wMonth; r->R_DL = (u8)st.wDay;
        r->R_AL = (u8)st.wDayOfWeek;
        break;
    }
    case 0x2B: r->R_AL = 0; break;      /* the host's date stands */
    case 0x2C: {
        /* from the BIOS tick count, so TIMER and TIME$ agree with guest time */
        uint32_t t = rw(m, BDA + 0x6C) | ((uint32_t)rw(m, BDA + 0x6E) << 16);
        uint32_t cs = (uint32_t)((double)t * 100.0 / GWB_TICKS_PER_SEC);
        r->R_CH = (u8)(cs / 360000u);
        r->R_CL = (u8)(cs / 6000u % 60u);
        r->R_DH = (u8)(cs / 100u % 60u);
        r->R_DL = (u8)(cs % 100u);
        break;
    }
    case 0x2D: {
        uint32_t cs = r->R_CH * 360000u + r->R_CL * 6000u + r->R_DH * 100u + r->R_DL;
        wd(m, BDA + 0x6C, (uint32_t)((double)cs * GWB_TICKS_PER_SEC / 100.0));
        r->R_AL = 0;
        break;
    }
    case 0x2E: m->verify = r->R_AL & 1; break;
    case 0x2F:
        set_seg(m, R_ES_INDEX, m->dta_seg);
        r->R_BX = m->dta_off;
        break;
    case 0x30: r->R_AX = 0x1E03; r->R_BX = 0; r->R_CX = 0; break;  /* DOS 3.30 */
    case 0x33:
        if (r->R_AL == 0) r->R_DL = 0;
        else if (r->R_AL == 5) r->R_DL = 3;
        break;
    case 0x35:
        set_seg(m, R_ES_INDEX, rw(m, r->R_AL * 4u + 2));
        r->R_BX = rw(m, r->R_AL * 4u);
        break;
    case 0x36: {
        ULARGE_INTEGER fr;
        uint32_t clus = 65535;
        if (GetDiskFreeSpaceExW(m->root, &fr, NULL, NULL) && fr.QuadPart / 4096 < 65535)
            clus = (uint32_t)(fr.QuadPart / 4096);
        r->R_AX = 8; r->R_BX = (u16)clus; r->R_CX = 512; r->R_DX = 65535;
        break;
    }
    case 0x37:
        if (r->R_AL == 0) { r->R_DL = '/'; r->R_AL = 0; }
        else r->R_AL = 0;
        break;
    case 0x38: {
        unsigned i;
        a = lin(r->R_DS, r->R_DX);
        for (i = 0; i < 34; i++) wb(m, a + i, 0);
        ww(m, a, 0);                    /* USA date order */
        wb(m, a + 2, '$');
        wb(m, a + 7, ',');
        wb(m, a + 9, '.');
        wb(m, a + 11, '-');
        wb(m, a + 13, ':');
        wb(m, a + 16, 2);
        r->R_BX = 1;
        ok(m);
        break;
    }
    case 0x39: case 0x3A: case 0x3B:
        read_asciiz(m, r->R_DS, r->R_DX, name, sizeof name);
        if ((rc = resolve(m, name, path, MAX_PATH, NULL, 0)) != 0) { fail(m, (uint16_t)rc); break; }
        if (r->R_AH == 0x39) {
            if (!CreateDirectoryW(path, NULL)) { fail(m, 5); break; }
        } else if (r->R_AH == 0x3A) {
            if (wcslen(path) == wcslen(m->root) || !RemoveDirectoryW(path)) { fail(m, 5); break; }
        } else {
            DWORD at = GetFileAttributesW(path);
            size_t rl = wcslen(m->root), i, n = 0;
            if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) { fail(m, 3); break; }
            for (i = rl + (path[rl] == L'\\' ? 1 : 0); path[i] && n + 1 < sizeof m->cwd; i++)
                m->cwd[n++] = (char)path[i];
            m->cwd[n] = 0;
        }
        ok(m);
        break;
    case 0x3C:
    case 0x3D:
        read_asciiz(m, r->R_DS, r->R_DX, name, sizeof name);
        if ((rc = resolve(m, name, path, MAX_PATH, NULL, 0)) != 0) { fail(m, (uint16_t)rc); break; }
        h = open_host(path, r->R_AH == 0x3C ? 2 : (r->R_AL & 3), r->R_AH == 0x3C);
        if (h == INVALID_HANDLE_VALUE) { fail(m, win_err()); break; }
        if ((slot = handle_new(m, h)) < 0) { CloseHandle(h); fail(m, 4); break; }
        r->R_AX = (u16)slot;
        ok(m);
        break;
    case 0x3E:
        if (r->R_BX < 5) { ok(m); break; }
        if (!(h = handle_get(m, r->R_BX))) { fail(m, 6); break; }
        CloseHandle(h);
        m->fh[r->R_BX] = NULL;
        ok(m);
        break;
    case 0x3F:
    case 0x40: {
        DWORD x = 0;
        unsigned i, n = r->R_CX;
        uint8_t *buf;
        a = lin(r->R_DS, r->R_DX);
        if (r->R_BX < 5) {
            if (r->R_AH == 0x40 && (r->R_BX == 1 || r->R_BX == 2))
                for (i = 0; i < n; i++) dos_putc(m, rb(m, a + i));
            r->R_AX = r->R_AH == 0x40 ? (u16)n : 0;
            ok(m);
            break;
        }
        if (!(h = handle_get(m, r->R_BX))) { fail(m, 6); break; }
        if (r->R_AH == 0x40 && n == 0) {
            if (!SetEndOfFile(h)) { fail(m, 5); break; }
            r->R_AX = 0;
            ok(m);
            break;
        }
        buf = malloc(n ? n : 1);
        if (!buf) { fail(m, 8); break; }
        if (r->R_AH == 0x3F) {
            if (!ReadFile(h, buf, n, &x, NULL)) { free(buf); fail(m, 5); break; }
            for (i = 0; i < x; i++) wb(m, a + i, buf[i]);
        } else {
            for (i = 0; i < n; i++) buf[i] = rb(m, a + i);
            if (!WriteFile(h, buf, n, &x, NULL)) { free(buf); fail(m, 5); break; }
        }
        free(buf);
        r->R_AX = (u16)x;
        ok(m);
        break;
    }
    case 0x41:
        read_asciiz(m, r->R_DS, r->R_DX, name, sizeof name);
        if ((rc = resolve(m, name, path, MAX_PATH, NULL, 0)) != 0) { fail(m, (uint16_t)rc); break; }
        if (!DeleteFileW(path)) { fail(m, win_err()); break; }
        ok(m);
        break;
    case 0x42: {
        LARGE_INTEGER d, np;
        if (!(h = handle_get(m, r->R_BX))) { fail(m, 6); break; }
        d.QuadPart = (LONGLONG)(int32_t)(((uint32_t)r->R_CX << 16) | r->R_DX);
        if (r->R_AL == 0) d.QuadPart = (LONGLONG)(((uint32_t)r->R_CX << 16) | r->R_DX);
        if (!SetFilePointerEx(h, d, &np, r->R_AL == 0 ? FILE_BEGIN : r->R_AL == 1 ? FILE_CURRENT : FILE_END)) {
            fail(m, 0x19);
            break;
        }
        r->R_DX = (u16)(np.QuadPart >> 16);
        r->R_AX = (u16)np.QuadPart;
        ok(m);
        break;
    }
    case 0x43:
        read_asciiz(m, r->R_DS, r->R_DX, name, sizeof name);
        if ((rc = resolve(m, name, path, MAX_PATH, NULL, 0)) != 0) { fail(m, (uint16_t)rc); break; }
        {
            DWORD at = GetFileAttributesW(path);
            if (at == INVALID_FILE_ATTRIBUTES) { fail(m, 2); break; }
            if (r->R_AL == 0) {
                r->R_CX = (u16)(((at & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x20) |
                                ((at & FILE_ATTRIBUTE_READONLY) ? 1 : 0));
            }
        }
        ok(m);
        break;
    case 0x44:
        switch (r->R_AL) {
        case 0x00:
            if (r->R_BX < 5) { r->R_DX = r->R_BX <= 2 ? 0x80D3 : 0x80C0; ok(m); }
            else if (handle_get(m, r->R_BX)) { r->R_DX = 0x0002; ok(m); }
            else fail(m, 6);
            break;
        case 0x01: ok(m); break;
        case 0x06: r->R_AL = (r->R_BX == 0 && !key_count(m)) ? 0 : 0xFF; ok(m); break;
        case 0x07: r->R_AL = 0xFF; ok(m); break;
        case 0x08: r->R_AX = 1; ok(m); break;
        case 0x09: r->R_DX = 0; ok(m); break;
        default: fail(m, 1); break;
        }
        break;
    case 0x45:
    case 0x46: {
        HANDLE src, dup;
        if (r->R_BX < 5) { if (r->R_AH == 0x45) r->R_AX = r->R_BX; ok(m); break; }
        if (!(src = handle_get(m, r->R_BX))) { fail(m, 6); break; }
        if (!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(), &dup, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) { fail(m, 4); break; }
        if (r->R_AH == 0x45) {
            if ((slot = handle_new(m, dup)) < 0) { CloseHandle(dup); fail(m, 4); break; }
            r->R_AX = (u16)slot;
        } else {
            if (r->R_CX < 5 || r->R_CX >= MAX_HANDLES) { CloseHandle(dup); fail(m, 6); break; }
            if (m->fh[r->R_CX]) CloseHandle(m->fh[r->R_CX]);
            m->fh[r->R_CX] = dup;
        }
        ok(m);
        break;
    }
    case 0x47: {
        unsigned i;
        a = lin(r->R_DS, r->R_SI);
        for (i = 0; m->cwd[i] && i < 63; i++) wb(m, a + i, (uint8_t)m->cwd[i]);
        wb(m, a + i, 0);
        ok(m);
        break;
    }
    case 0x48:
        if ((uint32_t)m->heap_seg + r->R_BX <= MEM_TOP_SEG) {
            r->R_AX = m->heap_seg;
            m->heap_seg = (uint16_t)(m->heap_seg + r->R_BX);
            ok(m);
        } else {
            r->R_BX = (u16)(MEM_TOP_SEG - m->heap_seg);
            fail(m, 8);
        }
        break;
    case 0x49: ok(m); break;
    case 0x4A:
        if (r->R_ES == PSP_SEG) {
            if ((uint32_t)PSP_SEG + r->R_BX > MEM_TOP_SEG) {
                r->R_BX = (u16)(MEM_TOP_SEG - PSP_SEG);
                fail(m, 8);
                break;
            }
            m->heap_seg = (uint16_t)(PSP_SEG + r->R_BX);
        }
        ok(m);
        break;
    case 0x4B:
        read_asciiz(m, r->R_DS, r->R_DX, name, sizeof name);
        logf_(m, "INT 21h EXEC \"%s\" refused (no child programs)", name);
        fail(m, 2);
        break;
    case 0x4C:
        return SVC_EXIT;
    case 0x4D: r->R_AX = 0; break;
    case 0x4E:
    case 0x4F: {
        search *s;
        const dirent83 *e;
        if (r->R_AH == 0x4E) {
            uint8_t pat[11];
            int is_root = 0;
            read_asciiz(m, r->R_DS, r->R_DX, name, sizeof name);
            if ((rc = prepare_search(m, name, path, MAX_PATH, pat, &is_root)) != 0) { fail(m, (uint16_t)rc); break; }
            s = search_new(m);
            memcpy(s->pattern, pat, 11);
            s->attr = (uint8_t)r->R_CX;
            if (list_dir(m, path, is_root, &s->ents, &s->count)) { s->used = 0; fail(m, 3); break; }
        } else {
            a = lin(m->dta_seg, m->dta_off);
            s = (rb(m, a + 1) == 'G' && rb(m, a) < MAX_SEARCHES) ? &m->searches[rb(m, a)] : NULL;
        }
        e = s ? search_next(s) : NULL;
        if (!e) { fail(m, 0x12); break; }
        put_find(m, s, e);
        ok(m);
        break;
    }
    case 0x52:
        /* List of Lists. GWBASICA (GIODSX) reads the System File Table
         * from it to tell same-named files in different directories
         * apart. There is no SFT here; a list of all-FFh bytes makes the
         * SFT pointer FFFF:FFFF, which GIODSX treats as "none" and falls
         * back to its own DIDFBK path. */
        set_seg(m, R_ES_INDEX, LOL_SEG);
        r->R_BX = 0x10;
        break;
    case 0x54: r->R_AL = m->verify; break;
    case 0x56:
        read_asciiz(m, r->R_DS, r->R_DX, name, sizeof name);
        if ((rc = resolve(m, name, path, MAX_PATH, NULL, 0)) != 0) { fail(m, (uint16_t)rc); break; }
        read_asciiz(m, r->R_ES, r->R_DI, name, sizeof name);
        if ((rc = resolve(m, name, path2, MAX_PATH, last, 0)) != 0) { fail(m, (uint16_t)rc); break; }
        if (!MoveFileW(path, path2)) { fail(m, win_err()); break; }
        ok(m);
        break;
    case 0x57:
        if (!(h = handle_get(m, r->R_BX))) { fail(m, 6); break; }
        if (r->R_AL == 0) {
            FILETIME ft;
            uint16_t d = 0, t = 0;
            if (GetFileTime(h, NULL, NULL, &ft)) file_times(&ft, &d, &t);
            r->R_CX = t; r->R_DX = d;
        }
        ok(m);
        break;
    case 0x59: r->R_AX = 0; r->R_BX = 0; r->R_CH = 0; break;
    case 0x62: r->R_BX = PSP_SEG; break;
    default:
        logf_(m, "INT 21h AH=%02Xh not implemented (from %04X:%04X)", r->R_AH,
              rw(m, lin(r->R_SS, (uint16_t)(r->R_SP + 2))), rw(m, lin(r->R_SS, r->R_SP)));
        fail(m, 1);
        break;
    }
    return SVC_DONE;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static int service(gwb_machine *m, unsigned vec)
{
    x86emu_regs_t *r = &R(m);

    if (m->trace && vec != 0x08 && vec != 0x1C)
        logf_(m, "svc %02Xh AX=%04X BX=%04X CX=%04X DX=%04X", vec, r->R_AX, r->R_BX, r->R_CX, r->R_DX);

    switch (vec) {
    case 0x08: timer_tick(m); return SVC_DONE;
    case 0x10: return bios_video(m);
    case 0x11: r->R_AX = rw(m, BDA + 0x10); return SVC_DONE;
    case 0x12: r->R_AX = 640; return SVC_DONE;
    case 0x15:
        r->R_AH = 0x86;                 /* "function not supported" */
        ret_flag(m, F_CF, 1);
        return SVC_DONE;
    case 0x16: return bios_keyboard(m);
    case 0x17: r->R_AH = 0x01; return SVC_DONE;     /* printer: time-out */
    case 0x1A: return bios_clock(m);
    case 0x20: return SVC_EXIT;
    case 0x21: return dos(m);
    case 0x29: dos_putc(m, r->R_AL); return SVC_DONE;
    case 0x05: case 0x1B: case 0x1C: case 0x23: case 0x24: case 0x28:
    case 0x2F: case 0x33:
        return SVC_DONE;
    default:
        if (vec < 8) return SVC_DONE;   /* CPU exceptions: ignore, as a PC BIOS does */
        logf_(m, "INT %02Xh not implemented (AX=%04X)", vec, r->R_AX);
        return SVC_DONE;
    }
}

static int code_check(x86emu_t *emu)
{
    gwb_machine *m = (gwb_machine *)emu->_private;
    unsigned ip, vec;
    int rc;

    if (emu->x86.R_CS != ROM_SEG) return 0;
    ip = emu->x86.R_IP;
    if (ip >= STUB_END || (ip & 3)) return 0;
    vec = ip >> 2;

    rc = service(m, vec);
    if (rc == SVC_WAIT) {
        emu->x86.R_EIP = WAIT_BASE + vec * 8;
    } else if (rc == SVC_EXIT) {
        m->exited = 1;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Loader                                                              */
/* ------------------------------------------------------------------ */

static int load_exe(gwb_machine *m, const uint8_t *exe, size_t len, char *err, size_t errn)
{
    unsigned cblp, cp, crlc, cparhdr, ss, sp, ip, cs, lfarlc, i;
    size_t image, hdr;
    uint16_t load = PSP_SEG + 0x10;
    uint32_t psp = (uint32_t)PSP_SEG << 4, env = (uint32_t)ENV_SEG << 4;
    static const char envtext[] = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\\0\0\x01\0C:\\GWBASIC.EXE";

#define W(o) ((unsigned)exe[o] | ((unsigned)exe[(o) + 1] << 8))
    if (len < 28 || exe[0] != 'M' || exe[1] != 'Z') {
        snprintf(err, errn, "not an MZ executable");
        return 0;
    }
    cblp = W(2); cp = W(4); crlc = W(6); cparhdr = W(8);
    ss = W(14); sp = W(16); ip = W(20); cs = W(22); lfarlc = W(24);
    hdr = (size_t)cparhdr * 16;
    image = (size_t)cp * 512 - (cblp ? 512 - cblp : 0);
    if (image > len) image = len;
    if (hdr >= image || (size_t)(load << 4) + (image - hdr) > (size_t)MEM_TOP_SEG << 4) {
        snprintf(err, errn, "bad MZ header");
        return 0;
    }
    memcpy(m->mem + ((uint32_t)load << 4), exe + hdr, image - hdr);
    for (i = 0; i < crlc; i++) {
        size_t r = lfarlc + (size_t)i * 4;
        uint32_t at;
        if (r + 4 > len) break;
        at = lin(load + W(r + 2), W(r));
        ww(m, at, (uint16_t)(rw(m, at) + load));
    }

    memset(m->mem + ((uint32_t)LOL_SEG << 4), 0xFF, 0x80);

    /* environment and PSP */
    memcpy(m->mem + env, envtext, sizeof envtext);
    memset(m->mem + psp, 0, 256);
    m->mem[psp + 0] = 0xCD; m->mem[psp + 1] = 0x20;         /* INT 20h */
    ww(m, psp + 2, MEM_TOP_SEG);
    m->mem[psp + 5] = 0x9A;                                 /* CALL FAR 0:00C0 (unused) */
    ww(m, psp + 0x0A, rw(m, 0x22 * 4)); ww(m, psp + 0x0C, rw(m, 0x22 * 4 + 2));
    ww(m, psp + 0x0E, rw(m, 0x23 * 4)); ww(m, psp + 0x10, rw(m, 0x23 * 4 + 2));
    ww(m, psp + 0x12, rw(m, 0x24 * 4)); ww(m, psp + 0x14, rw(m, 0x24 * 4 + 2));
    ww(m, psp + 0x16, PSP_SEG);
    for (i = 0; i < 20; i++) m->mem[psp + 0x18 + i] = i < 5 ? (uint8_t)i : 0xFF;
    ww(m, psp + 0x2C, ENV_SEG);
    ww(m, psp + 0x32, 20);
    ww(m, psp + 0x34, 0x18); ww(m, psp + 0x36, PSP_SEG);
    m->mem[psp + 0x50] = 0xCD; m->mem[psp + 0x51] = 0x21; m->mem[psp + 0x52] = 0xCB;
    memset(m->mem + psp + 0x5C, ' ', 11); m->mem[psp + 0x5C] = 0;
    memset(m->mem + psp + 0x6C, ' ', 11); m->mem[psp + 0x6C] = 0;
    m->mem[psp + 0x5D] = ' ';
    memset(m->mem + psp + 0x5D, ' ', 11);
    memset(m->mem + psp + 0x6D, ' ', 11);
    m->mem[psp + 0x80] = 0;
    m->mem[psp + 0x81] = 0x0D;

    m->heap_seg = MEM_TOP_SEG;          /* the program owns everything until it shrinks */
    m->dta_seg = PSP_SEG;
    m->dta_off = 0x80;

    set_seg(m, R_CS_INDEX, (uint16_t)(load + cs));
    m->emu->x86.R_EIP = ip;
    set_seg(m, R_SS_INDEX, (uint16_t)(load + ss));
    m->emu->x86.R_ESP = sp;
    set_seg(m, R_DS_INDEX, PSP_SEG);
    set_seg(m, R_ES_INDEX, PSP_SEG);
    m->emu->x86.R_EAX = 0;
    m->emu->x86.R_EBX = 0;
    m->emu->x86.R_ECX = 0xFF;
    m->emu->x86.R_EDX = PSP_SEG;
    m->emu->x86.R_EFLG = 0x0202;        /* IF set */
#undef W
    return 1;
}

static void init_bda(gwb_machine *m)
{
    SYSTEMTIME st;
    double secs;

    memset(m->mem + BDA, 0, 256);
    ww(m, BDA + 0x10, 0x0021 | (2 << 4));   /* 80x25 colour, one floppy... */
    ww(m, BDA + 0x13, 640);
    ww(m, BDA + 0x1A, 0x1E);
    ww(m, BDA + 0x1C, 0x1E);
    ww(m, BDA + 0x80, 0x1E);
    ww(m, BDA + 0x82, 0x3E);

    GetLocalTime(&st);
    secs = st.wHour * 3600.0 + st.wMinute * 60.0 + st.wSecond + st.wMilliseconds / 1000.0;
    wd(m, BDA + 0x6C, (uint32_t)(secs * GWB_TICKS_PER_SEC));

    set_mode(m, 3);
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

gwb_machine *gwb_create(const void *exe, size_t exe_len, const wchar_t *root_dir,
                        char *err, size_t err_len)
{
    gwb_machine *m = calloc(1, sizeof *m);
    DWORD at;

    if (!m) { snprintf(err, err_len, "out of memory"); return NULL; }
    m->mem = calloc(1, MEM_SIZE + 16);
    if (!m->mem) { free(m); snprintf(err, err_len, "out of memory"); return NULL; }

    at = GetFileAttributesW(root_dir);
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) {
        snprintf(err, err_len, "the BASIC folder does not exist");
        free(m->mem); free(m);
        return NULL;
    }
    wcsncpy(m->root, root_dir, MAX_PATH - 1);
    {
        size_t n = wcslen(m->root);
        while (n > 3 && (m->root[n - 1] == L'\\' || m->root[n - 1] == L'/')) m->root[--n] = 0;
    }

    m->trace = getenv("GWB_TRACE") != NULL;
    InitializeCriticalSection(&m->kq_lock);
    m->dos_ext_scan = -1;

    m->emu = x86emu_new(X86EMU_PERM_RWX, X86EMU_PERM_RW);
    if (!m->emu) {
        snprintf(err, err_len, "CPU init failed");
        DeleteCriticalSection(&m->kq_lock);
        free(m->mem); free(m);
        return NULL;
    }
    m->emu->_private = m;
    x86emu_set_memio_handler(m->emu, memio);
    x86emu_set_code_handler(m->emu, code_check);

    build_rom(m);
    init_bda(m);
    if (!load_exe(m, exe, exe_len, err, err_len)) {
        gwb_destroy(m);
        return NULL;
    }
    m->tick_budget = (unsigned)(GWB_IPS / GWB_TICKS_PER_SEC);
    return m;
}

void gwb_destroy(gwb_machine *m)
{
    int i;

    if (!m) return;
    for (i = 0; i < MAX_HANDLES; i++) if (m->fh[i]) CloseHandle(m->fh[i]);
    for (i = 0; i < MAX_SEARCHES; i++) free(m->searches[i].ents);
    if (m->emu) x86emu_done(m->emu);
    DeleteCriticalSection(&m->kq_lock);
    free(m->mem);
    free(m);
}

static int can_interrupt(gwb_machine *m)
{
    return (m->emu->x86.R_EFLG & F_IF) && !m->emu->x86.intr_type;
}

/* IRQ0 comes from PIT channel 0, as on a PC: every reload / 1,193,182
 * seconds, i.e. every GWB_IPS * reload / 1193182 guest instructions. At
 * the BIOS default (reload 0 = 65536) that is the 18.2 Hz tick. It matters
 * because GW-BASIC's sound code (OEMSND.ASM) reprograms channel 0 to
 * 400 Hz while it plays notes, counts each note's length in those
 * 400 Hz interrupts, and chains to the BIOS tick about 18.2 times a
 * second; a fixed 18.2 Hz IRQ0 made every note 22 times too long. */
static double irq0_period(gwb_machine *m)
{
    uint32_t reload = m->pit_reload[0] ? m->pit_reload[0] : 65536u;
    return (double)GWB_IPS * reload / 1193182.0;
}

/* The tick's speaker events as 16-bit PCM at GWB_AUDIO_RATE: a square
 * wave at 1,193,182 / divisor while port 61h has both the timer gate and
 * the speaker data bits set, silence otherwise. Phase carries across
 * ticks so a held note has no seams. */
static void render_pcm(gwb_machine *m, unsigned budget)
{
    double want = (double)GWB_AUDIO_RATE / GWB_TICKS_PER_SEC + m->pcm_carry;
    int n = (int)want, i, e = 0;
    uint8_t on = m->spk_on;
    uint16_t div = m->spk_div;

    m->pcm_carry = want - n;
    if (n > (int)(sizeof m->pcm / sizeof m->pcm[0])) n = (int)(sizeof m->pcm / sizeof m->pcm[0]);
    for (i = 0; i < n; i++) {
        uint32_t t = (uint32_t)((uint64_t)i * budget / (unsigned)n);
        double hz;
        while (e < m->spk_nev && m->spk_ev[e].at <= t) {
            on = m->spk_ev[e].on;
            div = m->spk_ev[e].div;
            e++;
        }
        hz = 1193182.0 / (div ? div : 65536u);
        if (on && hz >= 20.0 && hz <= 15000.0) {
            m->spk_phase += hz / GWB_AUDIO_RATE;
            m->spk_phase -= (int)m->spk_phase;
            m->pcm[i] = (int16_t)(m->spk_phase < 0.5 ? 5000 : -5000);
        } else {
            m->pcm[i] = 0;
        }
    }
    while (e < m->spk_nev) { on = m->spk_ev[e].on; div = m->spk_ev[e].div; e++; }
    m->spk_on = on;
    m->spk_div = div;
    m->spk_nev = 0;
    m->pcm_n = n;
}

unsigned gwb_run_tick(gwb_machine *m)
{
    x86emu_t *emu = m->emu;
    double want = GWB_IPS / GWB_TICKS_PER_SEC + m->budget_carry;
    unsigned budget = (unsigned)want;
    uint64_t t0, end, slept = 0;
    unsigned result = 0;
    int guard = 0;

    m->pcm_n = 0;
    if (m->exited) return GWB_TICK_EXITED;
    m->budget_carry = want - budget;

    if (InterlockedExchange(&m->break_req, 0)) {
        /* what the BIOS keyboard interrupt does for Ctrl-Break */
        key_clear(m);
        m->dos_ext_scan = -1;
        wb(m, BDA + 0x71, (uint8_t)(rb(m, BDA + 0x71) | 0x80));
        gwb_push_key(m, 0x0000);
        m->break_pending = 1;
    }
    key_deliver(m);

    t0 = gnow(m);
    end = t0 + budget;
    m->tick_t0 = t0;
    m->spk_nev = 0;
    if (m->next_irq0 <= 0) m->next_irq0 = (double)t0 + irq0_period(m);

    while (gnow(m) < end && guard++ < 200000) {
        uint64_t now = gnow(m), target;

        if ((double)now >= m->next_irq0) {
            m->irq0_pending = 1;
            m->next_irq0 += irq0_period(m);
            if (m->next_irq0 <= (double)now) m->next_irq0 = (double)now + irq0_period(m);
        }
        if (can_interrupt(m)) {
            if (m->break_pending) {
                x86emu_intr_raise(emu, 0x1B, INTR_TYPE_SOFT, 0);
                m->break_pending = 0;
            } else if (m->irq0_pending) {
                x86emu_intr_raise(emu, 0x08, INTR_TYPE_SOFT, 0);
                m->irq0_pending = 0;
            }
        }
        target = end;
        if (m->next_irq0 < (double)end) target = (uint64_t)m->next_irq0 + 1;
        if (target <= now) target = now + 1;

        emu->max_instr = emu->x86.R_TSC + (target - now);
        x86emu_run(emu, X86EMU_RUN_MAX_INSTR);
        if (m->exited) return GWB_TICK_EXITED;
        if (emu->x86.mode & _MODE_HALTED) {
            emu->x86.mode &= ~_MODE_HALTED;
            if ((m->irq0_pending || m->break_pending) && can_interrupt(m)) continue;
            if (key_count(m) || m->dos_ext_scan >= 0 || key_deliver(m)) continue;
            /* The CPU sleeps until the next interrupt: guest time jumps
             * there, or to the end of the tick, whichever is first. */
            now = gnow(m);
            if (target > now) {
                m->idle_skip += target - now;
                slept += target - now;
            }
        }
    }
    /* idle: the CPU sat halted for at least half of the tick, waiting
     * for an interrupt (a keystroke, mostly) with nothing to run */
    if (slept * 2 >= budget) result |= GWB_TICK_IDLE;
    render_pcm(m, budget);
    m->ticks++;
    return result;
}

int gwb_audio(gwb_machine *m, const int16_t **pcm)
{
    *pcm = m->pcm;
    return m->pcm_n;
}

void gwb_push_key(gwb_machine *m, uint16_t key)
{
    EnterCriticalSection(&m->kq_lock);
    if (((m->kq_tail + 1) & 63u) != m->kq_head) {
        m->kq[m->kq_tail] = key;
        m->kq_tail = (m->kq_tail + 1) & 63u;
    }
    LeaveCriticalSection(&m->kq_lock);
}

int gwb_key_room(gwb_machine *m)
{
    int used;
    EnterCriticalSection(&m->kq_lock);
    used = (int)((m->kq_tail - m->kq_head) & 63u);
    LeaveCriticalSection(&m->kq_lock);
    return 63 - used;
}

void gwb_set_shift_flags(gwb_machine *m, uint8_t flags)
{
    InterlockedExchange(&m->shift_flags, flags);
}

void gwb_ctrl_break(gwb_machine *m)
{
    InterlockedExchange(&m->break_req, 1);
}

void gwb_snapshot(gwb_machine *m, gwb_screen *s)
{
    unsigned page = rb(m, BDA + 0x62), row, col;

    memcpy(s->vram, m->mem + VRAM_BASE, GWB_VRAM_BYTES);
    s->mode = vmode(m);
    s->cols = (uint8_t)vcols(m);
    s->page_start = rw(m, BDA + 0x4E);
    if (m->crtc_cursor_live) {
        unsigned addr = ((unsigned)m->crtc[0x0E] << 8 | m->crtc[0x0F]) - s->page_start / 2;
        s->cur_row = (uint8_t)(addr / s->cols);
        s->cur_col = (uint8_t)(addr % s->cols);
        s->cur_start = m->crtc[0x0A];
        s->cur_end = m->crtc[0x0B];
    } else {
        cursor_get(m, page, &row, &col);
        s->cur_row = (uint8_t)row;
        s->cur_col = (uint8_t)col;
        s->cur_start = rb(m, BDA + 0x61);
        s->cur_end = rb(m, BDA + 0x60);
    }
    s->cga_color = m->cga_color;
    s->cga_mode = m->cga_mode;
    s->speaker_on = (m->port61 & 3) == 3;
    s->speaker_divisor = m->pit_reload[2];
}

uint64_t gwb_ticks(gwb_machine *m)        { return m->ticks; }
uint64_t gwb_instructions(gwb_machine *m) { return m->emu->x86.R_TSC; }

void gwb_set_log(gwb_machine *m, void (*fn)(void *, const char *), void *ctx)
{
    m->log = fn;
    m->log_ctx = ctx;
}

int gwb_screen_text(const gwb_screen *s, char *buf)
{
    unsigned r, c, n = 0;

    if (s->mode > 3) return 0;
    for (r = 0; r < 25; r++) {
        for (c = 0; c < s->cols; c++) {
            uint8_t ch = s->vram[(s->page_start + (r * s->cols + c) * 2) & 0x3FFF];
            buf[n++] = (char)(ch == 0 || ch == 0xFF ? ' ' : ch < 32 || ch == 127 ? '.' : ch);
        }
        while (n && buf[n - 1] == ' ') n--;
        buf[n++] = '\n';
    }
    buf[n] = 0;
    return 1;
}
