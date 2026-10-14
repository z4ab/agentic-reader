#!/usr/bin/env python3
"""project.json checks for tests/acceptance/m10.2.sh and its steps.

usage:
  m10.2-state.py check ROOT REL PAGE LAYOUT
      ROOT/.reader/project.json has files[REL] with last_page == PAGE (0-based)
      and the layout key LAYOUT: "single", "continuous", or "absent" (no
      "layout" key in the entry).
  m10.2-state.py check-open ROOT REL
      open_file is REL.

Exit status 0 on success, 1 on a failed check (message on stderr), 2 on bad usage.
The file is read only. The app writes it.
"""

import json
import pathlib
import sys


def fail(message):
    print("m10.2-state: FAIL " + message, file=sys.stderr)
    sys.exit(1)


def project_json(root):
    return pathlib.Path(root) / ".reader" / "project.json"


def load(root):
    path = project_json(root)
    if not path.exists():
        fail("missing %s" % path)
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except ValueError as exc:
        fail("%s is not JSON: %s" % (path, exc))


def cmd_check(root, rel, page, layout):
    data = load(root)
    files = data.get("files")
    if not isinstance(files, dict) or not isinstance(files.get(rel), dict):
        fail("%s has no files entry %r: %s" % (project_json(root), rel, json.dumps(data)))
    entry = files[rel]
    if entry.get("last_page") != int(page):
        fail("%s: %r last_page is %r, expected %s" % (project_json(root), rel, entry.get("last_page"), page))
    if layout == "absent":
        if "layout" in entry:
            fail("%s: %r has layout %r, expected no layout key" % (project_json(root), rel, entry["layout"]))
    elif entry.get("layout") != layout:
        fail("%s: %r layout is %r, expected %r" % (project_json(root), rel, entry.get("layout"), layout))


def cmd_check_open(root, rel):
    data = load(root)
    if data.get("open_file") != rel:
        fail("%s: open_file is %r, expected %r" % (project_json(root), data.get("open_file"), rel))


COMMANDS = {
    "check": (cmd_check, 4),
    "check-open": (cmd_check_open, 2),
}


def main(argv):
    if len(argv) < 2 or argv[1] not in COMMANDS or len(argv) - 2 != COMMANDS[argv[1]][1]:
        print(__doc__, file=sys.stderr)
        return 2
    if argv[1] == "check" and argv[5] not in ("single", "continuous", "absent"):
        print(__doc__, file=sys.stderr)
        return 2
    COMMANDS[argv[1]][0](*argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
