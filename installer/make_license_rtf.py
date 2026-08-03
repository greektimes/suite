#!/usr/bin/env python3
"""Wrap a plain-text licence in minimal RTF for the WiX licence dialog.

Part of the Montreal Greek Times Unicorn Suite.
Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
Licensed under the GNU Affero General Public License v3.0 or later.

WixUILicenseRtf wants RTF, and the licence we have to show is /COPYING,
which is plain text. Keeping a hand-made LICENSE.rtf beside it would be a
second copy of the AGPL free to fall behind the first, so it is generated
at package time instead and payload/ is gitignored.

The rendering is deliberately dumb: a monospaced font, no styling, hard
line breaks exactly where the source has them. The AGPL is laid out for
a fixed-width reader and reflowing it would only make it worse.

usage: make_license_rtf.py <in.txt> <out.rtf>
"""
import io
import sys

src, dst = sys.argv[1], sys.argv[2]
text = io.open(src, encoding="utf-8").read()

out = []
# \fs18 is 9pt (RTF half-points). The AGPL is 34 kB and the dialog's box
# is small, so anything larger scrolls forever.
out.append(r"{\rtf1\ansi\ansicpg1252\deff0"
           r"{\fonttbl{\f0\fmodern\fcharset0 Courier New;}}"
           r"\viewkind4\uc1\pard\f0\fs18 ")

for ch in text.replace("\r\n", "\n").replace("\r", "\n"):
    if ch == "\n":
        out.append("\\par\n")
    elif ch in ("\\", "{", "}"):
        out.append("\\" + ch)
    elif ord(ch) < 128:
        out.append(ch)
    else:
        # RTF escapes non-ASCII as a signed 16-bit decimal after \u,
        # with an ASCII fallback character for readers that cannot.
        cp = ord(ch)
        if cp > 32767:
            cp -= 65536
        out.append("\\u%d?" % cp)

out.append("}")

io.open(dst, "w", encoding="ascii", newline="").write("".join(out))
print("wrote %s (%d bytes)" % (dst, len("".join(out))))
