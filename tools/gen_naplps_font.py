#!/usr/bin/env python3
"""
gen_naplps_font.py - Emit the 5x7 bitmap glyphs used by the Suite's NAPLPS
videotex renderer, whose NORMAL TEXT cell is only about 6 x 10 pixels.

Part of the Montreal Greek Times Unicorn Suite.
Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
Licensed under the GNU Affero General Public License v3.0 or later.

The glyph shapes below are ORIGINAL WORK drawn for this project.  Nothing
here is copied from any existing font file, package or web source: the
classic 5x7 dot-matrix letterform is a generic style, and these are plain,
conventional, legibility-first renderings of it.

  usage: python tools/gen_naplps_font.py > src/naplps_font_data.c
     or: python tools/gen_naplps_font.py src/naplps_font_data.c

Glyphs are held as ASCII art: a dict from the character to exactly seven
strings of exactly five characters, '#' for an on pixel and '.' for off.
Row 0 is the TOP row, row 6 the baseline row.

Vertical metrics, chosen once and applied consistently:

  * Uppercase and digits are full height, rows 0..6.
  * Lowercase x-height letters sit on the baseline with a five-row body,
    rows 2..6.  Ascenders (b d f h k l, and the dotted i) start at row 0.
  * There is no room below the baseline in a seven-row box, so DESCENDERS
    (g j p q y) are RAISED by one row: their body occupies rows 1..5 and
    the descending tail is drawn on row 6, the baseline row.  They
    therefore read as hanging below their neighbours even though no pixel
    actually goes below the baseline.

In the emitted C table each glyph is seven bytes, one per scan line, top
row first, and within a byte BIT 0 IS THE LEFTMOST PIXEL - column c sets
bit c for c in 0..4, so bits 5..7 are always zero.  That matches the
convention already documented in src/rip_font8x8.h.
"""

import sys

WIDTH = 5
HEIGHT = 7

FIRST_CHAR = 0x20
LAST_CHAR = 0x7E

GLYPHS = {
    ' ': ['.....',
          '.....',
          '.....',
          '.....',
          '.....',
          '.....',
          '.....'],

    '!': ['..#..',
          '..#..',
          '..#..',
          '..#..',
          '..#..',
          '.....',
          '..#..'],

    '"': ['.#.#.',
          '.#.#.',
          '.....',
          '.....',
          '.....',
          '.....',
          '.....'],

    '#': ['.#.#.',
          '.#.#.',
          '#####',
          '.#.#.',
          '#####',
          '.#.#.',
          '.#.#.'],

    '$': ['..#..',
          '.####',
          '#.#..',
          '.###.',
          '..#.#',
          '####.',
          '..#..'],

    '%': ['##...',
          '##..#',
          '...#.',
          '..#..',
          '.#...',
          '#..##',
          '...##'],

    '&': ['.##..',
          '#..#.',
          '#.#..',
          '.#...',
          '#.#.#',
          '#..#.',
          '.##.#'],

    "'": ['..#..',
          '..#..',
          '.....',
          '.....',
          '.....',
          '.....',
          '.....'],

    '(': ['...#.',
          '..#..',
          '.#...',
          '.#...',
          '.#...',
          '..#..',
          '...#.'],

    ')': ['.#...',
          '..#..',
          '...#.',
          '...#.',
          '...#.',
          '..#..',
          '.#...'],

    '*': ['.....',
          '..#..',
          '#.#.#',
          '.###.',
          '#.#.#',
          '..#..',
          '.....'],

    '+': ['.....',
          '..#..',
          '..#..',
          '#####',
          '..#..',
          '..#..',
          '.....'],

    ',': ['.....',
          '.....',
          '.....',
          '.....',
          '..#..',
          '..#..',
          '.#...'],

    '-': ['.....',
          '.....',
          '.....',
          '#####',
          '.....',
          '.....',
          '.....'],

    '.': ['.....',
          '.....',
          '.....',
          '.....',
          '.....',
          '.##..',
          '.##..'],

    '/': ['....#',
          '....#',
          '...#.',
          '..#..',
          '.#...',
          '#....',
          '#....'],

    '0': ['.###.',
          '#...#',
          '#..##',
          '#.#.#',
          '##..#',
          '#...#',
          '.###.'],

    '1': ['..#..',
          '.##..',
          '..#..',
          '..#..',
          '..#..',
          '..#..',
          '.###.'],

    '2': ['.###.',
          '#...#',
          '....#',
          '...#.',
          '..#..',
          '.#...',
          '#####'],

    '3': ['#####',
          '...#.',
          '..#..',
          '...#.',
          '....#',
          '#...#',
          '.###.'],

    '4': ['...#.',
          '..##.',
          '.#.#.',
          '#..#.',
          '#####',
          '...#.',
          '...#.'],

    '5': ['#####',
          '#....',
          '####.',
          '....#',
          '....#',
          '#...#',
          '.###.'],

    '6': ['..##.',
          '.#...',
          '#....',
          '####.',
          '#...#',
          '#...#',
          '.###.'],

    '7': ['#####',
          '....#',
          '...#.',
          '..#..',
          '.#...',
          '.#...',
          '.#...'],

    '8': ['.###.',
          '#...#',
          '#...#',
          '.###.',
          '#...#',
          '#...#',
          '.###.'],

    '9': ['.###.',
          '#...#',
          '#...#',
          '.####',
          '....#',
          '...#.',
          '.##..'],

    ':': ['.....',
          '..#..',
          '..#..',
          '.....',
          '..#..',
          '..#..',
          '.....'],

    ';': ['.....',
          '..#..',
          '..#..',
          '.....',
          '..#..',
          '..#..',
          '.#...'],

    '<': ['...#.',
          '..#..',
          '.#...',
          '#....',
          '.#...',
          '..#..',
          '...#.'],

    '=': ['.....',
          '.....',
          '#####',
          '.....',
          '#####',
          '.....',
          '.....'],

    '>': ['.#...',
          '..#..',
          '...#.',
          '....#',
          '...#.',
          '..#..',
          '.#...'],

    '?': ['.###.',
          '#...#',
          '....#',
          '...#.',
          '..#..',
          '.....',
          '..#..'],

    '@': ['.###.',
          '#...#',
          '#.###',
          '#.#.#',
          '#.###',
          '#....',
          '.###.'],

    'A': ['.###.',
          '#...#',
          '#...#',
          '#####',
          '#...#',
          '#...#',
          '#...#'],

    'B': ['####.',
          '#...#',
          '#...#',
          '####.',
          '#...#',
          '#...#',
          '####.'],

    'C': ['.###.',
          '#...#',
          '#....',
          '#....',
          '#....',
          '#...#',
          '.###.'],

    'D': ['###..',
          '#..#.',
          '#...#',
          '#...#',
          '#...#',
          '#..#.',
          '###..'],

    'E': ['#####',
          '#....',
          '#....',
          '####.',
          '#....',
          '#....',
          '#####'],

    'F': ['#####',
          '#....',
          '#....',
          '####.',
          '#....',
          '#....',
          '#....'],

    'G': ['.###.',
          '#...#',
          '#....',
          '#.###',
          '#...#',
          '#...#',
          '.####'],

    'H': ['#...#',
          '#...#',
          '#...#',
          '#####',
          '#...#',
          '#...#',
          '#...#'],

    'I': ['.###.',
          '..#..',
          '..#..',
          '..#..',
          '..#..',
          '..#..',
          '.###.'],

    'J': ['..###',
          '...#.',
          '...#.',
          '...#.',
          '...#.',
          '#..#.',
          '.##..'],

    'K': ['#...#',
          '#..#.',
          '#.#..',
          '##...',
          '#.#..',
          '#..#.',
          '#...#'],

    'L': ['#....',
          '#....',
          '#....',
          '#....',
          '#....',
          '#....',
          '#####'],

    'M': ['#...#',
          '##.##',
          '#.#.#',
          '#.#.#',
          '#...#',
          '#...#',
          '#...#'],

    'N': ['#...#',
          '#...#',
          '##..#',
          '#.#.#',
          '#..##',
          '#...#',
          '#...#'],

    'O': ['.###.',
          '#...#',
          '#...#',
          '#...#',
          '#...#',
          '#...#',
          '.###.'],

    'P': ['####.',
          '#...#',
          '#...#',
          '####.',
          '#....',
          '#....',
          '#....'],

    'Q': ['.###.',
          '#...#',
          '#...#',
          '#...#',
          '#.#.#',
          '#..#.',
          '.##.#'],

    'R': ['####.',
          '#...#',
          '#...#',
          '####.',
          '#.#..',
          '#..#.',
          '#...#'],

    'S': ['.####',
          '#....',
          '#....',
          '.###.',
          '....#',
          '....#',
          '####.'],

    'T': ['#####',
          '..#..',
          '..#..',
          '..#..',
          '..#..',
          '..#..',
          '..#..'],

    'U': ['#...#',
          '#...#',
          '#...#',
          '#...#',
          '#...#',
          '#...#',
          '.###.'],

    'V': ['#...#',
          '#...#',
          '#...#',
          '#...#',
          '#...#',
          '.#.#.',
          '..#..'],

    'W': ['#...#',
          '#...#',
          '#...#',
          '#.#.#',
          '#.#.#',
          '##.##',
          '#...#'],

    'X': ['#...#',
          '#...#',
          '.#.#.',
          '..#..',
          '.#.#.',
          '#...#',
          '#...#'],

    'Y': ['#...#',
          '#...#',
          '.#.#.',
          '..#..',
          '..#..',
          '..#..',
          '..#..'],

    'Z': ['#####',
          '....#',
          '...#.',
          '..#..',
          '.#...',
          '#....',
          '#####'],

    '[': ['.###.',
          '.#...',
          '.#...',
          '.#...',
          '.#...',
          '.#...',
          '.###.'],

    '\\': ['#....',
           '#....',
           '.#...',
           '..#..',
           '...#.',
           '....#',
           '....#'],

    ']': ['.###.',
          '...#.',
          '...#.',
          '...#.',
          '...#.',
          '...#.',
          '.###.'],

    '^': ['..#..',
          '.#.#.',
          '#...#',
          '.....',
          '.....',
          '.....',
          '.....'],

    '_': ['.....',
          '.....',
          '.....',
          '.....',
          '.....',
          '.....',
          '#####'],

    '`': ['.#...',
          '..#..',
          '.....',
          '.....',
          '.....',
          '.....',
          '.....'],

    'a': ['.....',
          '.....',
          '.###.',
          '....#',
          '.####',
          '#...#',
          '.####'],

    'b': ['#....',
          '#....',
          '####.',
          '#...#',
          '#...#',
          '#...#',
          '####.'],

    'c': ['.....',
          '.....',
          '.###.',
          '#....',
          '#....',
          '#....',
          '.###.'],

    'd': ['....#',
          '....#',
          '.####',
          '#...#',
          '#...#',
          '#...#',
          '.####'],

    'e': ['.....',
          '.....',
          '.###.',
          '#...#',
          '#####',
          '#....',
          '.###.'],

    'f': ['..##.',
          '.#...',
          '####.',
          '.#...',
          '.#...',
          '.#...',
          '.#...'],

    'g': ['.....',
          '.####',
          '#...#',
          '#...#',
          '.####',
          '....#',
          '.###.'],

    'h': ['#....',
          '#....',
          '####.',
          '#...#',
          '#...#',
          '#...#',
          '#...#'],

    'i': ['..#..',
          '.....',
          '.##..',
          '..#..',
          '..#..',
          '..#..',
          '.###.'],

    'j': ['...#.',
          '.....',
          '...#.',
          '...#.',
          '...#.',
          '#..#.',
          '.##..'],

    'k': ['#....',
          '#....',
          '#..#.',
          '#.#..',
          '##...',
          '#.#..',
          '#..#.'],

    'l': ['.##..',
          '..#..',
          '..#..',
          '..#..',
          '..#..',
          '..#..',
          '.###.'],

    'm': ['.....',
          '.....',
          '##.#.',
          '#.#.#',
          '#.#.#',
          '#...#',
          '#...#'],

    'n': ['.....',
          '.....',
          '####.',
          '#...#',
          '#...#',
          '#...#',
          '#...#'],

    'o': ['.....',
          '.....',
          '.###.',
          '#...#',
          '#...#',
          '#...#',
          '.###.'],

    'p': ['.....',
          '####.',
          '#...#',
          '#...#',
          '####.',
          '#....',
          '#....'],

    'q': ['.....',
          '.####',
          '#...#',
          '#...#',
          '.####',
          '....#',
          '....#'],

    'r': ['.....',
          '.....',
          '#.##.',
          '##..#',
          '#....',
          '#....',
          '#....'],

    's': ['.....',
          '.....',
          '.####',
          '#....',
          '.###.',
          '....#',
          '####.'],

    't': ['.#...',
          '.#...',
          '####.',
          '.#...',
          '.#...',
          '.#..#',
          '..##.'],

    'u': ['.....',
          '.....',
          '#...#',
          '#...#',
          '#...#',
          '#..##',
          '.##.#'],

    'v': ['.....',
          '.....',
          '#...#',
          '#...#',
          '#...#',
          '.#.#.',
          '..#..'],

    'w': ['.....',
          '.....',
          '#...#',
          '#...#',
          '#.#.#',
          '#.#.#',
          '.#.#.'],

    'x': ['.....',
          '.....',
          '#...#',
          '.#.#.',
          '..#..',
          '.#.#.',
          '#...#'],

    'y': ['.....',
          '#...#',
          '#...#',
          '#...#',
          '.####',
          '....#',
          '.###.'],

    'z': ['.....',
          '.....',
          '#####',
          '...#.',
          '..#..',
          '.#...',
          '#####'],

    '{': ['...##',
          '..#..',
          '..#..',
          '.#...',
          '..#..',
          '..#..',
          '...##'],

    '|': ['..#..',
          '..#..',
          '..#..',
          '..#..',
          '..#..',
          '..#..',
          '..#..'],

    '}': ['##...',
          '..#..',
          '..#..',
          '...#.',
          '..#..',
          '..#..',
          '##...'],

    '~': ['.....',
          '.....',
          '.#..#',
          '#.#.#',
          '#..#.',
          '.....',
          '.....'],
}


def rows_to_bytes(rows):
    """Turn seven 5-character art rows into seven bytes, bit 0 leftmost."""
    out = []
    for row in rows:
        v = 0
        for c, px in enumerate(row):
            if px == '#':
                v |= 1 << c
        out.append(v)
    return out


def main():
    codes = list(range(FIRST_CHAR, LAST_CHAR + 1))

    # Validate the art before emitting anything.
    for code in codes:
        ch = chr(code)
        if ch not in GLYPHS:
            sys.stderr.write("missing glyph for 0x%02X %r\n" % (code, ch))
            return 1
        rows = GLYPHS[ch]
        if len(rows) != HEIGHT:
            sys.stderr.write("glyph 0x%02X has %d rows, expected %d\n"
                             % (code, len(rows), HEIGHT))
            return 1
        for row in rows:
            if len(row) != WIDTH or any(p not in '#.' for p in row):
                sys.stderr.write("glyph 0x%02X has a bad row %r\n"
                                 % (code, row))
                return 1

    extra = sorted(set(GLYPHS) - set(chr(c) for c in codes))
    if extra:
        sys.stderr.write("unused glyphs: %r\n" % extra)
        return 1

    out = []
    w = out.append
    w("/*")
    w(" * naplps_font_data.c - 5x7 bitmap glyphs for the NAPLPS videotex")
    w(" *                      renderer.  GENERATED, do not edit.")
    w(" *")
    w(" * Generated by tools/gen_naplps_font.py.  The glyph shapes are")
    w(" * ORIGINAL WORK drawn for this project, not copied from any existing")
    w(" * font file, package or web source; the 5x7 dot-matrix letterform is")
    w(" * a generic teletext/videotex style.")
    w(" *")
    w(" * 95 glyphs, ASCII 0x20 (space) through 0x7E (tilde) inclusive.")
    w(" * Seven bytes per glyph, one per scan line, top row first, bit 0")
    w(" * leftmost - column c of a row is bit c, for c in 0..4.")
    w(" *")
    w(" * Part of the Montreal Greek Times Unicorn Suite.")
    w(" * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times")
    w(" * Licensed under the GNU Affero General Public License v3.0 or later.")
    w(" * See /COPYING and THIRD-PARTY-NOTICES.md.")
    w(" */")
    w("")
    w('#include "naplps_font.h"')
    w("")
    w("const unsigned char naplps_font5x7[95][7] = {")
    for code in codes:
        ch = chr(code)
        b = rows_to_bytes(GLYPHS[ch])
        # '*' and '/' would close or open a comment; name them instead.
        if ch == '*':
            label = "0x%02X asterisk" % code
        elif ch == '/':
            label = "0x%02X slash" % code
        elif ch == ' ':
            label = "0x%02X space" % code
        else:
            label = "0x%02X '%s'" % (code, ch)
        w("    { %s }, /* %s */"
          % (", ".join("0x%02X" % v for v in b), label))
    w("};")
    w("")

    text = "\n".join(out) + "\n"
    if len(sys.argv) > 1:
        with open(sys.argv[1], "w", encoding="ascii", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    sys.stderr.write("emitted %d glyphs\n" % len(codes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
