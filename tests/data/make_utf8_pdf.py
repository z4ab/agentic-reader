#!/usr/bin/env python3
"""Write tests/data/utf8.pdf: one 300 x 200 pt page with non-ASCII words for the M3.1 text test.

The page shows one line in 18 pt Helvetica with /WinAnsiEncoding at baseline y = 150
(PDF user space, origin bottom-left):

    Café naïve Größe €5

The string bytes are WinAnsi: é = 0xE9, ï = 0xEF, ö = 0xF6, ß = 0xDF, € = 0x80. Poppler
maps them to Unicode, so text extraction gives U+00E9, U+00EF, U+00F6, U+00DF and U+20AC
(a 3-byte UTF-8 sequence). The font is one of the standard 14 and is not embedded.
Output is deterministic.

usage: python3 tests/data/make_utf8_pdf.py [OUT]   (default: next to this script)
"""

import pathlib
import sys

WIDTH, HEIGHT = 300, 200
LINE = "Café naïve Größe €5"


def content() -> bytes:
    text = LINE.encode("cp1252")
    escaped = text.replace(b"\\", b"\\\\").replace(b"(", b"\\(").replace(b")", b"\\)")
    return b"BT /F1 18 Tf 20 150 Td (" + escaped + b") Tj ET"


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
    target = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).with_name("utf8.pdf")
    target.write_bytes(build())
