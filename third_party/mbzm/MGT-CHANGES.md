# mbzm as vendored by the MGT Unicorn Suite

Upstream: https://github.com/roscopeco/mbzm
Upstream licence: MIT, Copyright (c) 2020 Ross Bamford. See `LICENSE`.

The Suite uses mbzm as the Zmodem RECEIVE core behind the RIPscrip tab's
file download. The Suite side of that (the session driver, the Win32
threading, the file naming policy and every security decision) lives in
`src/zmodem_recv.[ch]` and is not part of this directory.

## What was changed

### 1. The two GPL CRC files were removed and replaced

`crc16.c`, `crc32.c`, `include/crc16.h` and `include/crc32.h` came from
Synchronet's smblib under the GNU GPL v2 or later, not under mbzm's own
MIT licence. The Suite ships no copyleft code, so all four were deleted
and rewritten from the polynomial definitions.

The replacements are public domain, build their tables at run time, and
keep the same names, macros and semantics so the rest of the core
compiles unmodified. `licenses/LICENSE.mbzm-smblib-REMOVED.txt` records
what was taken out and why. Upstream's `LICENSE.smblib` is kept here
alongside it as the primary evidence.

One behavioural detail is worth recording because it is easy to get
wrong: the removed `crc32i()` ended with `return(~crc)`. The final
complement therefore belongs inside `crc32i()`, and the replacement does
it there. The binary32 HEADER path never calls `crc32i()` at all: it
drives the `ucrc32()` macro directly and applies its own `crc = ~crc`.
Only the data-subpacket path goes through the function. Omitting the
complement leaves headers working and every data subpacket failing CRC.

### 2. `rz.c` was dropped

Upstream's `rz.c` is an example driver: a POSIX `main()` that opens a
device file, writes whatever filename the sender asks for, and has no
size limits. The Suite has its own driver with the security policy in
it, so the example is not vendored.

## What was NOT changed

`zserial.c`, `zheaders.c`, `znumbers.c` and the remaining headers are
upstream byte for byte. The core declares but does not define `zm_recv()`
and `zm_send()`; `src/zmodem_recv.c` supplies both.

## Layering note

mbzm implements the INNER escape layer, ZDLE (0x18). The OUTER telnet
layer, IAC (0xFF) doubling, is handled entirely in `src/telnet_proto.c`,
below this core, in both directions. mbzm never sees a doubled 0xFF and
never has to produce one.
