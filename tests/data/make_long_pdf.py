#!/usr/bin/env python3
"""Write tests/data/long.pdf: 520 US Letter pages for the M2.3 scroll test.

Each page shows "Page N of 520" in 48 pt Helvetica at the top, 25 lines of body
text, and a dense pattern of 1500 stroked lines in the lower half so that each
render takes noticeable time (about 70 ms at 72 dpi). The pattern is one shared
form XObject, which keeps the file small. Output is deterministic.

usage: python3 tests/data/make_long_pdf.py [OUT]   (default: next to this script)
"""

import math
import pathlib
import sys
import zlib

PAGES = 520
WIDTH, HEIGHT = 612, 792


def content(n: int) -> bytes:
    ops = ["BT /F1 48 Tf 72 700 Td (Page %d of %d) Tj ET" % (n, PAGES)]
    ops.append("BT /F1 11 Tf 72 660 Td 14 TL")
    for i in range(25):
        ops.append("(Line %d of page %d. The quick brown fox jumps over the lazy dog.) '" % (i + 1, n))
    ops.append("ET")
    ops.append("/P1 Do")
    return zlib.compress("\n".join(ops).encode("ascii"), 9)


def pattern() -> bytes:
    ops = ["0.2 w 0 0 0.6 RG"]
    cx, cy, r = 306, 160, 135
    for i in range(1500):
        a = 2 * math.pi * i / 1500
        b = a * 7
        ops.append("%.2f %.2f m %.2f %.2f l S" % (
            cx + r * math.cos(a), cy + r * math.sin(a),
            cx + r * math.cos(b), cy + r * math.sin(b)))
    return zlib.compress("\n".join(ops).encode("ascii"), 9)


def stream(header: str, data: bytes) -> bytes:
    return ("<< %s /Length %d /Filter /FlateDecode >>\nstream\n" % (header, len(data))).encode(
        "ascii") + data + b"\nendstream"


def build() -> bytes:
    # Objects: 1 catalog, 2 pages, 3 font, 4 pattern, then (page, content) pairs.
    objs = {}
    objs[1] = b"<< /Type /Catalog /Pages 2 0 R >>"
    objs[3] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
    objs[4] = stream("/Type /XObject /Subtype /Form /BBox [0 0 %d %d]" % (WIDTH, HEIGHT), pattern())
    kids = []
    for n in range(1, PAGES + 1):
        page_id = 3 + 2 * n
        content_id = page_id + 1
        kids.append("%d 0 R" % page_id)
        objs[page_id] = (
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
            "/Resources << /Font << /F1 3 0 R >> /XObject << /P1 4 0 R >> >> /Contents %d 0 R >>"
            % (WIDTH, HEIGHT, content_id)
        ).encode("ascii")
        objs[content_id] = stream("", content(n))
    objs[2] = ("<< /Type /Pages /Kids [%s] /Count %d >>" % (" ".join(kids), PAGES)).encode("ascii")

    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}
    for i in sorted(objs):
        offsets[i] = len(out)
        out += b"%d 0 obj\n" % i + objs[i] + b"\nendobj\n"
    xref = len(out)
    count = max(objs) + 1
    out += b"xref\n0 %d\n0000000000 65535 f \n" % count
    for i in range(1, count):
        out += b"%010d 00000 n \n" % offsets[i]
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (count, xref)
    return bytes(out)


if __name__ == "__main__":
    target = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).with_name("long.pdf")
    target.write_bytes(build())
