/*
 * nabu_core.c - the NABU machine, lifted out of Marduk's main().
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * ------------------------------------------------------------------
 * DERIVED WORK NOTICE
 * ------------------------------------------------------------------
 *
 * The memory map, the port map, the interrupt priority encoder, the
 * keyboard buffer and the scanline loop below are derived from Marduk's
 * main.c, which is MIT:
 *
 *   Copyright 2022, 2023 S. V. Nickolas.
 *   Copyright 2023 Marcin Woloszczuk.
 *
 * The full notice is in third_party/marduk/license.txt and travels with
 * this file. What is NOT from Marduk is the shape: the host layer is
 * gone, the globals are confined to this translation unit, the loop is
 * re-entrant per frame instead of running forever, and the machine
 * reports on itself so a caller can prove it booted. See
 * third_party/marduk/MGT-CHANGES.md.
 *
 * ------------------------------------------------------------------
 * WHAT WAS DROPPED FROM THE LOOP, AND WHY IT IS SAFE
 * ------------------------------------------------------------------
 *
 * Marduk's per-scanline work is:
 *
 *     disksys_tick()          kept. Inert with no floppy mounted.
 *     modem poll -> HCCA int  kept. This is the whole point.
 *     keyboard buffer -> int  kept.
 *     every_scanline()        DROPPED. It is keyboard_poll() (SDL input)
 *                             plus throttle() (host sleep). Phase 3.
 *     the watchdog kick       kept. The firmware expects the keyboard to
 *                             say 0x94 about every 3.7 seconds and sulks
 *                             if it stops.
 *     render_scanline()       DROPPED. Reads pixels OUT of the VDP; the
 *                             VDP's state does not depend on it. Phase 2.
 *     next_frame()            DROPPED. SDL present. Phase 2.
 *     the VBlank interrupt    KEPT, and this is the one that matters.
 *                             It is raised by the loop from TMS_REG_1,
 *                             not by the renderer, so dropping the
 *                             renderer does not cost the machine its
 *                             frame interrupt. Had it come from inside
 *                             render_scanline, a headless boot would
 *                             hang and this comment would be a bug
 *                             report instead.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#include "nabu_core.h"
#include "nabu_channel.h"

#include "tms9918.h"
#include "tms_util.h"   /* vrEmuTms9918Palette */
#include "emu2149.h"
#include "z80.h"
#include "disk.h"
#include "modem.h"
#include "marduk_diag.h"

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

static nabu_diag_fn g_diag_fn   = NULL;
static void        *g_diag_user = NULL;

void nabu_core_set_diag(nabu_diag_fn fn, void *user)
{
    g_diag_fn   = fn;
    g_diag_user = user;
}

/* Port-access trace, identical in format to the instrumented upstream
 * Marduk in the scratch tree, so the two can be diffed line for line.
 * This is how the boot stall was found; it is kept because it is the
 * only tool that answers "where did these two machines diverge". */
static nabu_port_trace_fn g_ptrace_fn   = NULL;
static void              *g_ptrace_user = NULL;

void nabu_core_set_port_trace(nabu_port_trace_fn fn, void *user)
{
    g_ptrace_fn   = fn;
    g_ptrace_user = user;
}

static nabu_hcca_trace_fn g_hcca_fn   = NULL;
static void              *g_hcca_user = NULL;

void nabu_core_set_hcca_trace(nabu_hcca_trace_fn fn, void *user)
{
    g_hcca_fn   = fn;
    g_hcca_user = user;
}

/* Called by the vendored Marduk files in place of printf/perror. */
void marduk_diag(const char *fmt, ...)
{
    char    buf[512];
    va_list ap;

    if (!g_diag_fn) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_diag_fn(buf, g_diag_user);
}

int marduk_socket_error(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

/* ------------------------------------------------------------------ */
/* Machine state. One machine per process; see the header.             */
/* ------------------------------------------------------------------ */

/* The NABU has 64K of RAM. Early machines have a 4K ROM, later ones 8K. */
static uint8_t RAM[65536], ROM[8192];
static int     romsize;

static z80           cpu;
static VrEmuTms9918 *vdp;
static PSG          *psg;
static int           ctrlreg;

static int      gotmodem;
static unsigned dog_speed;
static unsigned long next;          /* next cycle count for the scanline */
static unsigned next_watchdog;
static int      scanline;

/* PSG port A is the interrupt mask the firmware programs; port B is what
 * the priority encoder feeds back. See update_interrupts. */
static uint8_t psg_portb      = 0;
static uint8_t psg_porta      = 0;
static uint8_t psg_reg_address = 0x00;

static uint8_t hccarint = 0;   /* HCCA receive buffer full   */
static uint8_t hccatint = 0;   /* HCCA transmit buffer empty */
static uint8_t keybdint = 0;
static uint8_t vdpint   = 0;
static uint8_t interrupts = 0;

static char    keyboard_buffer[256];
static uint8_t keyboard_buffer_write_ptr = 0;
static uint8_t keyboard_buffer_read_ptr  = 0;

/* The VDP's output, retained.
 *
 * Phase 1 threw each scanline away and only ran the chip because
 * generating pixels is what makes it set its own status flags. Phase 2
 * keeps them: this is the emulated screen, 256 by 192 TMS9918 colour
 * INDICES, one byte per pixel. The palette lookup is the caller's job,
 * because the caller knows what pixel format it wants and the machine
 * should not care.
 *
 * Written only by whatever thread drives nabu_core_run_*, read by
 * nabu_core_copy_frame. See the header on the locking. */
static uint8_t  g_vdp_frame[TMS9918_PIXELS_X * TMS9918_PIXELS_Y];

/* The border colour for the frame just completed. The visible 256x192 is
 * a window in the middle of a bigger raster, and real NABU software sets
 * the border deliberately, so it is reported rather than assumed black. */
static uint8_t  g_border_index;

/* The real keyboard's heartbeat interval, in milliseconds of wall clock. */
#define NABU_WATCHDOG_MS  3700
static ULONGLONG g_dog_last_ms = 0;

/* Consecutive reads of the HCCA port that found nothing. A long run of
 * these means the guest is spinning on the wire waiting for the adapter,
 * which is the one situation where the emulated clock must not be
 * allowed to run ahead of the real one. See nabu_core_hcca_waiting. */
static int      g_hcca_dry = 0;

/* Wall-clock stamp of the last byte the guest read from the wire. */
static ULONGLONG g_hcca_last_rx_ms = 0;

static int      g_dog_override_set = 0;
static int      g_inited   = 0;
static uint64_t g_hcca_rx  = 0;
static uint64_t g_hcca_tx  = 0;
static uint64_t g_frames   = 0;

/* Diagnostic override for the emulated keyboard watchdog; 0 disables it.
 * Declared here because it touches the machine state below. */
void nabu_core_set_watchdog(unsigned scanlines)
{
    dog_speed = scanlines;
    g_dog_override_set = 1;
}

/* ------------------------------------------------------------------ */
/* Memory. The ROM overlays the bottom of RAM until it is banked out.   */
/* ------------------------------------------------------------------ */

static uint8_t mem_read(void *blob, uint16_t addr)
{
    (void)blob;
    if ((!(ctrlreg & 0x01)) && (addr < romsize))
        return ROM[addr];
    return RAM[addr];
}

static void mem_write(void *blob, uint16_t addr, uint8_t val)
{
    (void)blob;
    RAM[addr] = val;
}

/* ------------------------------------------------------------------ */
/* Interrupts: a 74LS148-style priority encoder, as the hardware has.   */
/* ------------------------------------------------------------------ */

static void int_prio_enc(int EI, int I0, int I1, int I2, int I3, int I4,
                         int I5, int I6, int I7,
                         int *GS, int *Q0, int *Q1, int *Q2, int *EO)
{
    *GS = 0;
    *EO = 1;
    *Q0 = *Q1 = *Q2 = 0;
    if (EI == 1) {
        *GS = 1; *Q0 = 1; *Q1 = 1; *Q2 = 1;
    } else if (I0 & I1 & I2 & I3 & I4 & I5 & I6 & I7) {
        *GS = *Q0 = *Q1 = *Q2 = 1;
        *EO = 0;
    } else if (!I7) {
        /* nop */
    } else if (!I6) {
        *Q0 = 1;
    } else if (!I5) {
        *Q1 = 1;
    } else if (!I4) {
        *Q0 = *Q1 = 1;
    } else if (!I3) {
        *Q2 = 1;
    } else if (!I2) {
        *Q0 = *Q2 = 1;
    } else if (!I1) {
        *Q1 = *Q2 = 1;
    } else {
        *Q0 = *Q1 = *Q2 = 1;
    }
}

static void int_prio_enc_alt(int EI, int ints, int *GS, int *Q0,
                             int *Q1, int *Q2, int *EO)
{
    int_prio_enc(EI, ints & 0x01, (ints & 0x02) >> 1,
                 (ints & 0x04) >> 2, (ints & 0x08) >> 3,
                 (ints & 0x10) >> 4, (ints & 0x20) >> 5,
                 (ints & 0x40) >> 6, (ints & 0x80) >> 7,
                 GS, Q0, Q1, Q2, EO);
}

static void update_interrupts(void)
{
    int int_prio, GS, Q0, Q1, Q2, EO;

    if (hccarint) interrupts |=  0x80; else interrupts &= ~0x80;
    if (hccatint) interrupts |=  0x40; else interrupts &= ~0x40;
    if (keybdint) interrupts |=  0x20; else interrupts &= ~0x20;
    if (vdpint)   interrupts |=  0x10; else interrupts &= ~0x10;

    int_prio = ~(interrupts & psg_porta);
    int_prio_enc_alt(0, int_prio, &GS, &Q0, &Q1, &Q2, &EO);
    psg_portb &= 0xf0;
    psg_portb |= EO | (Q0 << 1) | (Q1 << 2) | (Q2 << 3);
    PSG_writeReg(psg, 15, psg_portb);
    z80_gen_int(&cpu, !GS, psg_portb & 0x0e);
}

/* ------------------------------------------------------------------ */
/* Keyboard buffer. A 256-entry ring with wrapping 8-bit pointers.      */
/* ------------------------------------------------------------------ */

static void keyboard_buffer_put(uint8_t code)
{
    keyboard_buffer[keyboard_buffer_write_ptr++] = (char)code;
}

static int keyboard_buffer_empty(void)
{
    return keyboard_buffer_read_ptr == keyboard_buffer_write_ptr;
}

static uint8_t keyboard_buffer_get(void)
{
    if (keyboard_buffer_read_ptr != keyboard_buffer_write_ptr)
        return (uint8_t)keyboard_buffer[keyboard_buffer_read_ptr++];
    return 255;
}

/* ------------------------------------------------------------------ */
/* Ports.                                                              */
/*                                                                     */
/*   00  control register (write): bit 0 banks the ROM out, bit 1       */
/*       enables video, bit 2 strobes the parallel port, bits 3/4/5 are */
/*       the CHECK, ALERT and PAUSE lamps                              */
/*   40  PSG data, 41 PSG address (backwards from the MSX)             */
/*   80  HCCA, the cable modem                                          */
/*   90  keyboard data, 91 keyboard strobe                              */
/*   A0  TMS9918 data, A1 TMS9918 address/control                       */
/*   B0  parallel port data                                             */
/*   Cx  floppy controller                                              */
/*                                                                     */
/* Source: Nabu_Computer_Technical_Manual_by_MJP, vintagecomputer.ca.   */
/* ------------------------------------------------------------------ */

/* Per-port read counters. A stalled machine is always waiting on
 * something; the port it hammers says what. Cheap enough to leave in. */
static uint64_t g_port_reads[256];

static uint8_t port_read_inner(z80 *mycpu, uint8_t port);
static void    port_write_inner(z80 *mycpu, uint8_t port, uint8_t val);

static uint8_t port_read(z80 *mycpu, uint8_t port)
{
    uint8_t v = port_read_inner(mycpu, port);
    if (g_ptrace_fn)
        g_ptrace_fn('R', port, v, mycpu ? mycpu->pc : 0, g_ptrace_user);
    return v;
}

static void port_write(z80 *mycpu, uint8_t port, uint8_t val)
{
    if (g_ptrace_fn)
        g_ptrace_fn('W', port, val, mycpu ? mycpu->pc : 0, g_ptrace_user);
    port_write_inner(mycpu, port, val);
}

static uint8_t port_read_inner(z80 *mycpu, uint8_t port)
{
    uint8_t t, b;
    (void)mycpu;

    g_port_reads[port]++;

    if ((port & 0xF0) == 0xC0) return disksys_read(port);

    switch (port) {
    case 0x40:
        return PSG_readReg(psg, psg_reg_address);
    case 0x41:
        /* Upstream calls fatal_diag here and exits the process. A tab
         * may not kill the Suite because a guest program did something
         * odd, so this returns 0 and says so instead. */
        marduk_diag("NABU: unexpected I/O read from port 0x41\n");
        return 0;
    case 0x80:
        if (gotmodem) {
            t = modem_read(&b);
            if (t) {
                g_hcca_rx++;
                g_hcca_dry = 0;
                g_hcca_last_rx_ms = GetTickCount64();
                if (g_hcca_fn) g_hcca_fn('R', b, g_hcca_user);
                /*
                 * RE-ARM IMMEDIATELY IF ANOTHER BYTE IS ALREADY HERE.
                 *
                 * The loop raises this interrupt once per scanline, so
                 * before this the guest could take at most one byte
                 * every 63 microseconds of machine time no matter how
                 * much had already arrived. A whole TCP segment then
                 * trickled in over a thousand scanlines and the boot
                 * crawled at about 800 bytes a second.
                 *
                 * Real hardware does not work that way. The receive
                 * interrupt follows the UART's receive register, and
                 * the moment the next byte is latched the line is high
                 * again. Asking the socket right here is the faithful
                 * thing as well as the fast one, and it costs one
                 * non-blocking select on a socket that has just been
                 * read.
                 */
                hccarint = modem_bytes_available() ? 1 : 0;
                update_interrupts();
                return b;
            }
            /* Nothing there. A run of these is the guest spinning on
             * the wire, which is what nabu_core_hcca_waiting reports.
             * That used to gate the fast boot; since 2026-08-07 the
             * driver runs at 1x always and the count is observation. */
            if (g_hcca_dry < 0x7fffffff) g_hcca_dry++;
        }
        return 0;
    case 0x90:
        t = keyboard_buffer_get();
        keybdint = 0;
        update_interrupts();
        return (t == 255) ? 0 : t;
    case 0x91:
        return keyboard_buffer_empty() ? 0x00 : 0xff;
    case 0xA0:
        return vrEmuTms9918ReadData(vdp);
    case 0xA1:
        b = vrEmuTms9918ReadStatus(vdp);
        vdpint = 0;
        update_interrupts();
        return b;
    default:
        return 0;
    }
}

static void port_write_inner(z80 *mycpu, uint8_t port, uint8_t val)
{
    uint8_t psg_reg7;
    (void)mycpu;

    if ((port & 0xF0) == 0xC0) disksys_write(port, val);

    switch (port) {
    case 0x00:
        /* Upstream also strobes a real LPT file here. The Suite has no
         * printer, so the parallel port is data-only. */
        ctrlreg = val;
        return;
    case 0x40:
        psg_reg7 = PSG_readReg(psg, 7);
        if (psg_reg_address == 0x0E) {
            if (!(psg_reg7 & 0x40))
                marduk_diag("NABU: write to PSG port A while it is an input\n");
            if (psg_porta != val) {
                psg_porta = val;
                update_interrupts();
            }
        }
        if (psg_reg_address == 0x0F && !(psg_reg7 & 0x80))
            marduk_diag("NABU: write to PSG port B while it is an input\n");
        PSG_writeReg(psg, psg_reg_address, val);
        return;
    case 0x41:
        /* Upstream exits the process if this is out of range. Same
         * reasoning as port 0x41 above: clamp and report. */
        if (val > 0x1f) {
            marduk_diag("NABU: PSG register address 0x%02X out of range\n", val);
            return;
        }
        psg_reg_address = val;
        return;
    case 0x80:
        if (gotmodem) {
            modem_write(val);
            g_hcca_tx++;
            /* The guest has just asked the adapter something, so any
             * poll from here on is a wait for the answer. Start the
             * dry count fresh. */
            g_hcca_dry = 0;
            if (g_hcca_fn) g_hcca_fn('W', val, g_hcca_user);
        }
        /*
         * A NOTE ON THE TRANSMIT INTERRUPT, SO NOBODY RE-RUNS THIS.
         *
         * Upstream holds "transmit buffer empty" high forever, on the
         * grounds that a socket is always ready. That looked like a
         * prime suspect for the stall this port hit: a permanently
         * asserted interrupt through a priority encoder should storm the
         * moment the firmware enables it in the PSG mask.
         *
         * It was tried. Clearing the flag on write and raising it again
         * on the next scanline, which is what the real shift register
         * does, changed NOTHING: same three bytes out, same eight back,
         * same stall in the same place. DISPROVEN. The cause was the VDP
         * status flag; see nabu_core_run_scanlines.
         *
         * Upstream's behaviour is kept because it is upstream's, and
         * because the experiment says it is not what was wrong.
         */
        return;
    case 0xA0:
        vrEmuTms9918WriteData(vdp, val);
        return;
    case 0xA1:
        vrEmuTms9918WriteAddr(vdp, val);
        return;
    case 0xB0:
        return;
    default:
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

static void init_cpu(void)
{
    z80_init(&cpu);

    cpu.read_byte  = mem_read;
    cpu.write_byte = mem_write;
    cpu.port_in    = port_read;
    cpu.port_out   = port_write;

    next     = 228;
    scanline = 0;

    /* The real keyboard announces itself with 0x95 at power-on, and the
     * firmware waits for it. */
    keyboard_buffer_put(0x95);

    psg_portb  = 0;
    psg_porta  = 0;
    hccarint   = 0;
    vdpint     = 0;
    interrupts = 0;
    keybdint   = 1;
    /* Transmit buffer empty is held high forever: a socket never makes
     * the guest wait for the wire the way a real 6402 UART would. */
    hccatint   = 1;
    update_interrupts();
}

static int load_rom(const char *path, char *err, size_t errcap)
{
    FILE *f;
    long  sz;
    size_t got;

    if (!path || !*path) {
        if (err) snprintf(err, errcap, "no boot ROM path was given");
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        if (err) snprintf(err, errcap, "cannot open the boot ROM '%s'", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz != 4096 && sz != 8192) {
        fclose(f);
        if (err)
            snprintf(err, errcap,
                     "'%s' is %ld bytes; a NABU boot ROM is 4096 or 8192",
                     path, sz);
        return -1;
    }
    got = fread(ROM, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        if (err) snprintf(err, errcap, "short read on the boot ROM '%s'", path);
        return -1;
    }
    romsize = (int)sz;
    return 0;
}

/*
 * CHOOSE THE ADAPTER'S CHANNEL, BEFORE THE Z80 RUNS.
 *
 * WHY THE CORE DOES THIS AND NOT THE FIRMWARE. A real NABU changes
 * channel by sending CHANGE_CHANNEL, 0x85, to the adapter and then two
 * bytes of channel code. Neither firmware the Suite can boot ever sends
 * it: MGT IPL goes straight to a packet request, and so does stock
 * OpenNabu IPL, which was checked line by line. Only the original NABU
 * firmware asks, and it asks the USER, on a keyboard, which is no use to
 * a tab with a channel already chosen in a field above the screen. So
 * without this the channel would be whatever nabud's connection is
 * configured for and the Channel field would be a decoration.
 *
 * WHAT GOES ON THE WIRE IS EXACTLY WHAT A NABU SENDS. This is not a
 * private side channel or a nabud extension: it is the classic adapter
 * message, in the documented order, and nabud cannot tell it from a real
 * machine changing channel. Verified against nabud's own source, in
 * adaptor.c, adaptor_msg_change_channel:
 *
 *   we send    0x85
 *   adapter    0x10 0x06        (ACK)
 *   we send    channel low, channel high   (LITTLE ENDIAN, nabu_get_uint16)
 *   adapter    0xE4             (CONFIRMED)
 *
 * The timing is the one liberty taken: the message is sent at connect,
 * before the guest has executed an instruction, rather than by the guest.
 * A whole well-formed message goes out before the guest's first one, so
 * the adapter's state machine sees an ordinary sequence.
 *
 * IT CANNOT BREAK A BOOT THAT WOULD OTHERWISE WORK. Every failure path
 * here gives up and leaves the connection on the server's default. nabud
 * itself treats an unknown or out-of-range channel the same way: it logs
 * it, keeps the current channel, and still confirms. Since the Suite's
 * default channel is 1 and the Greek Times connection is configured for
 * channel 1, the out-of-box boot lands in the same place whether this
 * succeeds, fails or is skipped.
 */
static int nabu_core_read_byte_timeout(uint8_t *out, unsigned ms)
{
    ULONGLONG deadline = GetTickCount64() + ms;
    for (;;) {
        if (modem_read(out)) {
            g_hcca_rx++;
            if (g_hcca_fn) g_hcca_fn('R', *out, g_hcca_user);
            return 1;
        }
        if (GetTickCount64() >= deadline) return 0;
        Sleep(1);
    }
}

/*
 * The two halves of the transport, as nabu_channel wants them: a byte
 * out through the vendored modem, and a byte in with a deadline. Both
 * keep the counters and the byte trace fed, so a channel change looks on
 * a trace exactly like the traffic either side of it.
 */
static int nabu_core_ch_write(void *ctx, uint8_t b)
{
    (void)ctx;
    modem_write(b);
    g_hcca_tx++;
    if (g_hcca_fn) g_hcca_fn('W', b, g_hcca_user);
    return 1;
}

static int nabu_core_ch_read(void *ctx, uint8_t *b, unsigned ms)
{
    (void)ctx;
    return nabu_core_read_byte_timeout(b, ms);
}

void nabu_core_select_channel(int channel)
{
    char err[160];

    if (!gotmodem) return;

    /* The exchange itself lives in nabu_channel.c, because the serial
     * bridge needs the same bytes over a socket of its own and the
     * protocol is not a thing to have two copies of. */
    if (nabu_channel_select(channel, nabu_core_ch_write, nabu_core_ch_read,
                            NULL, err, sizeof err))
        marduk_diag("NABU: channel %d selected on the adapter\n", channel);
    else
        marduk_diag("NABU: %s, staying on the server's default channel\n",
                    err);
}

int nabu_core_init(const nabu_core_config_t *cfg, char *err, size_t errcap)
{
    const char *host;
    int         port;
    char        portstr[16];
    int         e;

    if (g_inited) {
        if (err) snprintf(err, errcap, "a NABU is already running");
        return -1;
    }
    if (err && errcap) err[0] = '\0';

    host = (cfg && cfg->host && cfg->host[0]) ? cfg->host : NABU_CORE_DEFAULT_HOST;
    port = (cfg && cfg->port > 0) ? cfg->port : NABU_CORE_DEFAULT_PORT;

    memset(RAM, 0, sizeof RAM);
    memset(ROM, 0, sizeof ROM);
    memset(g_vdp_frame, 0, sizeof g_vdp_frame);
    g_border_index = 0;
    g_dog_last_ms  = GetTickCount64();
    g_hcca_dry     = 0;
    g_hcca_last_rx_ms = GetTickCount64();
    romsize = 0;

    /*
     * The control register starts at 0x3A, not 0. Upstream sets it
     * deliberately and says why: the first thing the ROM does is
     * initialize this register, which flicks the lamps off and drops TV
     * mode, so the machine has to START with the lamps on and TV mode
     * set for that to be a transition rather than a no-op.
     *
     * 0x3A is video enable, the parallel strobe, and all three lamps.
     * Bit 0 stays clear, so the ROM is banked in, which is how a NABU
     * powers on.
     */
    ctrlreg = 0x3A;
    keyboard_buffer_read_ptr = keyboard_buffer_write_ptr = 0;
    g_hcca_rx = g_hcca_tx = g_frames = 0;
    next_watchdog = 0;

    if (load_rom(cfg ? cfg->rom_path : NULL, err, errcap) != 0)
        return -1;

    vdp = vrEmuTms9918New();
    if (!vdp) {
        if (err) snprintf(err, errcap, "out of memory building the video chip");
        return -1;
    }

    /* 1789772 Hz is the NABU's PSG clock. The second argument is the
     * rate PSG_calc produces samples at, and it is now the rate the
     * Suite's waveOut device is opened with, so one PSG_calc is one
     * output sample and no resampling happens anywhere. */
    psg = PSG_new(1789772, NABU_AUDIO_RATE);
    if (!psg) {
        vrEmuTms9918Destroy(vdp);
        vdp = NULL;
        if (err) snprintf(err, errcap, "out of memory building the sound chip");
        return -1;
    }
    PSG_setVolumeMode(psg, 2);
    PSG_reset(psg);

    /* 58000 scanlines is about 3.7 seconds at 15720 lines a second, which
     * is how often the real keyboard kicks the watchdog. */
    if (!g_dog_override_set) dog_speed = 58000;

    /* The floppy controller. Its state is file-scope in disk.c and only
     * zero by accident of being static, which is true once per process
     * and not once per session. The Suite connects and disconnects
     * repeatedly, so it gets initialized properly. */
    disksys_init();

    /* init_cpu calls update_interrupts, which writes to the PSG, so the
     * chips have to exist first. */
    init_cpu();

    snprintf(portstr, sizeof portstr, "%d", port);
    e = modem_init((char *)host, portstr);
    gotmodem = !e;
    if (!gotmodem && err && errcap && !err[0])
        snprintf(err, errcap, "could not reach the channel server at %s:%d",
                 host, port);

    /* The wire is up and the Z80 has not run an instruction yet, which is
     * the only moment the channel can be chosen. See the function. */
    if (gotmodem && cfg && cfg->channel > 0)
        nabu_core_select_channel(cfg->channel);

    g_inited = 1;
    return 0;
}

void nabu_core_reset(void)
{
    void *tmp;
    if (!g_inited) return;
    tmp = cpu.userdata;
    init_cpu();
    cpu.userdata = tmp;
}

void nabu_core_shutdown(void)
{
    if (!g_inited) return;
    if (gotmodem) { modem_deinit(); gotmodem = 0; }
    if (psg) { PSG_delete(psg); psg = NULL; }
    if (vdp) { vrEmuTms9918Destroy(vdp); vdp = NULL; }
    g_inited = 0;
}

/* ------------------------------------------------------------------ */
/* The loop                                                            */
/* ------------------------------------------------------------------ */

void nabu_core_run_scanlines(int lines)
{
    int done = 0;

    if (!g_inited) return;

    while (done < lines) {
        if (cpu.cyc > next) {
            disksys_tick();

            /* A byte waiting on the wire raises the HCCA receive
             * interrupt. This is the line the whole phase exists for. */
            if (gotmodem && modem_bytes_available()) {
                hccarint = 1;
                update_interrupts();
            }

            if (!keyboard_buffer_empty() && !keybdint) {
                keybdint = 1;
                update_interrupts();
            }

            /* every_scanline() went here. See the header comment. */

            /*
             * THE KEYBOARD WATCHDOG IS ON THE WALL CLOCK, NOT THE
             * EMULATED ONE.
             *
             * A real NABU keyboard is its own little machine. It sends
             * 0x94 about every 3.7 seconds of REAL time, and it does
             * not know or care what the CPU is doing. Upstream counts
             * scanlines instead, which is the same thing only while the
             * emulation runs at exactly 1x.
             *
             * It stopped being the same thing when the driver was
             * allowed to run the Z80 faster than real time: a scanline
             * counter then kicked the dog many times a second in real
             * terms. That fast path was removed on 2026-08-07 and the
             * two are equivalent again, but the wall clock is kept,
             * because it is what the hardware actually does and it stays
             * right whatever the driver above it decides.
             */
            if (dog_speed && keyboard_buffer_empty()) {
                ULONGLONG nowms = GetTickCount64();
                if (nowms - g_dog_last_ms >= NABU_WATCHDOG_MS) {
                    g_dog_last_ms = nowms;
                    keyboard_buffer_put(0x94);   /* kick the dog */
                }
            } else {
                g_dog_last_ms = GetTickCount64();
            }

            scanline++;

            /*
             * STEP THE VIDEO CHIP, EVEN THOUGH NOBODY IS LOOKING.
             *
             * This is the line this whole phase turned on, and the
             * assumption it cost was mine: that because the renderer
             * only reads pixels OUT of the VDP, a headless machine could
             * skip it. That is wrong.
             *
             * vrEmuTms9918ScanLine does not just produce pixels. It is
             * where the chip raises the flags in its own status
             * register: the frame flag (STATUS_INT) at the last visible
             * line, and the fifth-sprite and sprite-collision flags on
             * the way. The firmware's video interrupt handler reads that
             * register through port 0xA1, and with the frame flag never
             * set it concludes the interrupt was not the VDP's and does
             * nothing at all.
             *
             * The result was a machine that executed tens of millions of
             * t-states, banked its ROM out, lit its lamps and then sat
             * there: it sent 0x83, 0x82, 0x01 to the adapter, read the
             * eight bytes back, and never sent the 0x81 that comes next.
             * A byte trace of the working external emulator through a
             * logging proxy is what pinned it down, because both
             * machines received IDENTICAL bytes and only one of them
             * carried on.
             *
             * So the chip is run for real. The pixels go into a scratch
             * line and are discarded; Phase 2 will keep them instead,
             * which is the only difference video will make here.
             *
             * The 24 line offset and the 24..215 window are upstream's
             * and are the NTSC top border: the visible 192 lines of a
             * 262 line frame start at line 24.
             */
            if (scanline >= 24 && scanline < 216) {
                int y = scanline - 24;
                vrEmuTms9918ScanLine(vdp, (uint8_t)y,
                                     g_vdp_frame + (size_t)y * TMS9918_PIXELS_X);
            }

            if (scanline > 261) {
                scanline = 0;
                /* Register 7's low nibble is the backdrop, which is both
                 * the border and what shows through transparent pixels. */
                g_border_index = vrEmuTms9918RegValue(vdp, TMS_REG_7) & 0x0F;
                g_frames++;
                /* next_frame() went here. */
                if (vrEmuTms9918RegValue(vdp, TMS_REG_1) & 0x20) {
                    if (vdpint == 0) {
                        vdpint = 1;
                        update_interrupts();
                    }
                }
            }
            next += 228;
            done++;
        }
        z80_step(&cpu);
    }
}

void nabu_core_run_frame(void)
{
    nabu_core_run_scanlines(262);
}

/* ------------------------------------------------------------------ */
/* Observation                                                         */
/* ------------------------------------------------------------------ */

int      nabu_core_modem_up(void) { return gotmodem; }
uint64_t nabu_core_hcca_rx(void)  { return g_hcca_rx; }
uint64_t nabu_core_hcca_tx(void)  { return g_hcca_tx; }
uint64_t nabu_core_frames(void)   { return g_frames; }
unsigned long nabu_core_cycles(void) { return g_inited ? cpu.cyc : 0; }
int      nabu_core_ctrlreg(void)  { return ctrlreg; }

/* ------------------------------------------------------------------ */
/* The screen                                                          */
/* ------------------------------------------------------------------ */

/*
 * Convert the retained frame to 32-bit pixels for a Windows DIB.
 *
 * vrEmuTms9918Palette is RGBA, i.e. 0xRRGGBBAA. A 32-bit BI_RGB DIB
 * wants 0x00RRGGBB per pixel, so the conversion is one right shift of 8.
 * That is the same arithmetic Marduk's own renderer does, and doing it
 * here keeps the chip's palette next to the chip.
 *
 * Colour index 0 is TRANSPARENT, not black. On real hardware whatever is
 * behind the display shows through, which for a NABU is the backdrop in
 * register 7. Painting it as black would be wrong and visibly so: the
 * Greek Times front page uses a coloured backdrop, and a transparent-as-
 * black frame would show black bands where the page expects its own
 * background.
 *
 * The caller supplies the buffer, which must hold at least
 * NABU_SCREEN_W * NABU_SCREEN_H pixels. Returns the frame serial so a
 * caller can skip converting a frame it has already drawn.
 */
uint64_t nabu_core_copy_frame(uint32_t *dst)
{
    const uint8_t *src = g_vdp_frame;
    uint32_t backdrop;
    int i, n = TMS9918_PIXELS_X * TMS9918_PIXELS_Y;

    if (!dst) return g_frames;

    backdrop = vrEmuTms9918Palette[g_border_index & 0x0F] >> 8;
    if ((g_border_index & 0x0F) == 0) backdrop = 0;   /* transparent backdrop reads as black */

    for (i = 0; i < n; i++) {
        uint8_t c = src[i] & 0x0F;
        dst[i] = c ? (vrEmuTms9918Palette[c] >> 8) : backdrop;
    }
    return g_frames;
}

/* ------------------------------------------------------------------ */
/* Keyboard, HCCA wait state, and sound                                */
/* ------------------------------------------------------------------ */

void nabu_core_key_put(uint8_t code)
{
    if (!g_inited) return;
    keyboard_buffer_put(code);
}

int nabu_core_hcca_waiting(void)
{
    /*
     * "Waiting" means the guest has polled the HCCA and found nothing
     * many times in a row, AND there is still nothing there. The
     * threshold is deliberately not 1: a single empty read happens
     * constantly in the normal interrupt path and means nothing.
     *
     * The second half of that test is what makes the fast boot work, and
     * getting it wrong is what made the first attempt only 1.1x. While
     * bytes are sitting in the socket the guest is not waiting for
     * anything, it is CHEWING, and chewing is exactly what should be
     * allowed to run flat out.
     */
    return (gotmodem && g_hcca_dry > 64 && !modem_bytes_available());
}

int nabu_core_hcca_draining(void)
{
    /* Bytes have already arrived and the guest has not read them yet. */
    return (gotmodem && modem_bytes_available());
}

unsigned nabu_core_hcca_idle_ms(void)
{
    /*
     * How long since the guest last actually took a byte off the wire.
     *
     * Asking "is there data right now" turned out to be useless as a
     * throttle signal: it is sampled once a frame, and the guest empties
     * a burst in a fraction of a frame, so the answer was almost always
     * no even in the middle of a transfer. Asking how recently a byte
     * moved is stable across a whole transfer and is what the driver
     * uses to decide it may run fast.
     */
    if (!gotmodem || g_hcca_rx == 0) return 0xFFFFFFFFu;
    return (unsigned)(GetTickCount64() - g_hcca_last_rx_ms);
}

/* One PSG sample at the rate the chip was constructed with. Emulation
 * thread only, like everything else here. */
int16_t nabu_core_psg_sample(void)
{
    if (!g_inited || !psg) return 0;
    return (int16_t)PSG_calc(psg);
}

void nabu_core_vdp_regs(uint8_t *out8)
{
    int i;
    if (!out8) return;
    for (i = 0; i < 8; i++)
        out8[i] = vdp ? vrEmuTms9918RegValue(vdp, (vrEmuTms9918Register)i) : 0;
}

uint32_t nabu_core_border_rgb(void)
{
    uint8_t c = g_border_index & 0x0F;
    return c ? (vrEmuTms9918Palette[c] >> 8) : 0;
}

uint64_t nabu_core_port_reads(int port)
{
    if (port < 0 || port > 255) return 0;
    return g_port_reads[port];
}

void nabu_core_get_state(nabu_core_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!g_inited) return;
    out->pc          = cpu.pc;
    out->halted      = (uint8_t)cpu.halted;
    out->iff1        = (uint8_t)cpu.iff1;
    out->int_pending = (uint8_t)cpu.int_pending;
    out->imode       = cpu.interrupt_mode;
    out->int_mask    = psg_porta;
    out->int_lines   = interrupts;
    out->vdp_reg1    = vdp ? vrEmuTms9918RegValue(vdp, TMS_REG_1) : 0;
}
