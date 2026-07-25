#!/usr/bin/env python3
"""Generate CREDITS.TXT from CREDITS.md at package time.

The release archive uses README.TXT, LICENSE.TXT and NOTICES.TXT, so the
credits ship as CREDITS.TXT to match. It is GENERATED rather than
hand-maintained so the two cannot drift: CREDITS.md is the only source.

The rendering is deliberately minimal. It strips the markdown that reads
badly in Notepad (emphasis markers, header hashes, link brackets) and
leaves everything else alone, including the line breaks, because the file
is written to be read as prose either way.

usage: make_credits_txt.py <in.md> <out.txt>
"""
import re, sys, io

src, dst = sys.argv[1], sys.argv[2]
text = io.open(src, encoding='utf-8').read()

out = []
for line in text.split('\n'):
    # [label](url) -> label (url); bare autolinks are left as they are.
    line = re.sub(r'\[([^\]]+)\]\((https?://[^)]+)\)', r'\1 (\2)', line)
    # **bold** and *italic* markers carry no meaning in plain text.
    line = line.replace('**', '')
    # Headings: drop the hashes, underline the top two levels so the
    # structure survives without markdown.
    m = re.match(r'^(#{1,6})\s+(.*)$', line)
    if m:
        level, title = len(m.group(1)), m.group(2).rstrip()
        out.append(title)
        if level == 1:
            out.append('=' * len(title))
        elif level == 2:
            out.append('-' * len(title))
        continue
    out.append(line.rstrip())

body = '\n'.join(out).rstrip() + '\n'
# CRLF: this file is read on Windows, in Notepad, from a zip.
io.open(dst, 'w', encoding='utf-8', newline='\r\n').write(body)
print('wrote %s (%d bytes)' % (dst, len(body)))
