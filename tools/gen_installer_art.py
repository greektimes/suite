#!/usr/bin/env python3
"""gen_installer_art.py - the MSI wizard artwork.

Part of the Montreal Greek Times Unicorn Suite.
Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
Licensed under the GNU Affero General Public License v3.0 or later.

Run from the repository root:  python tools/gen_installer_art.py
Needs ImageMagick 7 on PATH, or at the path in MAGICK below. Build-time
only; nothing here ships inside the exe.

WHAT THIS REPLACES, AND WHY IT MATTERS

Until now the MSI shipped the WiX Toolset's own stock artwork: a maroon
dithered panel with the WiX wheel bleeding off the left edge and a black
badge holding the WiX logo. That is WiX's branding, not ours, and it was
going out on our installer.

The composition is kept, because it is a good one and because the
operator asked for it kept: same maroon panel, same partial slanted
bleed off the left edge, same badge at the top right. Only the marks
change.

    stock                            ours
    ------------------------------   ------------------------------
    maroon dithered panel, 164 px    BLUE dithered panel, same 164 px,
                                     same one-pixel dither. The mark is
                                     blue, so a maroon panel fought it.
    WiX wheel, slanted, bleeding     the MGT roundel, slanted, bleeding,
                                     tone-on-tone in the blue family
    black badge with the WiX logo    nothing. No badge, no box.

SIZES, verified against WiX 5.0.2's own art in
src/ext/UI/wixlib/Bitmaps/ rather than from memory:

    WixUIDialogBmp   493 x 312   dlgbmp.bmp
    WixUIBannerBmp   493 x  58   bannrbmp.bmp

The left 180 pixels are the only safe place for art on the dialog
bitmap: WelcomeDlg and ExitDialog put their title and body text at
dialog-unit X=135 of 370, which is 180 px in, and that text is black.
The panel is 164 px, inside that limit, and everything right of it stays
white.

PALETTE. Three colours: black, VGA blue 0000AA, and white. The ground is
0000AA laid down as a one-pixel checkerboard against black, which reads
as a deep navy and is a genuine 16-colour-era dither; the roundel is the
SAME blue laid down solid, so it reads a full step brighter than the
ground it sits on. That solid-on-dithered relationship is the stock's
own trick and it is what makes the mark a watermark rather than a
competitor to the text.

0000AA is the VGA palette's blue and 5555FF its bright blue. Both are
relatives of the logo's own srgb(0,0,254) rather than a clash with it.

WHY THE MARK IS A DIFFERENT BLUE FROM THE GROUND, and not merely solid
against dithered the way the maroon original was. Maroon carries about
eleven per cent luminance, so solid maroon against a fifty per cent
maroon dither is a five point difference and the eye sees the shape.
Blue carries about five. Solid 0000AA against dithered 0000AA is a two
point difference and the bleed simply disappeared: measured, the first
blue attempt put 76 per cent blue pixels in the panel and still looked
like a flat slab. The mark therefore steps up to the brighter blue.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pixelart import write_png

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, "installer", "art")
ICONSRC = os.path.join(ROOT, "assets", "icon-src")
LOGO = os.path.join(ROOT, "assets", "mgt-logo-thin.png")

MAGICK = os.environ.get(
    "MAGICK", r"C:\Program Files\ImageMagick-7.1.2-Q16-HDRI\magick.exe")

DIALOG_W, DIALOG_H = 493, 312
BANNER_W, BANNER_H = 493, 58
PANEL_W = 164                       # matches the stock panel exactly
PANEL_BLUE = "#0000AA"     # VGA blue: the dithered ground
MARK_BLUE  = "#5555FF"     # VGA bright blue: the roundel bleed


def run(args):
    r = subprocess.run([MAGICK] + args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(" ".join(args) + "\n" + r.stderr)
        raise SystemExit("ImageMagick failed")


def dither_panel(path, w, h):
    """Blue on black, one-pixel checkerboard. Written pixel by pixel
    because an ordered dither from a resampler is not the same thing and
    does not tile cleanly at this size."""
    m = (0x00, 0x00, 0xAA, 255)
    k = (0x00, 0x00, 0x00, 255)
    rows = [[m if (x + y) % 2 == 0 else k for x in range(w)]
            for y in range(h)]
    write_png(path, w, h, rows)


def main():
    if not os.path.isdir(ART):
        os.makedirs(ART)
    tmp = os.path.join(ART, "_tmp")
    if not os.path.isdir(tmp):
        os.makedirs(tmp)

    def t(n):
        return os.path.join(tmp, n)

    # ---- the marks -------------------------------------------------
    # The roundel in solid maroon. Solid against the dithered ground is
    # the stock's own trick: the mark reads as a watermark rather than
    # competing with the text.
    run([LOGO, "-alpha", "extract", "-threshold", "50%",
         "-negate", "-transparent", "white",
         "-fill", PANEL_BLUE, "-colorize", "100",
         "PNG32:" + t("logo_mask.png")])
    run([LOGO, "-fuzz", "35%", "-fill", MARK_BLUE, "-opaque", "#0000FE",
         "-fill", "none", "-opaque", "white",
         "PNG32:" + t("logo_maroon.png")])

    # Slanted and large enough to bleed off two edges, as the stock does.
    run([t("logo_maroon.png"), "-resize", "300x300",
         "-background", "none", "-rotate", "-20",
         "PNG32:" + t("logo_slant.png")])

    dither_panel(t("panel.png"), PANEL_W, DIALOG_H)

    # ---- dialog bitmap ---------------------------------------------
    # White ground, maroon panel on the left, the slanted roundel
    # clipped to the panel and bleeding off the left and bottom.
    run(["-size", "%dx%d" % (DIALOG_W, DIALOG_H), "xc:white",
         t("panel.png"), "-geometry", "+0+0", "-composite",
         "PNG32:" + t("stage1.png")])

    run([t("panel.png"),
         t("logo_slant.png"), "-geometry", "-132+8", "-composite",
         "PNG32:" + t("panel_logo.png")])

    run([t("stage1.png"),
         t("panel_logo.png"), "-geometry", "+0+0", "-composite",
         "PNG32:" + t("stage2.png")])

    # Four-colour remap: black, the ground blue, the mark blue, white.
    # This also kills the antialiasing the rotation introduced.
    run(["(", "xc:black", ")", "(", "xc:" + PANEL_BLUE, ")",
         "(", "xc:" + MARK_BLUE, ")", "(", "xc:white", ")",
         "+append", "PNG32:" + t("palette4.png")])
    run([t("stage2.png"), "-dither", "None",
         "-remap", t("palette4.png"), "PNG32:" + t("stage3.png")])

    # NO BADGE. The stock put a boxed logo here and the first revision
    # of this file put a boxed floppy in its place. Both were removed:
    # the panel is the mark and the bleed, and nothing else.
    run([t("stage3.png"), "PNG32:" + t("dialog.png")])

    run([t("dialog.png"), "-alpha", "remove", "-alpha", "off",
         "-type", "TrueColor", "BMP3:" + os.path.join(ART, "dialog.bmp")])

    # ---- banner bitmap ---------------------------------------------
    # The thin strip across the top of every interior page. White, a
    # blue rule along the bottom to tie it to the panel, and the roundel
    # itself at the right end where the stock put its logo. No box
    # around it: the mark stands on the white on its own.
    run([LOGO, "-filter", "Lanczos", "-resize", "40x40",
         "PNG32:" + t("logo_small.png")])
    run(["-size", "%dx%d" % (BANNER_W, BANNER_H), "xc:white",
         "(", "-size", "%dx3" % BANNER_W, "xc:" + PANEL_BLUE, ")",
         "-geometry", "+0+%d" % (BANNER_H - 3), "-composite",
         "(", t("logo_small.png"), ")",
         "-geometry", "+%d+8" % (BANNER_W - 56), "-composite",
         "PNG32:" + t("banner.png")])

    run([t("banner.png"), "-alpha", "remove", "-alpha", "off",
         "-type", "TrueColor", "BMP3:" + os.path.join(ART, "banner.bmp")])

    for f in ("dialog.bmp", "banner.bmp"):
        p = os.path.join(ART, f)
        print("  installer/art/%s  %d bytes" % (f, os.path.getsize(p)))


if __name__ == "__main__":
    main()
