/*
 * gwb_machine.h - a small IBM PC that runs Microsoft's GW-BASIC, in process.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The CPU is libx86emu (third_party/libx86emu, X11/BSD-style licence).
 * The program it runs is GWBASICA.EXE, built from Microsoft's 1983
 * GW-BASIC source plus T.K. Chia's OEM modules, both MIT, by
 * firmware/gwbasic/mgt_build.sh. Nothing here is derived from either; this
 * file only provides the machine they need.
 *
 * ------------------------------------------------------------------
 * WHAT THIS IS
 * ------------------------------------------------------------------
 *
 * Just enough PC for one DOS program: 1 MB of memory, an 8088 (libx86emu
 * runs a 386 superset; real-mode 8088 code cannot tell), a CGA adapter
 * whose memory at B800:0 is the screen, a BIOS and the parts of MS-DOS
 * GW-BASIC calls, all written in C. There is no BIOS ROM image and no
 * DOS kernel. Every interrupt vector starts out pointing at a 4-byte stub
 * at F000:(4 * n); when the CPU arrives at a stub, the service is done in
 * C and the stub's IRET returns. Because the vectors are real, GW-BASIC can
 * hook INT 1Bh, 1Ch, 23h and 24h exactly as it does on a PC and chain on
 * to the stubs, and both paths reach the same C.
 *
 * Files live in one host folder, which is drive C:'s root as far as the
 * guest can tell. Nothing outside it is reachable: paths are resolved
 * against that root and ".." stops there.
 *
 * ------------------------------------------------------------------
 * TIME
 * ------------------------------------------------------------------
 *
 * The machine runs in timer ticks, 18.2065 a second as on a PC. One call
 * to gwb_run_tick covers GWB_IPS / 18.2065 instructions of guest time,
 * and IRQ0 fires as PIT channel 0 says (once a tick at the BIOS default,
 * 400 times a second while GW-BASIC plays music), so guest time is counted
 * in instructions, not in host time. A caller that wants real time calls it 18.2 times a
 * second (the tab does); a test harness may call it as fast as it likes and
 * still read guest seconds as ticks / 18.2065. When the guest is waiting
 * for a key the tick ends early and costs nothing.
 *
 * ------------------------------------------------------------------
 * THREADS
 * ------------------------------------------------------------------
 *
 * One thread owns the machine and calls everything here except the three
 * input calls, which may come from any thread: gwb_push_key,
 * gwb_set_shift_flags and gwb_ctrl_break.
 */

#ifndef GWB_MACHINE_H
#define GWB_MACHINE_H

#include <stddef.h>
#include <stdint.h>

/* Guest instructions per second. An 8088 at 4.77 MHz averages roughly
 * 15 clocks per instruction once bus waits are counted, which is about
 * 0.33 million a second. This is the one knob that makes a FOR-NEXT delay
 * loop in a printed listing take as long as it did in 1983. */
#define GWB_IPS 330000u

#define GWB_TICKS_PER_SEC 18.2065

/* PC speaker output, mono 16-bit PCM, see gwb_audio */
#define GWB_AUDIO_RATE 44100

#define GWB_VRAM_BYTES 0x4000           /* CGA: 16 KB at B800:0 */

/* gwb_run_tick results (bits) */
#define GWB_TICK_IDLE   1u              /* guest is waiting for a key */
#define GWB_TICK_EXITED 2u              /* guest ended (SYSTEM) */

/* BIOS shift flags, for gwb_set_shift_flags (0040:0017) */
#define GWB_KF_RSHIFT  0x01
#define GWB_KF_LSHIFT  0x02
#define GWB_KF_CTRL    0x04
#define GWB_KF_ALT     0x08
#define GWB_KF_SCROLL  0x10
#define GWB_KF_NUM     0x20
#define GWB_KF_CAPS    0x40
#define GWB_KF_INSERT  0x80

typedef struct gwb_machine gwb_machine;

/* What a renderer needs, copied out by gwb_snapshot. */
typedef struct gwb_screen {
    uint8_t  vram[GWB_VRAM_BYTES];
    uint8_t  mode;                      /* BIOS video mode, 0..6 */
    uint8_t  cols;                      /* 40 or 80 in text modes */
    uint16_t page_start;                /* byte offset of the shown page */
    uint8_t  cur_row, cur_col;
    uint8_t  cur_start, cur_end;        /* CGA scan lines 0..7; start & 0x20 = hidden */
    uint8_t  cga_color;                 /* port 3D9h */
    uint8_t  cga_mode;                  /* port 3D8h */
    uint8_t  speaker_on;                /* PIT channel 2 gated to the speaker */
    uint16_t speaker_divisor;           /* 1193182 / divisor = Hz */
} gwb_screen;

/* Build the machine and load the EXE image (copied). root_dir is the host
 * folder that becomes C:\ and must already exist. Returns NULL and writes
 * a reason into err on failure. */
gwb_machine *gwb_create(const void *exe, size_t exe_len,
                        const wchar_t *root_dir,
                        char *err, size_t err_len);
void         gwb_destroy(gwb_machine *m);

/* Run one timer tick of guest time. Returns GWB_TICK_* bits. */
unsigned     gwb_run_tick(gwb_machine *m);

/* Keyboard. key is the BIOS word: scan code in the high byte, ASCII (or 0
 * for an extended key) in the low byte. Thread-safe. */
void         gwb_push_key(gwb_machine *m, uint16_t key);
void         gwb_set_shift_flags(gwb_machine *m, uint8_t flags);
/* Free places in the typed-ahead queue (for paste feeders). */
int          gwb_key_room(gwb_machine *m);
void         gwb_ctrl_break(gwb_machine *m);

void         gwb_snapshot(gwb_machine *m, gwb_screen *out);

/* The PC speaker during the last gwb_run_tick, as GWB_AUDIO_RATE mono
 * 16-bit PCM (about 2,422 samples a tick): a square wave at PIT channel
 * 2's frequency while port 61h gates it on, silence otherwise. Returns the
 * sample count; *pcm stays valid until the next gwb_run_tick. BEEP, SOUND
 * and PLAY all come out here. */
int          gwb_audio(gwb_machine *m, const int16_t **pcm);

/* Guest time so far, in ticks, and guest instructions executed. */
uint64_t     gwb_ticks(gwb_machine *m);
uint64_t     gwb_instructions(gwb_machine *m);

/* Called with one line for every BIOS or DOS service the machine does not
 * implement, and for other notable events. Optional. */
void         gwb_set_log(gwb_machine *m, void (*fn)(void *ctx, const char *line),
                         void *ctx);

/* Render the text page of a snapshot as 25 lines of plain text (CP437
 * bytes, controls shown as '.'), for logs and tests. buf must hold
 * 25 * 81 + 1 bytes. Returns 0 if the snapshot is not in a text mode. */
int          gwb_screen_text(const gwb_screen *s, char *buf);

#endif /* GWB_MACHINE_H */
