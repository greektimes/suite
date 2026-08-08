# OpenNabu IPL source, as vendored by the MGT Unicorn Suite

Upstream: https://github.com/buricco/opennabuipl
Licence: **MIT**, Copyright 2012, 2013, 2015, 2023 S. V. Nickolas.
See `license.txt`, which is upstream's own and is reproduced unmodified.

This directory is a **pristine, unmodified** copy of the upstream source, at
the commit recorded in `UPSTREAM-COMMIT.txt`. Nothing here is edited. The
Suite's own modified copy is a separate file, `firmware/mgtipl/mgtipl.a80`,
and every difference between the two is listed in
`firmware/mgtipl/MGT-CHANGES.md`.

Keeping the two apart is the point. A historian, or a future session, can
diff them and see exactly what the Suite changed and what it inherited.

## Building it

Upstream's `Makefile` calls the `z80asm` from the Debian repositories,
version 1.8. That is NOT the same assembler as z88dk's `z88dk-z80asm`; the
dialects differ and the source does not assemble under z88dk unmodified. The
Suite assembles both this and its fork on the Unicorn server, which is
Debian, with:

    z80asm -o opennabu.bin opennabu.a80

Assembling the vendored source with Debian z80asm 1.8 produces a 4096-byte
image with sha256

    44ee0854bb85896afbf056503e7a19efff250999f0885f227836e9bb9fa2fedd

## Two important facts about the binaries, established 2026-08-07

**This source is not where `third_party/opennabu/opennabu.bin` came from.**
That file, sha256 `474a2753...`, is the ROM the vendored Marduk emulator
ships and the one the native port was proven against. It is a different and
older OpenNabu build: its strings are `OPENNABU` and `BOOT ERROR`, where
every build from this repository says `OpenNabu IPL v0.6.5` and carries a
boot menu. All four commits in the upstream history were assembled and
compared; none of them produces `474a2753`, and each differs from it in more
than two thirds of its bytes. So the working ROM the Suite has been shipping
has no source in this repository, which is precisely why the Suite now
builds its own firmware from source it holds.

**The build of this source does NOT wedge the native port.** `CLAUDE.md`
records that the same-named ROM in the bundled MAME package, sha256
`44ee0854`, is a different build that wedges. That was true when it was
written and is not true now: `44ee0854` is exactly what this source
assembles to, and on 2026-08-07 it booted the live Greek Times channel end
to end in the current port, reaching the loaded program in about 31 seconds.
The wedge was real but was a property of the port at the time, not of the
firmware: this build waits for the keyboard's power-on byte, and the port
had no keyboard until Phase 3 added one. See
`docs/2026-08-07_NABU_MGT_IPL_FAST_BOOT.md`.
