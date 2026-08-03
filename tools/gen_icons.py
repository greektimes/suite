#!/usr/bin/env python3
"""gen_icons.py - generate the Suite's brand icon and the retro setup icon.

Part of the Montreal Greek Times Unicorn Suite.
Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
Licensed under the GNU Affero General Public License v3.0 or later.

Run from the repository root:  python tools/gen_icons.py

TWO ICONS, TWO JOBS.

1. THE BRAND ICON, assets/mgt_suite.ico. The one mark, used for the exe,
   the app window, both shortcuts and the Apps and features entry. Built
   from assets/mgt-logo-thin.png.

   EVERY size is a Lanczos reduction of the master. That is deliberate,
   and it is a correction.

   A previous revision drew 16 and 32 with hard pixels to fight the
   softness of a small downscale. It bought sharpness and paid for it
   with framing: those two came out at 87.5 and 93.8 per cent fill
   against 100 per cent at 48 and 256, and, worse, with a ring 25 per
   cent of the radius where the master's is 11 per cent, and two thick
   slashes where the master has seven fine ones. A heavier mark in a
   smaller box reads as ZOOMED IN, and it was: side by side with the
   larger sizes the icon visibly jumped scale between 32 and 48.

   Consistent framing beats sharpness. One filter, one source, one
   proportion at every size, exactly as the build before it did.

2. THE RETRO SETUP ICON, assets/mgt_setup_retro.ico. ORIGINAL MGT
   ARTWORK. A 3.5-inch floppy disk with a download arrow on its label,
   drawn in the Windows 3.1 and 95 idiom: hard-edged pixels, the 16
   colour EGA/VGA palette, a one pixel light bevel top and left and a
   dark edge bottom and right.

   It is drawn from scratch, rectangle by rectangle, in this file. It is
   not a copy, a trace or an upscale of any Microsoft icon or of anything
   from an icon site of unknown licence. Nothing third-party is involved.

   Authored at 16, 24 and 32. The other two sizes are exact integer
   derivations so no resampler ever touches the art: 48 is the 24 doubled
   and 256 is the 32 scaled eight times, both nearest-neighbour.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pixelart import VGA16, blank, scale, write_png

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "icon-src")
ASSETS = os.path.join(ROOT, "assets")
MASTER = os.path.join(ASSETS, "mgt-logo-thin.png")

# ImageMagick does the two jobs pixel placement cannot: the Lanczos
# reduction of the master for the sizes that survive it, and packing the
# PNGs into .ico containers. Build-time only.
MAGICK = os.environ.get(
    "MAGICK", r"C:\Program Files\ImageMagick-7.1.2-Q16-HDRI\magick.exe")


def magick(args):
    r = subprocess.run([MAGICK] + args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(" ".join(args) + "\n" + r.stderr)
        raise SystemExit("ImageMagick failed")

# Every app-icon size now comes from the master, so there is no
# hand-placed colour to keep in step with it any more.
APP_ICON_SIZES = (16, 32, 48, 256)


# ----------------------------------------------------------------------
# Tiny drawing helpers. Rectangles and nothing else, which is how these
# icons were made the first time round.
# ----------------------------------------------------------------------

def rect(px, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if 0 <= y < len(px) and 0 <= x < len(px[0]):
                px[y][x] = c
    return px


def frame(px, x0, y0, x1, y1, c):
    rect(px, x0, y0, x1, y0, c)
    rect(px, x0, y1, x1, y1, c)
    rect(px, x0, y0, x0, y1, c)
    rect(px, x1, y0, x1, y1, c)
    return px


C = VGA16


# ----------------------------------------------------------------------
# 2. Retro setup icon: a 3.5-inch floppy with a download arrow
# ----------------------------------------------------------------------

def floppy32():
    """32 x 32. The reference drawing; 256 is this one scaled 8x."""
    p = blank(32, 32)
    BODY, EDGE, LIT = C["8"], C["0"], C["7"]
    LABEL, ARROW, ASHADE = C["F"], C["2"], C["A"]

    # Shell, 26 wide by 28 tall, with the top-right corner chamfered the
    # way a real 3.5-inch disk is keyed.
    rect(p, 3, 1, 28, 28, BODY)
    for i in range(4):                       # chamfer
        rect(p, 28 - i, 1 + i, 28, 1 + i, (0, 0, 0, 0))
    frame(p, 3, 1, 28, 28, EDGE)
    for i in range(4):                       # chamfer edge
        p[1 + i][28 - i] = EDGE
    rect(p, 4, 2, 27, 2, LIT)                # bevel: light top
    rect(p, 4, 2, 4, 27, LIT)                # bevel: light left
    rect(p, 5, 27, 27, 27, EDGE)             # bevel: dark bottom
    rect(p, 27, 3, 27, 27, EDGE)             # bevel: dark right

    # Metal shutter, upper right of centre, with the head slot cut in it.
    rect(p, 14, 3, 25, 10, LIT)
    frame(p, 14, 3, 25, 10, EDGE)
    rect(p, 16, 5, 20, 9, C["8"])            # the slot
    rect(p, 22, 5, 24, 9, C["7"])            # the sprung plate

    # Label.
    rect(p, 6, 13, 25, 26, LABEL)
    frame(p, 6, 13, 25, 26, EDGE)

    # Download arrow on the label: this is the "install" of it.
    rect(p, 14, 15, 17, 20, ARROW)           # shaft
    for i, y in enumerate(range(21, 25)):    # head, narrowing downward
        rect(p, 11 + i, y, 20 - i, y, ARROW)
    rect(p, 17, 15, 17, 20, ASHADE)          # one-pixel lift on the shaft
    return p


def floppy24():
    """24 x 24, drawn for itself. 48 is this one doubled."""
    p = blank(24, 24)
    BODY, EDGE, LIT = C["8"], C["0"], C["7"]
    LABEL, ARROW = C["F"], C["2"]

    rect(p, 2, 1, 21, 22, BODY)
    for i in range(3):
        rect(p, 21 - i, 1 + i, 21, 1 + i, (0, 0, 0, 0))
    frame(p, 2, 1, 21, 22, EDGE)
    for i in range(3):
        p[1 + i][21 - i] = EDGE
    rect(p, 3, 2, 20, 2, LIT)
    rect(p, 3, 2, 3, 21, LIT)
    rect(p, 4, 21, 20, 21, EDGE)
    rect(p, 20, 3, 20, 21, EDGE)

    rect(p, 11, 3, 19, 8, LIT)
    frame(p, 11, 3, 19, 8, EDGE)
    rect(p, 13, 5, 16, 7, C["8"])

    rect(p, 5, 11, 19, 20, LABEL)
    frame(p, 5, 11, 19, 20, EDGE)

    rect(p, 11, 13, 13, 16, ARROW)
    for i, y in enumerate(range(17, 20)):
        rect(p, 9 + i, y, 15 - i, y, ARROW)
    return p


def floppy16():
    """16 x 16. Everything that will not survive at this size is gone:
    no bevel, no slot detail, no label rules. Shell, shutter, label,
    arrow."""
    p = blank(16, 16)
    BODY, EDGE, LIT = C["8"], C["0"], C["7"]
    LABEL, ARROW = C["F"], C["2"]

    rect(p, 1, 0, 14, 15, BODY)
    for i in range(2):
        rect(p, 14 - i, 0 + i, 14, 0 + i, (0, 0, 0, 0))
    frame(p, 1, 0, 14, 15, EDGE)
    p[0][14] = EDGE
    p[1][14] = EDGE

    rect(p, 8, 2, 13, 5, LIT)                # shutter
    frame(p, 8, 2, 13, 5, EDGE)

    rect(p, 3, 8, 12, 14, LABEL)             # label
    frame(p, 3, 8, 12, 14, EDGE)

    rect(p, 7, 9, 8, 11, ARROW)              # arrow
    rect(p, 5, 12, 10, 12, ARROW)
    rect(p, 6, 13, 9, 13, ARROW)
    return p


# ----------------------------------------------------------------------

def main():
    if not os.path.isdir(OUT):
        os.makedirs(OUT)

    def emit(name, px):
        path = os.path.join(OUT, name)
        write_png(path, len(px[0]), len(px), px)
        print("  %s  %dx%d" % (name, len(px[0]), len(px)))

    print("brand icon, one filter and one source at every size:")
    for size in APP_ICON_SIZES:
        out = os.path.join(OUT, "mgt-logo-%d.png" % size)
        magick([MASTER, "-filter", "Lanczos",
                "-resize", "%dx%d" % (size, size), "-strip",
                "PNG32:" + out])
        print("  mgt-logo-%d.png  %dx%d  Lanczos from the master"
              % (size, size, size))

    print("retro setup icon, original MGT artwork:")
    f16, f24, f32 = floppy16(), floppy24(), floppy32()
    emit("mgt-setup-16.png", f16)
    emit("mgt-setup-24.png", f24)
    emit("mgt-setup-32.png", f32)
    emit("mgt-setup-48.png", scale(f24, 2))     # exact 2x, no resampler
    emit("mgt-setup-256.png", scale(f32, 8))    # exact 8x, no resampler

    # Pack the .ico containers. Doing it here rather than by hand is the
    # difference between a pipeline and a note in a commit message.
    print("icon containers:")
    magick([os.path.join(OUT, "mgt-logo-%d.png" % s) for s in APP_ICON_SIZES]
           + [os.path.join(ASSETS, "mgt_suite.ico")])
    print("  assets/mgt_suite.ico        16, 32, 48, 256")
    magick([os.path.join(OUT, "mgt-setup-16.png"),
            os.path.join(OUT, "mgt-setup-24.png"),
            os.path.join(OUT, "mgt-setup-32.png"),
            os.path.join(OUT, "mgt-setup-48.png"),
            os.path.join(OUT, "mgt-setup-256.png"),
            os.path.join(ASSETS, "mgt_setup_retro.ico")])
    print("  assets/mgt_setup_retro.ico  16, 24, 32, 48, 256")


if __name__ == "__main__":
    main()
