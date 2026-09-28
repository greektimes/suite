#!/bin/bash
# wg1_run.sh - gate W-G1 end to end.
#
# Part of the MGT Unicorn Suite. AGPLv3 or later, see /COPYING.
#
#   bash web/minitel/test/wg1_run.sh FRAMES_DIR EVIDENCE_DIR
#
# 1. builds the NATIVE Suite harness (tools/minitel_selftest.c, MSYS2 gcc)
#    into EVIDENCE_DIR, runs its G1 self-tests, and has it dump and render
#    every frame (page memory; BMPs colour and mono at 1x, and at 2x with
#    scan lines);
# 2. runs web/minitel/test/wg1_parity.mjs, which loads the SHIPPED
#    web/minitel/dist/engine.js + engine.wasm under Node and compares.
# Run web/minitel/build.bat first. Nothing is written inside the repo.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
frames="$1"; ev="$2"
mkdir -p "$ev/native-dump" "$ev/native-bmp" "$ev/web-dump"
gcc="${GCC:-/c/msys64/ucrt64/bin/gcc}"

"$gcc" -O2 -Wall -Wextra -I"$root/src" -o "$ev/minitel_selftest_native.exe" \
    "$root/tools/minitel_selftest.c" "$root/src/minitel_decode.c" \
    "$root/src/minitel_screen.c" "$root/src/minitel_font_data.c" \
    "$root/src/naplps_font_data.c" || exit 1
H="$ev/minitel_selftest_native.exe"

"$H" | tr -d '\r' > "$ev/g1-native.txt"
tail -1 "$ev/g1-native.txt"

for f in "$frames"/*.VDT "$frames"/*.vdt; do
    [ -e "$f" ] || continue
    n="$(basename "$f")"; n="${n%.*}"
    "$H" --dump "$f" | tr -d '\r' > "$ev/native-dump/$n.txt"
    "$H" "$f" "$ev/native-bmp/$n.c1.bmp" > /dev/null
    "$H" "$f" "$ev/native-bmp/$n.m1.bmp" --mono > /dev/null
    "$H" "$f" "$ev/native-bmp/$n.c2s.bmp" --scale 2 --scanlines > /dev/null
    "$H" "$f" "$ev/native-bmp/$n.m2s.bmp" --mono --scale 2 --scanlines > /dev/null
done

node "$here/wg1_parity.mjs" "$frames" "$ev/native-dump" "$ev/web-dump" "$ev/native-bmp"
