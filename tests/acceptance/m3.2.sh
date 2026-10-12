#!/usr/bin/env bash
# M3.2 acceptance: text selection with the mouse, the highlight, Ctrl+C and the
# context menu "Copy", checked by reading the X clipboard after each copy (the
# automated form of "select a paragraph and paste it into a text editor").
#
# usage (from the worktree root):
#   tests/acceptance/m3.2.sh <outdir> [app]
#   e.g. tests/acceptance/m3.2.sh <repo>/.verify/m3.2/impl
#
# app defaults to ./build/app/agentic-reader. Three app sessions, one per PDF:
#   <outdir>/sample  tests/acceptance/m3.2.steps         sample.pdf (one column, 3 pages)
#   <outdir>/twocol  tests/acceptance/m3.2-twocol.steps  twocol.pdf (two columns)
#   <outdir>/utf8    tests/acceptance/m3.2-utf8.steps    utf8.pdf (non-ASCII words)
# Each session's clipboard reads are copied to <outdir>/<session>/clip/.
#
# The steps call fixed paths, so the fixture lives at a fixed place:
#   /tmp/agentic-reader-m3.2/clip-read   built from tests/acceptance/clip_read.c
#                                        (needs cc and the libX11 headers; xclip and
#                                        xsel are not installed here)
#   /tmp/agentic-reader-m3.2/clipcheck   `run .../clipcheck <name>` reads the clipboard
#                                        into clip/<name>.txt and compares it byte for
#                                        byte with expected/<name>.txt. On a mismatch
#                                        (or an empty clipboard) it prints both and
#                                        fails, so shot.sh stops with
#                                        "step failed: run .../clipcheck <name>".
#   /tmp/agentic-reader-m3.2/expected/   the expected texts, written below
#   /tmp/agentic-reader-m3.2/home/       HOME of every session
# The script runs all three sessions and exits 1 if any of them failed.
set -uo pipefail

[ $# -ge 1 ] || { sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
out="$1"
app="${2:-./build/app/agentic-reader}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
fix=/tmp/agentic-reader-m3.2

rm -rf "$fix"
mkdir -p "$fix/home" "$fix/expected" "$fix/clip"
cc -O2 -o "$fix/clip-read" "$here/clip_read.c" -lX11 || exit 1

cat >"$fix/clipcheck" <<'EOF'
#!/usr/bin/env bash
# clipcheck <name>: the clipboard must equal expected/<name>.txt exactly.
set -u
fix=/tmp/agentic-reader-m3.2
name="$1"
if ! "$fix/clip-read" >"$fix/clip/$name.txt"; then
  echo "clipcheck $name: reading the clipboard failed" >&2
  exit 1
fi
if ! cmp -s "$fix/expected/$name.txt" "$fix/clip/$name.txt"; then
  echo "clipcheck $name: the clipboard differs from the expected text" >&2
  echo "--- expected (cat -A)" >&2
  cat -A "$fix/expected/$name.txt" >&2
  echo >&2
  echo "--- clipboard (cat -A)" >&2
  cat -A "$fix/clip/$name.txt" >&2
  echo >&2
  exit 1
fi
echo "clipcheck $name: ok" >&2
EOF
chmod +x "$fix/clipcheck"

# expect <name> <text>: the exact clipboard text, no trailing newline.
expect() { printf '%s' "$2" >"$fix/expected/$1.txt"; }

# sample.pdf (m3.2.steps)
expect paragraph $'The quick brown fox jumps over the lazy dog. This sample document exists to\ntest PDF loading, rendering, and text extraction in Agentic Reader. Marker\nword: zebra.'
expect word 'brown'
expect line 'test PDF loading, rendering, and text extraction in Agentic Reader. Marker'
expect backward-menu $'quick brown fox jumps over the lazy dog. This sample document exists to\ntest PDF loading, rendering, and text extraction in Agentic Reader. Marker\nword: zebra.'
expect cross-page $'zebra.\n1\nPage Two\nAttention'

# twocol.pdf (m3.2-twocol.steps)
expect twocol-all $'Alpha starts the left column and\nits words run down a narrow strip\nof the page until the left side\ncomes to an end at omega.\nBravo opens the right column and\na reader meets it only after the\nleft column is done, so it closes\nthe page here at zulu.'
expect twocol-line-left 'comes to an end at omega.'
expect twocol-line-right 'a reader meets it only after the'
expect twocol-word 'Bravo'
expect twocol-jump $'omega.\nBravo opens'

# utf8.pdf (m3.2-utf8.steps)
expect utf8-line 'Café naïve Größe €5'
expect utf8-word 'Größe'

status=0
session() {
  local name="$1" steps="$2" pdf="$3"
  echo "== $name: $pdf"
  rm -rf "$out/$name"
  rm -f "$fix/clip/"*
  "$root/scripts/shot.sh" -o "$out/$name" -s "$here/$steps" -- \
    env HOME="$fix/home" "$app" "$root/tests/data/$pdf" || { echo "== $name FAILED"; status=1; }
  mkdir -p "$out/$name/clip"
  cp "$fix/clip/"* "$out/$name/clip/" 2>/dev/null || true
}

session sample m3.2.steps sample.pdf
session twocol m3.2-twocol.steps twocol.pdf
session utf8 m3.2-utf8.steps utf8.pdf
exit "$status"
