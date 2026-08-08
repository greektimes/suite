# Marduk as vendored by the MGT Unicorn Suite

Upstream: https://github.com/buricco/marduk
Upstream commit: `bd99967c2aded5f8a5fe25ac3bbb595a71b0dfea` (2026-03-03)
Upstream licence: MIT. See `license.txt`, which is upstream's own and is
reproduced unmodified.

Marduk is an emulator for the NABU Personal Computer. The Suite uses its
emulation core to run a NABU inside the Suite instead of launching a
separate emulator process. The Suite side of that (the machine driver,
the frame loop, the lifecycle and every Win32 decision) lives in
`src/nabu_core.[ch]` and is not part of this directory.

## Which base was chosen, and why

Four candidates were compared on 2026-08-05.

| repository | last push | verdict |
| --- | --- | --- |
| `buricco/marduk` | 2026-03-03 | **CHOSEN.** Most recent and most complete. |
| `visrealm/marduk` | 2026-01-07 | Same sources plus CMake and CI. Adds build tooling the Suite does not use. |
| `zdebel/marduk` | 2023-02-27 | Behind upstream: predates `disk.c` entirely. |
| `gtampdotcom/marduk` | 2024-05-19 | Not a fork of the emulator at all, a PSP port wrapper. |

The dispatch mentioned "an updated fork by Licca and zDebel". Those are
not a fork: they are the upstream AUTHORS. `license.txt` names S. V.
Nickolas and Marcin Woloszczuk, and `zdebel` is Woloszczuk's GitHub
account, whose own fork is three years behind the repository it forked
from. Upstream is the more complete and better maintained base by every
measure checked, so upstream is what was vendored.

## Licence confirmation

Every vendored file was checked individually. All carry the same MIT
grant in their own header, and all four copyright holders are covered by
`license.txt`:

| file | copyright | licence |
| --- | --- | --- |
| `z80.c`, `z80.h` | (c) 2019 Nicolas Allemand | MIT |
| `tms9918.c`, `tms9918.h` | (c) 2021 Troy Schrapel | MIT |
| `tms_util.c`, `tms_util.h` | (c) 2022 Troy Schrapel | MIT |
| `emu2149.c`, `emu2149.h` | (c) 2001-2022 Mitsutaka Okazaki | MIT |
| `disk.c`, `disk.h` | 2022, 2023 S. V. Nickolas; 2023 Marcin Woloszczuk | MIT |
| `modem.c`, `modem.h` | 2022, 2023 S. V. Nickolas; 2023 Marcin Woloszczuk | MIT |

Nothing copyleft was vendored and nothing of unclear provenance was
vendored.

**NOT vendored here:** `main.c` (the SDL host program, which is what the
Suite replaces), `modem_dummy.c`, `dasm80.c` (a debug disassembler),
`paths.h` and the Makefiles.

`opennabu.bin` **is** now vendored, but as its own project rather than as
part of Marduk, at `third_party/opennabu/`. It is OpenNabu IPL, MIT,
Copyright 2012-2023 S. V. Nickolas, from
https://github.com/buricco/opennabuipl . The licence was established
after the first pass of this file said it had not been. It matters more
than it looks: it is the boot ROM the native port actually boots the
channel on, and it is the one the Suite is licensed to redistribute.

A warning that cost a session. Marduk's `opennabu.bin` and the copy in
the bundled MAME package are DIFFERENT BUILDS with the same name and the
same 4096-byte size:

    474a27535a63f048...  Marduk's, boots the channel
    44ee0854bb85896a...  the MAME package's, wedges early

The binary vendored at `third_party/opennabu/` is Marduk's, the one this
port was proven against.

## What was changed

`modem.c` and `tms9918.c` were modified. Everything else is byte-for-byte upstream.
`marduk_diag.h` is new and is not upstream at all.

### 1. The socket handle was a 32-bit `int` on 64-bit Windows

Upstream declares `static int mosock`. On Win64 `SOCKET` is `UINT_PTR`,
64 bits wide, and `INVALID_SOCKET` is `(SOCKET)(~0)`. Storing a socket in
an `int` truncates the handle, and the `mosock==INVALID_SOCKET` test then
compares a truncated value against `-1`.

Upstream's own header comment says "the Windows code has not been tested
at all", and this is one of the reasons it could not have worked as
written. Replaced with a `marduk_socket_t` typedef that is `SOCKET` on
Windows and `int` elsewhere.

### 2. Console output was routed to a diagnostic hook

Upstream reports with `printf`, `fprintf(stderr, ...)` and `perror`. The
Suite is built `-mwindows` and has no console, so all of that would
vanish silently, taking the only diagnosis of a failed connection with
it. Every such call now goes through `marduk_diag`, declared in the new
`marduk_diag.h` and implemented in `src/nabu_core.c`.

The `perror` calls were doing worse than nothing on Windows in any case:
Winsock does not set `errno`, so a failed connect printed "Connection to
virtual modem failed: No error". Observed, not theorised, while comparing
against upstream. The replacements report `WSAGetLastError`.

### 3. `modem_deinit` did not clear `status`

Upstream only tears the modem down on the way out of `main()`, so a stale
`status` never mattered. The Suite connects and disconnects repeatedly
within one process, and a stale `status` of 1 means the next session
reads and writes a closed socket. Now cleared.

### 4. The connect result test was made explicit

`if (e==-1)` became `if (e!=0)`. Windows returns `SOCKET_ERROR`, which
happens to be `-1`, so this was not a bug; it is now not an accident
either.

### 5. TCP_NODELAY on the modem socket (2026-08-07)

**This is a MODERN NETWORK ACCOMMODATION, not a wire change.** It is the
kind of thing a historian should be able to tell from original code at a
glance, so it is called out here as well as in a long comment at the
change itself.

`modem_init` now calls `setsockopt(mosock, IPPROTO_TCP, TCP_NODELAY, ...)`
straight after `connect` succeeds. `<netinet/tcp.h>` was added to the
non-Windows include block; Winsock already declares both constants.

**It does not change one byte of what goes on the wire, or the order of
them.** The NABU's HCCA is a serial line to an adapter on the end of a
cable. It has no notion of packets, and neither the emulated NABU nor
nabud can tell whether the bytes of a request crossed the internet in one
TCP segment or four. Only the timing changes, and it changes back TOWARDS
the hardware: a real NABU's bytes were never held back waiting for an
acknowledgement from anything.

Why it was needed. `modem_write` sends one byte per `send()`, which is not
a defect but the shape of the emulation: the Z80 executes `OUT ($80),A`
and one byte is all the machine has to give. Nagle's algorithm holds a
small write while an earlier small write is unacknowledged, so where the
firmware writes a run of bytes with no read between them (the four-byte
pack number, the two-byte acknowledgement) only the first left
immediately. The server cannot answer until it has the whole request, so
it did not, and its delayed acknowledgement timer ran out instead.

Measured on the live channel at true 1x. Before: three stalls per packet
of about 116, 266 and 283 ms, around a payload that arrived in about 10
ms. After: three stalls of 116.2, 116.4 and 116.5 ms, which is the
measured ICMP round trip to the channel server and therefore the floor.
Boot to the loaded program fell from 22.1 s to 11.5 s, with the transmit
and receive byte counts identical (203 and 29036). See
`docs/2026-08-07_NABU_NODELAY.md`.

Nothing else in the transport was touched. In particular the one-byte
`send()` was NOT coalesced into larger writes, because there is nowhere to
do it honestly: `modem_write` is called from the Z80's `OUT` handler with
one byte and no knowledge of where a logical message ends, so coalescing
would mean buffering across guest writes and guessing boundaries, and
guessing wrong would hold the last byte of a request forever.

## tms9918.c: Graphics II address masking (2026-08-05)

Upstream refused to draw the Montreal Greek Times NABU application
correctly, and so did this port until it was fixed, because upstream's
Graphics II renderer treats anything but the textbook register values as
a broken configuration:

```c
bool invalidGfxII = (registers[TMS_REG_4] & 0x03) != 0x03 ||
                    (registers[TMS_REG_3] & 0x7f) != 0x7f;
if (invalidGfxII) { pageOffset = 0; }
...
if (invalidGfxII) { pattern &= 0x07; }
```

With `invalidGfxII` set it drops the three-way page split and masks every
name down to three bits, so the WHOLE SCREEN is drawn from eight
patterns. The result is blocky two-colour noise, which is exactly what
both upstream and this port produced.

Those register bits are **address masks**, not a validity flag. A program
may legitimately mask the pattern and colour tables so that the three
thirds of the screen share one 2K block, and the Greek Times application
does exactly that with `R4 = 0x00` and `R3 = 0x9F`.

Replaced with the datasheet behaviour:

    pattern base = (R4 & 0x04) << 11   mask = ((R4 & 0x03) << 11) | 0x7FF
    colour  base = (R3 & 0x80) << 6    mask = ((R3 & 0x7F) << 6)  | 0x3F

masking the offset `(third << 11) | (name << 3) | row`, with the final
VRAM index wrapped to 0x3FFF. Verified against the live channel: the
front page renders correctly, roundel and all. See
`docs/2026-08-05_NABU_NATIVE_PHASE2.md`.

The vendored snapshot of this chip core is from 2021 and its author has
released many versions since, so the durable fix is probably to update
the core rather than carry this patch forever. That is a Phase 3 or 4
decision. `tms9918.c.bak-20260805-pre-gfx2mask` is the untouched
original.

## What was NOT changed, and one thing that was tried and reverted

`hccatint`, the HCCA "transmit buffer empty" interrupt, is held
permanently high by upstream on the grounds that a socket is always
ready. That was a prime suspect for the stall described in
`docs/2026-08-05_NABU_NATIVE_PHASE1.md`, since a permanently asserted
interrupt through a priority encoder should storm as soon as the
firmware unmasks it.

It was implemented properly (clear on write, raise on the next scanline,
which is what the real shift register does) and it changed **nothing**:
the same three bytes out, the same eight back, the same stall in the same
place. **Disproven.** Upstream's behaviour was restored, and the note is
left in `src/nabu_core.c` so nobody spends the afternoon on it again.
