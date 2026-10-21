# libx86emu in the MGT Unicorn Suite: changes to the vendored copy

Upstream: https://github.com/wfeldt/libx86emu, commit
`ce81129c57fdfbf690bebc210a8db97d926cc5e7` (2025-02-24). Vendored files:
`api.c`, `decode.c`, `mem.c`, `ops.c`, `ops2.c`, `prim_ops.c`,
`include/*.h`, `LICENSE`, `LICENSE_INFO`, `README.md`. Licence: X11/BSD
style, see `LICENSE`.

## The one change (2026-10-20), at two sites

Upstream assumes an x86 Linux host may do real port I/O and probe IOPL
(`<sys/io.h>`, `getiopl`). Neither exists on Windows, and the Suite never
wants either: every guest port access goes through the Suite's own memio
callback (`src/gwb_machine.c`) and never reaches host hardware.

- `mem.c`: the `<sys/io.h>` include is skipped on `_WIN32`, which selects
  upstream's own non-x86 fallback (dummy `inb`/`outb` macros that nothing
  reaches, because the Suite replaces the memio handler).
- `include/x86emu_int.h`: on `_WIN32`, `WITH_TSC` stays 1 and `WITH_IOPL`
  is 0.

Both sites are marked `MGT:` in the source. Nothing else is modified.
