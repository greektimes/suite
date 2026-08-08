/*
 * nabu_core.h - the NABU Personal Computer as a driveable machine.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The machine layer this drives is DERIVED FROM MARDUK, which is MIT.
 * See third_party/marduk/license.txt and third_party/marduk/MGT-CHANGES.md
 * for the copyright notices that travel with it.
 *
 * ------------------------------------------------------------------
 * WHAT THIS IS
 * ------------------------------------------------------------------
 *
 * Marduk is a console program: one main() that owns SDL, the window,
 * the audio device, the joystick, the throttle and the emulation loop,
 * all at once. The Suite cannot use that. It already owns a message
 * loop, a window and an audio path, and it needs the machine to be
 * something it can step from where it stands.
 *
 * So this is Marduk's machine, with its host torn off: the memory map,
 * the port map, the interrupt priority encoder, the keyboard buffer and
 * the scanline loop, driven through four calls.
 *
 *     nabu_core_init      load the ROM, build the chips, dial the modem
 *     nabu_core_run_frame run one frame's worth of machine
 *     nabu_core_reset     the F3 soft reset
 *     nabu_core_shutdown  hang up and free
 *
 * ------------------------------------------------------------------
 * WHAT IT DELIBERATELY DOES NOT DO, IN THIS PHASE
 * ------------------------------------------------------------------
 *
 * Nothing is drawn, nothing is heard, nothing is typed. The VDP is a
 * state machine fed by port writes and it keeps its state whether or not
 * anyone reads pixels out of it; the PSG likewise keeps its registers
 * whether or not anyone calls PSG_calc. The machine boots and talks to
 * the channel server with no display, no audio device and no input, and
 * that is the whole point of this phase.
 *
 * GDI video is Phase 2, input and audio and timing are Phase 3, and the
 * tab is Phase 4.
 *
 * ------------------------------------------------------------------
 * ONE MACHINE AT A TIME
 * ------------------------------------------------------------------
 *
 * Marduk's machine state is a page of file-scope globals. Rewriting all
 * of it into a struct would touch every line of the port and interrupt
 * code, which is the code least safe to touch and hardest to prove, so
 * the globals were kept and confined to nabu_core.c as statics instead.
 *
 * The consequence is real and worth stating: there can be ONE NABU per
 * process. That is exactly what one tab needs, and it is why the API
 * below takes no handle. If a second machine is ever wanted, this is the
 * refactor to do first, and it should be done against a working and
 * verified machine rather than during a port.
 */

#ifndef NABU_CORE_H
#define NABU_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The channel server the Suite's own NABU service answers on. The tab
 * will offer these as the editable Host and Port defaults; they are not
 * baked into the machine, which takes whatever it is given. */
#define NABU_CORE_DEFAULT_HOST  "nabu.greektimes.ca"
#define NABU_CORE_DEFAULT_PORT  5816
/* Channel 1 on the Greek Times server is the Montreal Greek Times, and
 * it is also what that server's connection is configured for, so the
 * default asks for exactly what it would have got anyway. */
#define NABU_CORE_DEFAULT_CHANNEL 1

typedef struct {
    const char *host;      /* NULL for NABU_CORE_DEFAULT_HOST */
    int         port;      /* 0    for NABU_CORE_DEFAULT_PORT */
    const char *rom_path;  /* required: a 4096 or 8192 byte NABU boot ROM */
    /*
     * The channel to select on the adapter, 1 to 256, or 0 to leave the
     * server on whatever its connection is configured for.
     *
     * This is the adapter's own notion of a channel, the one a real NABU
     * changes with the CHANGE_CHANNEL message, and NOT the pack number
     * inside a channel. On the Greek Times server, channel 1 is the
     * Montreal Greek Times and 11, 12, 13 and 19 are the NABU Network
     * cycles and HomeBrew.
     *
     * See nabu_core_select_channel in nabu_core.c for why the core sends
     * this itself instead of leaving it to the firmware.
     */
    int         channel;
} nabu_core_config_t;

/* Diagnostics. The vendored Marduk files print to a console the Suite
 * does not have, so every such call is routed to marduk_diag, and
 * marduk_diag hands the formatted line to whatever is registered here.
 * NULL discards. Set this BEFORE nabu_core_init to see connection
 * failures. */
typedef void (*nabu_diag_fn)(const char *line, void *user);
void nabu_core_set_diag(nabu_diag_fn fn, void *user);

/* HCCA byte trace. Set before nabu_core_init to watch the wire. dir is
 * 'W' for a byte the NABU wrote to the adapter and 'R' for one it read
 * back. This is how the adapter handshake is debugged: the NABU protocol
 * is a byte-at-a-time conversation, and staring at a lamp register tells
 * you nothing about which turn of it went wrong. NULL disables. */
typedef void (*nabu_hcca_trace_fn)(char dir, uint8_t byte, void *user);
void nabu_core_set_hcca_trace(nabu_hcca_trace_fn fn, void *user);

/* Every I/O port access, with the PC that made it. Formatted the same
 * way as the instrumented upstream Marduk so the two traces diff line
 * for line, which is how a divergence between them gets found. */
typedef void (*nabu_port_trace_fn)(char dir, uint8_t port, uint8_t val,
                                   uint16_t pc, void *user);
void nabu_core_set_port_trace(nabu_port_trace_fn fn, void *user);

/* Build the machine and connect. Returns 0 on success. On failure a
 * human-readable reason is written to err (if err is non-NULL) and
 * nothing is left allocated.
 *
 * A failure to reach the channel server is NOT fatal and does not fail
 * this call: a real NABU with no cable still powers on. The machine runs
 * with the HCCA idle, exactly as Marduk does, and nabu_core_modem_up
 * reports what happened. */
int  nabu_core_init(const nabu_core_config_t *cfg, char *err, size_t errcap);

/* Run one frame: 262 scanlines of Z80, VDP and interrupt servicing at
 * 228 cycles per scanline, which is the NTSC NABU's timing. This does
 * NOT sleep. The caller owns pacing, because in the Suite the caller is
 * a message loop that already has opinions about time. */
void nabu_core_run_frame(void);

/* Run a specific number of scanlines. run_frame is this with 262. */
void nabu_core_run_scanlines(int scanlines);

/* The F3 soft reset: re-init the CPU and re-arm the keyboard, keeping
 * the modem connection and the loaded ROM. */
void nabu_core_reset(void);

/* Hang up and free. Safe to call when init failed or was never called. */
void nabu_core_shutdown(void);

/* ---- the screen ---------------------------------------------------- */

/* The TMS9918's visible raster. */
#define NABU_SCREEN_W  256
#define NABU_SCREEN_H  192

/* Convert the most recent frame into dst as 0x00RRGGBB pixels, top row
 * first, NABU_SCREEN_W * NABU_SCREEN_H of them. Returns the frame serial.
 *
 * THREADING. The machine is single-threaded and has no locks of its own:
 * whichever thread calls nabu_core_run_* owns it. This function reads the
 * frame the machine writes, so call it from that SAME thread, between
 * run calls, and hand the result to the UI under whatever lock the UI
 * uses. Do not call it from a paint handler while another thread is
 * running the machine. src/nabu_native_module.c does it the right way and
 * is the reference. */
uint64_t nabu_core_copy_frame(uint32_t *dst);

/* The backdrop colour of the last completed frame, as 0x00RRGGBB. The
 * visible 256x192 is a window in a larger raster and software sets the
 * border deliberately, so letterboxing should use this rather than
 * assuming black. */
uint32_t nabu_core_border_rgb(void);

/* The VDP's eight registers. Registers 0 and 1 carry the mode bits, and
 * 2 to 6 the table base addresses, which is what to look at when the
 * picture is structured but wrong. */
void nabu_core_vdp_regs(uint8_t *out8);

/* ---- keyboard ------------------------------------------------------ */

/* Put one byte into the emulated keyboard's buffer. These are the NABU
 * keyboard's own codes, not ASCII for the special keys: the arrows and
 * the function keys send a MAKE byte on press and a BREAK byte on
 * release, and printable keys send their ASCII once. The mapping lives
 * in src/nabu_native_module.c, which is where Win32 messages arrive.
 *
 * Emulation thread only, like everything else in this header. A UI
 * thread must hand its keystrokes across rather than call this. */
void nabu_core_key_put(uint8_t code);

/* ---- sound --------------------------------------------------------- */

/* The rate the PSG is clocked to produce samples at, and the rate the
 * Suite opens its waveOut device with, so one PSG sample is one output
 * sample and nothing resamples. 44100 because that is what every device
 * on the machine already runs at natively. */
#define NABU_AUDIO_RATE  44100

/* One PSG sample, signed 16-bit, at NABU_AUDIO_RATE. There is exactly
 * one of these per output sample, so a driver asks for
 * NABU_AUDIO_RATE / 60 of them per emulated frame. Emulation thread
 * only. */
int16_t nabu_core_psg_sample(void);

/* ---- wire state, formerly pacing ----------------------------------- */

/*
 * THE THREE CALLS BELOW NO LONGER DRIVE ANY SPEED DECISION.
 *
 * They were built so a driver could run the Z80 faster than real time
 * whenever the firmware was not blocked on an empty wire, which made the
 * boot several times quicker. That mechanism was removed on 2026-08-07:
 * the guest program keeps its own wall clock on screen, so a machine
 * that runs fast tells the operator the wrong time. Every driver now
 * paces at exactly 1x from the first frame, unconditionally, and asks
 * nothing here to decide it. See
 * docs/2026-08-07_NABU_NATIVE_1X_CLOCK.md.
 *
 * They are kept because they are honest observations of the wire and the
 * headless harnesses report them. Nothing in the Suite branches on them.
 */

/* Nonzero while the guest is spinning on the HCCA waiting for bytes that
 * have not arrived: many empty reads in a row, and still nothing there. */
int nabu_core_hcca_waiting(void);

/* Nonzero while bytes have arrived on the wire that the guest has not
 * read yet. Useful when reading a transfer trace: the image transfer is
 * bound by the GUEST'S OWN cost per byte, about 1800 Z80 cycles, which
 * at the authentic clock is roughly 2000 bytes a second, so a 25
 * kilobyte boot image takes about 25 seconds and this reads nonzero for
 * most of it. */
int nabu_core_hcca_draining(void);

/* Milliseconds of WALL CLOCK since the guest last read a byte off the
 * wire, or a very large number if it never has. Stable across a whole
 * transfer, unlike "is there data right now", which the guest empties in
 * a fraction of a frame and so reads as idle even mid-transfer. */
unsigned nabu_core_hcca_idle_ms(void);

/* ---- observation, for the headless proof and for later phases ------ */

int      nabu_core_modem_up(void);      /* nonzero if the HCCA is connected */
uint64_t nabu_core_hcca_rx(void);       /* bytes the NABU read from port 0x80 */
uint64_t nabu_core_hcca_tx(void);       /* bytes the NABU wrote to port 0x80 */
uint64_t nabu_core_frames(void);        /* frames run since init */
unsigned long nabu_core_cycles(void);   /* Z80 t-states since init */

/* The NABU's control register. Bit 0 banks the ROM out, which the boot
 * ROM does once it is running from RAM, so a nonzero read of bit 0 is
 * the cheapest evidence that the machine really executed. Bits 3, 4 and
 * 5 are the green CHECK, red ALERT and yellow PAUSE lamps on the front
 * of the machine. */
int      nabu_core_ctrlreg(void);

/* A snapshot of what the CPU is actually doing. This is here because a
 * stalled machine looks identical from the outside whether it is
 * polling, halted, or drowning in interrupts, and those want completely
 * different fixes. */
typedef struct {
    uint16_t pc;
    uint8_t  halted;       /* executed HALT and is waiting for an int  */
    uint8_t  iff1;         /* interrupts enabled                       */
    uint8_t  int_pending;
    uint8_t  imode;
    uint8_t  int_mask;     /* PSG port A: which interrupts are enabled */
    uint8_t  int_lines;    /* the four interrupt lines, live           */
    uint8_t  vdp_reg1;     /* bit 6 display enable, bit 5 int enable   */
} nabu_core_state_t;

void nabu_core_get_state(nabu_core_state_t *out);

/* How many times the guest has read a given I/O port since init. */
uint64_t nabu_core_port_reads(int port);

/* Send the adapter a CHANGE_CHANNEL message, the same one a real NABU
 * sends, and wait briefly for it to confirm. Called for you by
 * nabu_core_init when the config asks for a channel; exposed so a
 * harness can drive it. Failure is not an error: the connection simply
 * stays on the server's own default channel. */
void nabu_core_select_channel(int channel);

/* The emulated keyboard's watchdog: the real keyboard sends 0x94 about
 * every 3.7 seconds and the firmware sulks if it stops. Expressed in
 * scanlines; 0 disables it. Call before nabu_core_init. Exists so the
 * watchdog can be taken out of the picture while diagnosing a stall. */
void nabu_core_set_watchdog(unsigned scanlines);

#ifdef __cplusplus
}
#endif

#endif /* NABU_CORE_H */
