#!/bin/bash
# mgt_build.sh - rebuild GWBASIC.EXE and GWBASICA.EXE from this vendored tree.
#
# Part of the Montreal Greek Times Unicorn Suite. The interpreter source is
# Microsoft's (MIT) plus T.K. Chia's OEM modules (MIT); see LICENSE and
# MGT-BUILD.md. This script only drives upstream's own Makefile.
#
# Run it under MSYS2's POSIX shell, NOT UCRT64 and NOT Git Bash, because
# JWlink only builds as a Unix program:
#
#     C:\msys64\usr\bin\bash -l firmware/gwbasic/mgt_build.sh
#
# Needs the MSYS2 package "gcc" (pacman -S gcc), plus make and awk.
#
# OEMVER is pinned to the upstream commit date so the result is byte for
# byte the upstream 20261005 release; the Makefile would otherwise stamp
# today's date whenever it cannot prove the tree is a clean git checkout.

set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# GCC 15 and later default to C23, where 'bool' is a keyword; JWasm and
# JWlink predate that. Wrap the compiler rather than editing their sources.
WRAP="$HERE/.mgt_ccwrap"
mkdir -p "$WRAP"
for n in gcc cc; do
    printf '#!/bin/sh\nexec /usr/bin/gcc.exe -std=gnu17 -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration -Wno-error=int-conversion "$@"\n' > "$WRAP/$n"
    chmod +x "$WRAP/$n"
done
export PATH="$WRAP:/usr/bin:/bin"

make OEMVER=2026-10-05 GWBASIC.EXE GWBASICA.EXE

# Upstream release 20261005 (GWB26A05.ZIP).
expect_a=1a7b92b1aaf893e7f040190b8fa728e3c5b4dd82caaded8fefd0a471f669f7f7
expect_b=ceb0c57c1c31981844a2e41b375cd14f77630e3e200419fe746b501d21dcf333
got_a=$(sha256sum GWBASICA.EXE | cut -d' ' -f1)
got_b=$(sha256sum GWBASIC.EXE | cut -d' ' -f1)
echo "GWBASICA.EXE $got_a"
echo "GWBASIC.EXE  $got_b"
if [ "$got_a" = "$expect_a" ] && [ "$got_b" = "$expect_b" ]; then
    echo "MATCHES upstream release 20261005"
else
    echo "DIFFERS from upstream release 20261005" >&2
    exit 1
fi
