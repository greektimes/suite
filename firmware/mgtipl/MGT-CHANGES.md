# MGT IPL: every change from upstream OpenNabu IPL

`mgtipl.a80` is a **modified copy** of OpenNabu IPL v0.6.5 by S. V. Nickolas
(buricco), used and redistributed under its own MIT licence, which is
reproduced at the head of the file and in `third_party/opennabuipl/license.txt`.

The unmodified source it was forked from is vendored at
`third_party/opennabuipl/opennabu.a80`, upstream commit
`822ef201df58b0140d1b4c3713db8642e6333471`. Diff the two and this file
explains what you see.

## Why fork at all

The NABU Native tab is an emulated NABU permanently tuned to one channel. It
has no floppy drive, no Winchester, usually no keyboard, and exactly one
thing to do. Upstream firmware is general purpose: it probes for drives,
tests memory, offers a boot menu and remembers the last device booted. All of
that is correct for a real NABU on a real desk and is pure waiting on this
one.

Measured against the live channel at true 1x on 2026-08-07, upstream spent
**10.1 seconds before its first byte on the wire**. This fork spends **0.37
seconds**. Full measurements are in
`docs/2026-08-07_NABU_MGT_IPL_FAST_BOOT.md`.

## What was REMOVED

| removed | upstream label | why | measured or estimated cost |
| --- | --- | --- | --- |
| Floppy controller probe and recalibrate-and-read-T0S1 | `getfd`, `fdseek`, `drvfail`, `fdcmplt`, `fdwait`, `fdret` | No floppy, and the emulated machine reports a controller with no disk in it, so the INDEX wait runs its full timeout every boot | **about 8 s, measured** |
| Winchester probe and read | `gethd`, `hdseek`, `hdset`, `hdwait`, `cleanup`, `cleanup1` | No hard disk | small when absent |
| Power-on RAM test and its report | `testram`, `testlo`, `testhi`, `ramderp`, `rsod`, `ramok`, `rammsg` | 64 KB tested a byte at a time on every boot, to tell an emulated machine that its emulated RAM works | **about 2 s, measured** |
| The half-second wait for `<Esc>` | `entry3`, `entry5` | It is an offer to open a boot menu this fork does not have | about 0.7 s |
| Boot-device menu | `menu`, `.confirm`, `bootmsg` | Three choices, two of them removed, and often no keyboard to answer with | none, but it is unreachable now |
| Warm-boot memory of the last device | `bootany`, `bootany1` to `bootany4`, `setwarm`, and the `iplmode` / `warmflag` cells | Only ever selected between devices that no longer exist | none |
| The startup bell | the `call bell` in `entry2` | A delay loop whose entire purpose is to last long enough to hear | about 0.06 s |
| The NABU logo in the corner | `wrbanner`, `addrtab`, `strings` | Branding for a different machine, drawn on a screen the channel is about to overwrite | small |

The `bell` routine itself is KEPT, because the TTY vector at `$0015` points
at it and printing a BEL still rings it. Only the unconditional call at boot
is gone. The logo GLYPHS are also kept, in the first 32 entries of the font,
because the font is uploaded to the VDP as one block and carving 32 glyphs
out of the middle of it would buy nothing.

## What was CHANGED

**The wait for the keyboard's power-on byte is now bounded.** In `keyreset`,
upstream spins until the keyboard says something:

    keyrest3: in a,(c) / and 0x02 / jr z,keyrest3

On a machine with no keyboard attached that is forever, and it is the one
hang this fork had to fix to be usable. The fork counts down about a tenth of
a second and carries on. The keyboard controller is still programmed with
upstream's own byte sequence, so a real NABU with a real keyboard is set up
identically.

**Boot failure retries instead of offering a menu.** `err` now prints one
line and jumps back to `tryhcca`. The thing that realistically fails here is
the wire, and a wire that is down for a moment comes back; a halt would need
a human to notice. One failed attempt costs one round trip, so this is not a
hammer on the channel server.

**Reset goes straight to the cable modem.** `entry2` does the ROM shadow
copy, `setup`, one line of identification, and `jp tryhcca`.

**Strings.** The banner, the boot menu text, `64K OK` and `RAM failure` are
gone. Two remain: `MGT IPL - LOADING GREEK TIMES`, printed once, and
`Channel load failed, retrying`.

**A trailing `ds 0x1000-$`.** Deleting the logo tables freed 48 bytes and
would otherwise have produced a 4048-byte image. The image must be a full
4 KB: the machine shadows 4096 bytes from ROM into RAM at reset, and anything
past the end of a short file would be shadowed from whatever an emulator or
an empty EPROM socket happens to return.

## What was NOT touched

**The adapter protocol, byte for byte.** `tryhcca`, `inithcca`, `rdhcca`,
`xrdhcca`, `wrhcca` and `rdhcca2` are upstream's, unmodified. The same
request bytes go out in the same order, so nabud neither knows nor cares
which firmware is talking to it. This was verified on the live channel: both
firmwares transmit exactly 203 bytes and receive exactly 29036 to reach the
loaded program.

**The load loop was NOT tightened**, although the dispatch that commissioned
this fork asked for it. The premise was that the loop cost about 1800 Z80
cycles per byte and that a leaner one would close most of the gap to the
authentic link speed. Measurement disproved it: the payload streams in at
roughly 130 cycles per byte, and what actually costs the time is three stalls
per packet of 116, 266 and 283 milliseconds waiting on the wire. Rewriting a
working protocol implementation to chase about 5 percent of the transfer was
not a trade worth making. The evidence is in the phase doc.

**The TTY, its vectors and its font.** `setup`, `clrscr`, `putch`, `putln`,
`getch`, `getln`, `scroll`, `htab`, `vtab`, `bell` and the vector table at
`$0003` to `$001B` are upstream's and stay where upstream put them, in case
the loaded program calls them.

**The VDP initialisation, including the 16 KB video memory flush.** That is
hardware setup, not delay, and it costs about 0.19 s.

## Building it

Debian `z80asm` 1.8, as upstream specifies. Not z88dk's assembler of the same
name.

    z80asm -o mgtipl.bin mgtipl.a80

The result is 4096 bytes, sha256

    d3b0bb614cdb180889a6551600990f8dcf3c4780d6ef5aad738eecd720a8910a
