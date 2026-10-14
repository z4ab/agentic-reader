#!/usr/bin/env python3
"""Write tests/data/slides-16x9.pdf: 8 slides of 960 x 540 pt (16:9) for the M10.2 layout tests.

Each page has a thin grey frame inset 36 pt, the heading "Slide N" in 120 pt Helvetica
at baseline y = 300, and one body line in 28 pt Helvetica at baseline y = 220:

    Point <word>: distinct body text for slide N.

<word> is a NATO alphabet word per slide (alpha for slide 1 through hotel for slide 8),
so every page has text no other page has. Coordinates are PDF user space, origin
bottom-left. Content streams are not compressed, so the output does not depend on the
zlib version. Output is deterministic.

usage: python3 tests/data/make_slides_pdf.py [OUT]   (default: next to this script)
"""

import pathlib
import sys

PAGES = 8
WIDTH, HEIGHT = 960, 540
WORDS = ["alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel"]


def content(n: int) -> bytes:
    ops = [
        "0.6 0.6 0.6 RG 1 w 36 36 %d %d re S" % (WIDTH - 72, HEIGHT - 72),
        "BT /F1 120 Tf 72 300 Td (Slide %d) Tj ET" % n,
        "BT /F1 28 Tf 72 220 Td (Point %s: distinct body text for slide %d.) Tj ET"
        % (WORDS[n - 1], n),
    ]
    return "\n".join(ops).encode("ascii")


def stream(data: bytes) -> bytes:
    return b"<< /Length %d >>\nstream\n" % len(data) + data + b"\nendstream"


def build() -> bytes:
    # Objects: 1 catalog, 2 pages, 3 font, then a (page, content) pair per slide.
    objs = {}
    objs[1] = b"<< /Type /Catalog /Pages 2 0 R >>"
    objs[3] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
    kids = []
    for n in range(1, PAGES + 1):
        page_id = 2 + 2 * n
        content_id = page_id + 1
        kids.append("%d 0 R" % page_id)
        objs[page_id] = (
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
            "/Resources << /Font << /F1 3 0 R >> >> /Contents %d 0 R >>"
            % (WIDTH, HEIGHT, content_id)
        ).encode("ascii")
        objs[content_id] = stream(content(n))
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
    target = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).with_name("slides-16x9.pdf")
    target.write_bytes(build())
