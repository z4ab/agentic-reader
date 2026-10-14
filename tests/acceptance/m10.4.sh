#!/usr/bin/env bash
# M10.4 acceptance: selection and find in Single Page on a slide deck, steps in
# tests/acceptance/m10.4.steps. The clipboard is read after each Ctrl+C, as in M3.2
# (the expected texts are exact bytes).
#
# usage (from the worktree root):
#   tests/acceptance/m10.4.sh <outdir> [app]
#   e.g. tests/acceptance/m10.4.sh <repo>/.verify/m10.4/tester
#
# app defaults to ./build/app/agentic-reader. The deck is opened on its own, with a
# scratch HOME. The session bus is unreachable, so no keyring is touched.
#
# The steps call fixed paths, so the fixture lives at a fixed place:
#   /tmp/agentic-reader-m10.4/clip-read   built from tests/acceptance/clip_read.c
#                                         (needs cc and the libX11 headers)
#   /tmp/agentic-reader-m10.4/clipcheck   `run .../clipcheck <name>` reads the clipboard
#                                         into clip/<name>.txt and compares it byte for
#                                         byte with expected/<name>.txt. On a mismatch
#                                         (or an empty clipboard) it fails, so shot.sh
#                                         stops with "step failed: run .../clipcheck <name>".
#   /tmp/agentic-reader-m10.4/expected/   the expected texts, written below
#   /tmp/agentic-reader-m10.4/home/       HOME of the session
# The script exits 1 unless the run reached its last shot.
set -uo pipefail

[ $# -ge 1 ] || { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
out="$1"
app="${2:-./build/app/agentic-reader}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
fix=/tmp/agentic-reader-m10.4

rm -rf "$fix"
mkdir -p "$fix/home" "$fix/expected" "$fix/clip"
cc -O2 -o "$fix/clip-read" "$here/clip_read.c" -lX11 || exit 1

cat >"$fix/clipcheck" <<'EOF'
#!/usr/bin/env bash
# clipcheck <name>: the clipboard must equal expected/<name>.txt exactly.
set -u
fix=/tmp/agentic-reader-m10.4
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

# slides-16x9.pdf (m10.4.steps)
expect word-distinct 'distinct'
expect slide-3-line-from-slide-4 'Point charlie: distinct body text for slide 3.'

status=0
"$root/scripts/shot.sh" -o "$out" -s "$here/m10.4.steps" -- \
  env HOME="$fix/home" DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
  "$app" "$root/tests/data/slides-16x9.pdf" || status=1

last="$(ls "$out"/*-continuous-shift-enter-wraps-scrolls-to-slide-8.png 2>/dev/null | head -n 1 || true)"
if [ -z "$last" ]; then
  echo "m10.4.sh: FAIL run did not reach its last shot" >&2
  exit 1
fi
[ "$status" -eq 0 ] || { echo "m10.4.sh: FAIL shot.sh stopped early (see its message above)" >&2; exit 1; }
echo "m10.4.sh: all shots taken, check each against its comment in m10.4.steps"
