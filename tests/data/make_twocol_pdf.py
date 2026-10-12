#!/usr/bin/env python3
"""Write tests/data/twocol.pdf: one US Letter page with two text columns for the M3.2 selection test.

The page (612 x 792 pt) shows a heading across the top in 16 pt Helvetica at baseline
y = 720 (PDF user space, origin bottom-left), then two columns of four lines each in
11 pt Helvetica with 14 pt leading. The left column starts at x = 72, the right column
at x = 324 (a gap of about 40 pt between the columns), both with the first baseline at
y = 680:

    Two Column Sample

    Alpha starts the left column and       Bravo opens the right column and
    its words run down a narrow strip      a reader meets it only after the
    of the page until the left side        left column is done, so it closes
    comes to an end at omega.              the page here at zulu.

Each column is drawn in its own text object, left column first, so both the content
stream and poppler's reading order give: heading, left column, right column. The last
word of the left column ("omega.") sits low on the page and the first word of the right
column ("Bravo") high, which is the column jump the copied text must break with a newline.
The font is one of the standard 14 and is not embedded. Output is deterministic.

usage: python3 tests/data/make_twocol_pdf.py [OUT]   (default: next to this script)
"""

import pathlib
import sys

WIDTH, HEIGHT = 612, 792
HEADING = "Two Column Sample"
LEFT = [
    "Alpha starts the left column and",
    "its words run down a narrow strip",
    "of the page until the left side",
    "comes to an end at omega.",
]
RIGHT = [
    "Bravo opens the right column and",
    "a reader meets it only after the",
    "left column is done, so it closes",
    "the page here at zulu.",
]


def escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def column(x: int, lines) -> list:
    ops = ["BT /F1 11 Tf 14 TL %d 680 Td" % x]
    for i, line in enumerate(lines):
        ops.append(("(%s) Tj" if i == 0 else "(%s) '") % escape(line))
    ops.append("ET")
    return ops


def content() -> bytes:
    ops = ["BT /F1 16 Tf 72 720 Td (%s) Tj ET" % escape(HEADING)]
    ops += column(72, LEFT)
    ops += column(324, RIGHT)
    return "\n".join(ops).encode("ascii")


def build() -> bytes:
    data = content()
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        3: ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
            "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>" % (WIDTH, HEIGHT)).encode("ascii"),
        4: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        5: b"<< /Length %d >>\nstream\n" % len(data) + data + b"\nendstream",
    }

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
    target = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).with_name("twocol.pdf")
    target.write_bytes(build())
