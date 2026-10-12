#!/usr/bin/env python3
"""Write the 4-page PDF that tests/acceptance/m5.3.sh copies to attention.pdf.

Four US Letter pages (612 x 792 pt). Each page shows a heading "Page N" in 16 pt
Helvetica at baseline y = 720 (PDF user space, origin bottom-left), then a
paragraph in 11 pt Helvetica with 14 pt leading, first baseline y = 680, left
edge x = 72. Pages 1, 2 and 4 hold a one-line paragraph. Page 3 holds the
seven-line paragraph PARAGRAPH (469 chars with the "\\n" between lines, one of
them non-ASCII: "naïve"), so "Ask about selection" quotes only its first 300
chars while the prompt's context carries all of it.

The font is one of the standard 14, not embedded, with WinAnsiEncoding.
Output is deterministic.

usage: python3 tests/acceptance/m5.3-make-pdf.py OUT
       python3 tests/acceptance/m5.3-make-pdf.py --paragraph   (prints PARAGRAPH, no newline at the end)
"""

import pathlib
import sys

WIDTH, HEIGHT = 612, 792
PARAGRAPH_LINES = [
    "Scaled dot-product attention computes a weighted sum of the values,",
    "where each weight comes from a naïve softmax over the dot products",
    "of the query with every key, divided by the square root of the key",
    "size so that large dimensions do not push the softmax into regions",
    "with very small gradients. Multi-head attention runs several heads",
    "in parallel and concatenates their outputs before a final linear",
    "projection. This sentence closes the paragraph on page three: omega.",
]
PARAGRAPH = "\n".join(PARAGRAPH_LINES)
PAGES = [
    ("Page 1", ["The first page introduces the reader context test document."]),
    ("Page 2", ["The second page is only here so the selection sits further on."]),
    ("Page 3", PARAGRAPH_LINES),
    ("Page 4", ["The fourth page ends the reader context test document."]),
]


def pdf_string(s: str) -> str:
    """A PDF literal string body in WinAnsi (cp1252), non-ASCII as octal escapes."""
    out = []
    for byte in s.encode("cp1252"):
        ch = chr(byte)
        if ch in "\\()":
            out.append("\\" + ch)
        elif byte < 0x20 or byte > 0x7E:
            out.append("\\%03o" % byte)
        else:
            out.append(ch)
    return "".join(out)


def content(heading: str, lines) -> bytes:
    ops = ["BT /F1 16 Tf 72 720 Td (%s) Tj ET" % pdf_string(heading)]
    ops.append("BT /F1 11 Tf 14 TL 72 680 Td")
    for i, line in enumerate(lines):
        ops.append(("(%s) Tj" if i == 0 else "(%s) '") % pdf_string(line))
    ops.append("ET")
    return "\n".join(ops).encode("ascii")


def build() -> bytes:
    n = len(PAGES)
    # 1 catalog, 2 pages, 3 font, then a page object and its content per page.
    page_ids = [4 + 2 * i for i in range(n)]
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: ("<< /Type /Pages /Kids [%s] /Count %d >>"
            % (" ".join("%d 0 R" % p for p in page_ids), n)).encode("ascii"),
        3: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
    }
    for (heading, lines), pid in zip(PAGES, page_ids):
        data = content(heading, lines)
        objs[pid] = ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
                     "/Resources << /Font << /F1 3 0 R >> >> /Contents %d 0 R >>"
                     % (WIDTH, HEIGHT, pid + 1)).encode("ascii")
        objs[pid + 1] = b"<< /Length %d >>\nstream\n" % len(data) + data + b"\nendstream"

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
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    if sys.argv[1] == "--paragraph":
        sys.stdout.buffer.write(PARAGRAPH.encode("utf-8"))
    else:
        pathlib.Path(sys.argv[1]).write_bytes(build())
