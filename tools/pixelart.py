#!/usr/bin/env python3
"""pixelart.py - hard-edged pixel art helpers, and a PNG writer with no
dependencies.

Part of the Montreal Greek Times Unicorn Suite.
Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
Licensed under the GNU Affero General Public License v3.0 or later.

Why this exists rather than a call to ImageMagick: at 16 and 24 pixels
there is no such thing as a good automatic downscale. Every pixel has to
be placed, and every edge has to be hard. This module writes exact RGBA
pixels and never antialiases anything.

Two ways to make an image:

  Grid  an ASCII grid plus a palette, one character per pixel. This is
        how the retro setup icon is authored, because it is drawn rather
        than computed.

  Draw  circle, ring and diagonal-band primitives with hard edges, used
        for the small sizes of the roundel logo, whose geometry is
        regular enough to compute and which must stay faithful to the
        proportions of the full-size original.

PNG is written directly with zlib and struct. Pillow is not installed in
the MSYS2 UCRT64 python this repository builds with, and adding it just
to save thirty lines would be a new build dependency for artwork that is
generated once.
"""

import struct
import zlib


# ----------------------------------------------------------------------
# PNG output
# ----------------------------------------------------------------------

def write_png(path, w, h, pixels):
    """pixels: list of h rows, each a list of w (r, g, b, a) tuples."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)                      # filter type 0, None
        row = pixels[y]
        for x in range(w):
            r, g, b, a = row[x]
            raw += bytes((r, g, b, a))

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def blank(w, h, rgba=(0, 0, 0, 0)):
    return [[rgba for _ in range(w)] for _ in range(h)]


def scale(pixels, factor):
    """Nearest-neighbour integer upscale. No interpolation, ever."""
    h = len(pixels)
    w = len(pixels[0])
    out = []
    for y in range(h * factor):
        src = pixels[y // factor]
        out.append([src[x // factor] for x in range(w * factor)])
    return out


# ----------------------------------------------------------------------
# ASCII grids
# ----------------------------------------------------------------------

def from_grid(rows, palette):
    """rows: list of equal-length strings. palette: char -> (r,g,b,a).
    A space is always transparent unless the palette overrides it."""
    h = len(rows)
    w = len(rows[0])
    for i, r in enumerate(rows):
        if len(r) != w:
            raise ValueError("row %d is %d wide, expected %d" % (i, len(r), w))
    out = []
    for r in rows:
        out.append([palette.get(c, (0, 0, 0, 0)) for c in r])
    return out


# The classic 16-colour EGA/VGA palette, which is the whole visual
# vocabulary the retro setup icon is allowed to use.
VGA16 = {
    "0": (0x00, 0x00, 0x00, 255),   # black
    "1": (0x00, 0x00, 0xAA, 255),   # blue
    "2": (0x00, 0xAA, 0x00, 255),   # green
    "3": (0x00, 0xAA, 0xAA, 255),   # cyan
    "4": (0xAA, 0x00, 0x00, 255),   # red
    "5": (0xAA, 0x00, 0xAA, 255),   # magenta
    "6": (0xAA, 0x55, 0x00, 255),   # brown
    "7": (0xAA, 0xAA, 0xAA, 255),   # light grey
    "8": (0x55, 0x55, 0x55, 255),   # dark grey
    "9": (0x55, 0x55, 0xFF, 255),   # bright blue
    "A": (0x55, 0xFF, 0x55, 255),   # bright green
    "B": (0x55, 0xFF, 0xFF, 255),   # bright cyan
    "C": (0xFF, 0x55, 0x55, 255),   # bright red
    "D": (0xFF, 0x55, 0xFF, 255),   # bright magenta
    "E": (0xFF, 0xFF, 0x55, 255),   # yellow
    "F": (0xFF, 0xFF, 0xFF, 255),   # white
    ".": (0, 0, 0, 0),              # transparent
    " ": (0, 0, 0, 0),
}


# ----------------------------------------------------------------------
# Hard-edged primitives, for the roundel
# ----------------------------------------------------------------------

def disc_mask(size, r_outer, r_inner=0.0):
    """A filled disc, or a ring when r_inner > 0. Radii are in pixels,
    measured from the exact centre of the square. A pixel is in when its
    CENTRE is in, which is what keeps the edge hard and the result
    symmetric."""
    c = (size - 1) / 2.0
    m = [[False] * size for _ in range(size)]
    for y in range(size):
        for x in range(size):
            d = ((x - c) ** 2 + (y - c) ** 2) ** 0.5
            m[y][x] = (d <= r_outer) and (d >= r_inner)
    return m


def band_mask(size, angle_deg, period, width, phase=0.0):
    """Parallel diagonal bands, as in the logo's slashes. `period` is the
    centre-to-centre spacing and `width` the band thickness, both in
    pixels measured perpendicular to the bands."""
    import math
    a = math.radians(angle_deg)
    nx, ny = -math.sin(a), math.cos(a)      # band normal
    c = (size - 1) / 2.0
    m = [[False] * size for _ in range(size)]
    for y in range(size):
        for x in range(size):
            t = (x - c) * nx + (y - c) * ny + phase
            m[y][x] = (t % period) < width
    return m


def paint(pixels, mask, rgba):
    for y in range(len(mask)):
        for x in range(len(mask[0])):
            if mask[y][x]:
                pixels[y][x] = rgba
    return pixels
