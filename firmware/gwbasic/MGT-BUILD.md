# GW-BASIC in the MGT Unicorn Suite: how this tree is used

This directory is T. K. Chia's GW-BASIC tree, vendored **unmodified** from
https://codeberg.org/tkchia/GW-BASIC at commit
`1432e385776554e43784b411eda7b8b2a9709e73` (2026-10-05), every tracked file
via `git archive`. It is Microsoft's 1983 GW-BASIC source (MIT, see
`LICENSE`) plus OEM modules by D. Spinellis, S. Gros and T. K. Chia (MIT).

The only files added here are this one, `mgt_build.sh` and `.gitignore`.

## Build

    C:\msys64\usr\bin\bash -l firmware/gwbasic/mgt_build.sh

Run it under **MSYS2's POSIX shell**, not UCRT64 and not Git Bash. JWlink
(Open Watcom-derived) builds only as a Unix program. It needs the MSYS2
packages `gcc`, `make` and `gawk` (`pacman -S gcc make gawk`). The script:

- builds upstream's bundled JWasm and JWlink with `-std=gnu17` (GCC 15 and
  later default to C23, where `bool` is a keyword those sources predate),
  through a compiler wrapper rather than by editing their sources;
- runs upstream's own Makefile with `OEMVER=2026-10-05` pinned. Unpinned,
  the Makefile stamps today's date whenever it cannot prove a clean git
  tree, which changes two bytes of each binary;
- **checks both results against upstream release 20261005**
  (`GWB26A05.ZIP`) and fails if either differs.

| file | bytes | sha256 |
| --- | --- | --- |
| `GWBASICA.EXE` | 51,760 | `1a7b92b1aaf893e7f040190b8fa728e3c5b4dd82caaded8fefd0a471f669f7f7` |
| `GWBASIC.EXE` | 51,296 | `ceb0c57c1c31981844a2e41b375cd14f77630e3e200419fe746b501d21dcf333` |

Upstream's assembly reports 1 warning (SCNDRV) and 0 errors; that is
upstream's code and not part of the Suite's warning baseline.

## Use

The Suite embeds **`GWBASICA.EXE`** (resource 400 in `src/suite.rc`) and
runs it on its own x86 machine (`src/gwb_machine.c` over libx86emu). It
is never written to disk. GWBASICA rather than GWBASIC because only the
"A" build has `TIMER`; in GWBASIC.EXE `TIMER` is an ordinary variable
equal to 0, so `RANDOMIZE TIMER` would seed identically on every run
(verified 2026-10-20, `test/gwbasic/timer_item3.txt`).

See `docs/GWBASIC_PHASE_A_2026-10-20.md`.
