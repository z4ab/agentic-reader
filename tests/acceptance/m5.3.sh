#!/usr/bin/env bash
# M5.3 acceptance: the reader context of each prompt, the context chip and its
# x, and "Ask about selection" (context menu and Ctrl+Shift+A), against a
# logging fake sidecar. No Claude calls: whether a real reply "cites p. 3" is a
# human check, and the sidecar's DEBUG log of the <reader_context> block is
# covered by sidecar/tests/test_context_log.py.
#
# usage (from the worktree root):
#   tests/acceptance/m5.3.sh <outdir> [app]
#   e.g. tests/acceptance/m5.3.sh <repo>/.verify/m5.3/impl
#
# app defaults to ./build/app/agentic-reader. One app session runs
# tests/acceptance/m5.3.steps on a scratch copy of a 4-page PDF; screenshots
# and app.log go to <outdir>.
#
# The steps use fixed paths, so the fixture lives at a fixed place,
# /tmp/agentic-reader-m5.3 (recreated on every run):
#   home/                 HOME of the session. home/.config/agentic-reader.conf
#                         turns on the developer login, so the chat pane shows
#                         the input and not the first-run banner.
#   proj/attention.pdf    written by tests/acceptance/m5.3-make-pdf.py. Page 3
#                         holds the 7-line, 469-char paragraph the steps select.
#   empty/                a folder with no PDF, opened as a project at the end
#                         so that no PDF is open.
#   prompts.log           every prompt the fake sidecar received, one JSON
#                         object per line (tests/acceptance/m5.3-fake-sidecar.py)
#   expected/<name>.json  {"text": ..., "context": {...} or null}
#   promptcheck           `run /tmp/agentic-reader-m5.3/promptcheck <n> <name>`:
#                         prompts.log must hold exactly n prompts, and prompt n
#                         must have expected/<name>.json's text exactly and its
#                         context: null means no context (key absent or null);
#                         an object must equal the prompt's context once its
#                         null fields are dropped. On a mismatch it prints both
#                         and fails, so shot.sh stops with
#                         "step failed: run .../promptcheck <n> <name>".
#
# The expected prompts (PARAGRAPH is page 3's paragraph, PREFILL is
# "> " + each line of its first 300 chars, then "\n\n"; FILE is
# /tmp/agentic-reader-m5.3/proj/attention.pdf):
#   1 ask-shortcut   PREFILL + "what does this mean?"
#                    context {file FILE, page 3, page_count 4, selection PARAGRAPH}
#   2 ask-menu       PREFILL + "and the last line?", the same context
#   3 excluded       "Summarize this page." context {file, page 3, page_count 4}
#   4 included-again "And now?", the full context again
#   5 no-pdf         "Is a file open?" and no context
# Pass: shot.sh exits 0 (every promptcheck passed) and every shot matches the
# comment above it in the steps file.
set -euo pipefail

[ $# -ge 1 ] || { sed -n '2,46p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
out="$1"
app="${2:-./build/app/agentic-reader}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
fix=/tmp/agentic-reader-m5.3

rm -rf "$fix"
mkdir -p "$fix/home/.config" "$fix/proj" "$fix/empty" "$fix/expected"
printf '[Agent]\nUseLocalLogin=1\n' >"$fix/home/.config/agentic-reader.conf"
python3 "$here/m5.3-make-pdf.py" "$fix/proj/attention.pdf"
python3 "$here/m5.3-make-pdf.py" --paragraph >"$fix/paragraph.txt"
: >"$fix/prompts.log"

python3 - "$fix" <<'EOF'
import json
import pathlib
import sys

fix = pathlib.Path(sys.argv[1])
paragraph = (fix / "paragraph.txt").read_text(encoding="utf-8")
cut = paragraph[:300].rstrip("\n")
prefill = "".join("> " + line + "\n" for line in cut.split("\n")) + "\n"
file = str(fix / "proj" / "attention.pdf")
full = {"file": file, "page": 3, "page_count": 4, "selection": paragraph}
without = {"file": file, "page": 3, "page_count": 4}
expected = {
    "ask-shortcut": {"text": prefill + "what does this mean?", "context": full},
    "ask-menu": {"text": prefill + "and the last line?", "context": full},
    "excluded": {"text": "Summarize this page.", "context": without},
    "included-again": {"text": "And now?", "context": full},
    "no-pdf": {"text": "Is a file open?", "context": None},
}
for name, value in expected.items():
    (fix / "expected" / f"{name}.json").write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
EOF

cat >"$fix/promptcheck" <<'EOF'
#!/usr/bin/env python3
"""promptcheck <n> <name>: prompt n of prompts.log must match expected/<name>.json."""
import json
import pathlib
import sys

fix = pathlib.Path("/tmp/agentic-reader-m5.3")
n, name = int(sys.argv[1]), sys.argv[2]
lines = [l for l in (fix / "prompts.log").read_text(encoding="utf-8").splitlines() if l.strip()]
want = json.loads((fix / "expected" / f"{name}.json").read_text(encoding="utf-8"))


def fail(why):
    print(f"promptcheck {n} {name}: {why}", file=sys.stderr)
    print("--- expected", file=sys.stderr)
    print(json.dumps(want, indent=2, ensure_ascii=False), file=sys.stderr)
    print(f"--- prompts.log ({len(lines)} prompts)", file=sys.stderr)
    for line in lines:
        print(json.dumps(json.loads(line), indent=2, ensure_ascii=False), file=sys.stderr)
    sys.exit(1)


if len(lines) != n:
    fail(f"expected {n} prompts so far, got {len(lines)}")
got = json.loads(lines[n - 1])
if got.get("text") != want["text"]:
    fail(f"text differs: got {got.get('text')!r}")
context = got.get("context")
if want["context"] is None:
    if context is not None:
        fail(f"expected no context, got {context!r}")
else:
    if not isinstance(context, dict):
        fail(f"expected a context object, got {context!r}")
    present = {k: v for k, v in context.items() if v is not None}
    if present != want["context"]:
        fail(f"context differs: got {present!r}")
print(f"promptcheck {n} {name}: ok", file=sys.stderr)
EOF
chmod +x "$fix/promptcheck"

rm -rf "$out"
status=0
"$root/scripts/shot.sh" -o "$out" -s "$here/m5.3.steps" -- \
  env HOME="$fix/home" READER_SIDECAR_CMD="$here/m5.3-fake-sidecar.py" \
  READER_FAKE_LOG="$fix/prompts.log" "$app" "$fix/proj/attention.pdf" || status=$?
cp "$fix/prompts.log" "$out/prompts.log" 2>/dev/null || true
exit "$status"
