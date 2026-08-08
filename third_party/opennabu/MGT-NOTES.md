# OpenNabu IPL as vendored by the MGT Unicorn Suite

Upstream: https://github.com/buricco/opennabuipl
Licence: **MIT**, Copyright 2012, 2013, 2015, 2023 S. V. Nickolas.
See `license.txt`, which is upstream's own and is reproduced unmodified.

`opennabu.bin` is the assembled 4096-byte boot ROM, taken from the copy
Marduk ships (the exact binary this port was proven against), not
rebuilt. Its sha256 is

    474a27535a63f048e14c994098a5151b5233cb3f3a8f45e32e43ba21686b80aa

`upstream-readme.txt` is upstream's `readme.txt`, kept because it
documents the boot order and the Esc override.

## Why this is here: it is the SHIPPABLE boot ROM

A NABU cannot boot without firmware, and the Suite has two choices.

**The stock NABU firmware** is preserved proprietary code from a company
dissolved in the mid 1980s. The Suite already ships it for the external
player and credits it as preserved firmware, but no licence grant
accompanies it and none can be obtained.

**OpenNabu IPL is MIT.** It is a clean replacement boot ROM, and the
native port boots the Montreal Greek Times channel end to end on it, with
no keyboard attached at all: nabud logged the image fetch, then
`Opening 'NEWS.TXT'` and `Opening 'WEATHER.TXT'`. See
`docs/2026-08-05_NABU_NATIVE_PHASE1.md`.

So the native NABU tab can ship a boot ROM the Suite is actually licensed
to redistribute, and the proprietary ROM becomes unnecessary for it.

## The stock ROM needs a keyboard, and that is not a port defect

The stock firmware does **not** boot headless, in this port or in
unmodified upstream Marduk. Both sit in the same loop polling the
keyboard strobe at PC 0x0426, because the stock ROM asks the user for a
channel number when the adapter reports the channel code is not set.
Verified by running both against the same live server for 150 seconds
each: neither reached the channel.

OpenNabu does not ask. That is the whole difference, and it is a firmware
behaviour, not an emulation one.

The stock ROM should boot once Phase 3 adds a keyboard. Until then, the
open firmware is the one that works, which is a happy coincidence of the
licensing answer and the engineering answer pointing the same way.

## NOT YET SHIPPED

This ROM is in the repository but is not in the installer or the exe.
Adding it, and adding its entry to `THIRD-PARTY-NOTICES.md`, belongs to
Phase 4 when the native tab actually ships. `THIRD-PARTY-NOTICES.md`
describes what the Suite redistributes today, and it would be wrong to
claim this before it does.
