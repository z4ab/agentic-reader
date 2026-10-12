#!/usr/bin/env python3
"""Fixture and project.json helper for tests/acceptance/m6.5.sh and its steps.

usage:
  m6.5-state.py make-pdf OUT PAGES WIDTH HEIGHT
      Writes a PDF of PAGES pages of WIDTH x HEIGHT pt. Page N shows
      "Chapter N of PAGES" in 28 pt type at the top, three lines of body text
      and a thick border around the page. Deterministic output.
  m6.5-state.py check ROOT REL PAGE ZOOM
      ROOT/.reader/project.json has files[REL] with last_page == PAGE
      (0-based) and zoom ZOOM: "fit-width", "fit-page", or a number that the
      stored custom zoom (a decimal scale factor string, 1.0 = 100%) matches
      within 0.005.
  m6.5-state.py check-not ROOT REL PAGE
      files[REL].last_page is not PAGE yet (a missing file or entry passes).
  m6.5-state.py check-open ROOT REL
      open_file is REL.
  m6.5-state.py set ROOT REL PAGE ZOOM
      Writes files[REL] = {"last_page": PAGE, "zoom": ZOOM} into
      ROOT/.reader/project.json (created as an empty version 1 project when
      missing), keeping every other field.

Exit status 0 on success, 1 on a failed check (with a message on stderr),
2 on bad usage.
"""

import json
import os
import pathlib
import sys
import zlib


def fail(message):
    print("m6.5-state: FAIL " + message, file=sys.stderr)
    sys.exit(1)


def project_json(root):
    return pathlib.Path(root) / ".reader" / "project.json"


def load(root):
    path = project_json(root)
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except ValueError as exc:
        fail("%s is not JSON: %s" % (path, exc))


def entry(root, rel):
    data = load(root)
    if data is None:
        fail("missing %s" % project_json(root))
    files = data.get("files")
    if not isinstance(files, dict) or rel not in files:
        fail("%s has no files entry %r: %s" % (project_json(root), rel, json.dumps(data)))
    return files[rel]


def zoom_matches(stored, want):
    if want in ("fit-width", "fit-page"):
        return stored == want
    if not isinstance(stored, str) or stored in ("", "fit-width", "fit-page"):
        return False
    try:
        return abs(float(stored) - float(want)) <= 0.005
    except ValueError:
        return False


def cmd_check(root, rel, page, zoom):
    e = entry(root, rel)
    if e.get("last_page") != int(page):
        fail("%s: %r last_page is %r, expected %s" % (project_json(root), rel, e.get("last_page"), page))
    if not zoom_matches(e.get("zoom"), zoom):
        fail("%s: %r zoom is %r, expected %s" % (project_json(root), rel, e.get("zoom"), zoom))


def cmd_check_not(root, rel, page):
    data = load(root)
    if data is None:
        return
    files = data.get("files")
    if isinstance(files, dict) and isinstance(files.get(rel), dict):
        if files[rel].get("last_page") == int(page):
            fail("%s: %r last_page is already %s (saved before the 1 s debounce)"
                 % (project_json(root), rel, page))


def cmd_check_open(root, rel):
    data = load(root)
    if data is None:
        fail("missing %s" % project_json(root))
    if data.get("open_file") != rel:
        fail("%s: open_file is %r, expected %r" % (project_json(root), data.get("open_file"), rel))


def cmd_set(root, rel, page, zoom):
    data = load(root)
    if data is None:
        data = {"version": 1, "open_file": None, "files": {}, "chats": []}
    files = data.setdefault("files", {})
    old = files.get(rel) if isinstance(files.get(rel), dict) else {}
    old["last_page"] = int(page)
    old["zoom"] = zoom
    files[rel] = old
    path = project_json(root)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def page_content(n, pages, width, height):
    ops = [
        "6 w 0.2 0.4 0.8 RG 3 3 %d %d re S" % (width - 6, height - 6),
        "BT /F1 28 Tf 24 %d Td (Chapter %d of %d) Tj ET" % (height - 60, n, pages),
        "BT /F1 12 Tf 24 %d Td 16 TL" % (height - 100),
    ]
    for i in range(3):
        ops.append("(Line %d of chapter %d.) '" % (i + 1, n))
    ops.append("ET")
    return zlib.compress("\n".join(ops).encode("ascii"), 9)


def cmd_make_pdf(out, pages, width, height):
    pages, width, height = int(pages), int(width), int(height)
    objects = {}
    # 1 catalog, 2 pages, 3 font, then a page and its content per page.
    kids = []
    for i in range(pages):
        page_id, content_id = 4 + 2 * i, 5 + 2 * i
        kids.append(page_id)
        data = page_content(i + 1, pages, width, height)
        objects[content_id] = (b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(data)
                               + data + b"\nendstream")
        objects[page_id] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
                            b"/Resources << /Font << /F1 3 0 R >> >> /Contents %d 0 R >>"
                            % (width, height, content_id))
    objects[1] = b"<< /Type /Catalog /Pages 2 0 R >>"
    objects[2] = (b"<< /Type /Pages /Kids [" + b" ".join(b"%d 0 R" % k for k in kids)
                  + b"] /Count %d >>" % pages)
    objects[3] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"

    body = bytearray(b"%PDF-1.4\n")
    offsets = {}
    for num in sorted(objects):
        offsets[num] = len(body)
        body += b"%d 0 obj\n" % num + objects[num] + b"\nendobj\n"
    xref = len(body)
    count = max(objects) + 1
    body += b"xref\n0 %d\n0000000000 65535 f \n" % count
    for num in range(1, count):
        body += b"%010d 00000 n \n" % offsets[num]
    body += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (count, xref)
    pathlib.Path(out).write_bytes(bytes(body))


COMMANDS = {
    "make-pdf": (cmd_make_pdf, 4),
    "check": (cmd_check, 4),
    "check-not": (cmd_check_not, 3),
    "check-open": (cmd_check_open, 2),
    "set": (cmd_set, 4),
}


def main(argv):
    if len(argv) < 2 or argv[1] not in COMMANDS or len(argv) - 2 != COMMANDS[argv[1]][1]:
        print(__doc__, file=sys.stderr)
        return 2
    COMMANDS[argv[1]][0](*argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
