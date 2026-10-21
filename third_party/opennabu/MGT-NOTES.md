# OpenNabu IPL as vendored by the MGT Unicorn Suite

Upstream: https://github.com/buricco/opennabuipl
Licence: **MIT**, Copyright 2012, 2013, 2015, 2023 S. V. Nickolas.
See `license.txt`, which is upstream's own and is reproduced unmodified.

`opennabu.bin` is the assembled 4096-byte boot ROM the installer ships as
`NABU\opennabu.bin`, the NABU tab's FALLBACK when MGT IPL (the primary,
built from `firmware/mgtipl/`) is missing.

## Since 0.7.0-beta (2026-10-20): built from tracked source

The ROM here is now ASSEMBLED FROM THE SOURCE THIS REPOSITORY HOLDS:
`third_party/opennabuipl/opennabu.a80` (upstream commit `822ef201`, source
sha256 `bc91e62a...615f5`), with Debian `z80asm` 1.8 on the Unicorn server,
exactly as `third_party/opennabuipl/Makefile` says:

    z80asm -o opennabu.bin opennabu.a80

    opennabu.bin  44ee0854bb85896afbf056503e7a19efff250999f0885f227836e9bb9fa2fedd

That hash was matched at both ends (on Unicorn after assembly, and here
after the copy). It is the build CLAUDE.md records as booting the live
channel end to end since the port gained a keyboard (2026-08-07).

**What it replaced.** Until 0.6.0 this file was `474a2753...`, the ROM
Marduk ships, which the native port was first proven against. It is an
older OpenNabu build ("OPENNABU", "BOOT ERROR") that NO commit of the
upstream repository produces, so no source for it existed anywhere: an
MIT notice could travel with it, but the Suite could not offer its
source. It is kept OUTSIDE the shipped set as
`opennabu.bin.bak-20261020-474a2753-sourceless` (ignored by git) in case a
comparison is ever needed. See `docs/RELEASE_070_2026-10-20.md`, A5.

`upstream-readme.txt` is upstream's `readme.txt`, kept because it
documents the boot order and the Esc override.

## Why an open boot ROM at all

A NABU cannot boot without firmware. The stock NABU firmware is preserved
proprietary code from a company dissolved in the mid 1980s, and no licence
grant accompanies it. OpenNabu IPL is MIT, a clean replacement the Suite is
licensed to redistribute, and the native NABU tab boots the Montreal Greek
Times channel on it. MGT IPL, the Suite's own fork of it, is the primary
because it skips the floppy, hard disk and memory checks a real NABU does
not need here.
