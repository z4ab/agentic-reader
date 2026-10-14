#!/usr/bin/env bash
# M10.3 acceptance: Single Page navigation on a slide deck (keys, wheel, Previous and
# Next buttons, and the keys in Continuous and in the chat input), steps in
# tests/acceptance/m10.3.steps.
#
# usage (from the worktree root):
#   tests/acceptance/m10.3.sh <outdir> [app]
#   e.g. tests/acceptance/m10.3.sh <repo>/.verify/m10.3/tester
#
# app defaults to ./build/app/agentic-reader. The deck is copied into a saved project,
# /tmp/agentic-reader-m10.3/proj, so the chat pane has its input. The HOME of the run
# turns on the developer login, and the sidecar is the logging fake of M6.2. No Claude
# calls, no network. The session bus is unreachable, so no keyring is touched.
#
# Screenshots go to <outdir>. Each shot's comment in m10.3.steps says what it must show.
set -euo pipefail

[ $# -ge 1 ] || { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
out="$1"
app="${2:-./build/app/agentic-reader}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
fix=/tmp/agentic-reader-m10.3
fake="$here/m6.2-fake-sidecar.sh"

rm -rf "$fix"
mkdir -p "$fix/home/.config" "$fix/proj/.reader"
printf '[Agent]\nUseLocalLogin=1\n' >"$fix/home/.config/agentic-reader.conf"
cp "$root/tests/data/slides-16x9.pdf" "$fix/proj/slides-16x9.pdf"

mkdir -p "$out"
rm -f "$out"/[0-9][0-9]-*.png

"$root/scripts/shot.sh" -o "$out" -s "$here/m10.3.steps" -- \
  env HOME="$fix/home" DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
  READER_SIDECAR_CMD="$fake" READER_FAKE_LOG="$fix/fake.log" \
  "$app" "$fix/proj/slides-16x9.pdf" || true

last="$(ls "$out"/*-chat-input-no-flip.png 2>/dev/null | head -n 1 || true)"
[ -n "$last" ] || { echo "m10.3.sh: FAIL run did not reach its last shot" >&2; exit 1; }
echo "m10.3.sh: all shots taken, check each against its comment in m10.3.steps"
