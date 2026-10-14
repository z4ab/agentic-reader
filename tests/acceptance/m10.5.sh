#!/usr/bin/env bash
# M10.5 acceptance: fullscreen presentation mode (F11, Esc, the find bar and Esc, Continuous,
# Ctrl+Shift+A from fullscreen, the View menu item, and Esc stopping a reply in fullscreen),
# steps in tests/acceptance/m10.5.steps.
#
# usage (from the worktree root):
#   tests/acceptance/m10.5.sh <outdir> [app]
#   e.g. tests/acceptance/m10.5.sh <repo>/.verify/m10.5/tester
#
# app defaults to ./build/app/agentic-reader. The deck is copied into a saved project,
# /tmp/agentic-reader-m10.5/proj, so the chat pane has its input. The HOME of the run
# turns on the developer login. The sidecar is the slow fake of M9.1 (40 pieces, 0.5 s
# apart), so a reply is still running when F11 and Esc are pressed. No Claude calls, no
# network. The session bus is unreachable, so no keyring is touched.
#
# Screenshots go to <outdir>. Each shot's comment in m10.5.steps says what it must show.
set -euo pipefail

[ $# -ge 1 ] || { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
out="$1"
app="${2:-./build/app/agentic-reader}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
fix=/tmp/agentic-reader-m10.5
fake="$here/m9.1-fake-sidecar.sh"

rm -rf "$fix"
mkdir -p "$fix/home/.config" "$fix/proj/.reader"
printf '[Agent]\nUseLocalLogin=1\n' >"$fix/home/.config/agentic-reader.conf"
cp "$root/tests/data/slides-16x9.pdf" "$fix/proj/slides-16x9.pdf"

mkdir -p "$out"
rm -f "$out"/[0-9][0-9]-*.png

"$root/scripts/shot.sh" -o "$out" -s "$here/m10.5.steps" -- \
  env HOME="$fix/home" DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
  READER_SIDECAR_CMD="sh $fake" \
  "$app" "$fix/proj/slides-16x9.pdf" || true

last="$(ls "$out"/*-esc-stopped-then-f11.png 2>/dev/null | head -n 1 || true)"
[ -n "$last" ] || { echo "m10.5.sh: FAIL run did not reach its last shot" >&2; exit 1; }
echo "m10.5.sh: all shots taken, check each against its comment in m10.5.steps"
