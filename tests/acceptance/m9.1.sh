#!/usr/bin/env bash
# M9.1 acceptance: the Chat menu, Ctrl+N, Ctrl+L, Esc (stop, idle, and deny in
# a permission dialog) against the slow fake sidecar
# (tests/acceptance/m9.1-fake-sidecar.sh). No Claude calls, no network.
#
# usage (from the worktree root):
#   tests/acceptance/m9.1.sh <outdir> [app]
#   e.g. tests/acceptance/m9.1.sh <repo>/.verify/m9.1/tester
#
# app defaults to ./build/app/agentic-reader. Fixture at /tmp/agentic-reader-m9.1:
#   proj/paper.pdf      tests/data/sample.pdf (3 pages)
#   proj/.reader/       empty, so proj is a saved project with no chats
#   home/.config/       agentic-reader.conf turns on the developer login, so the
#                       first-run banner does not replace the chat input.
#                       The session bus is unreachable, so no keyring is touched.
#
# Steps: tests/acceptance/m9.1.steps. Each shot's comment says what it must show.
set -euo pipefail

[ $# -ge 1 ] || { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
out="$1"
app="${2:-./build/app/agentic-reader}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
fix=/tmp/agentic-reader-m9.1

rm -rf "$fix"
mkdir -p "$fix/proj/.reader" "$fix/home/.config" "$fix/state"
cp "$root/tests/data/sample.pdf" "$fix/proj/paper.pdf"
printf '[Agent]\nUseLocalLogin=1\n' >"$fix/home/.config/agentic-reader.conf"

"$root/scripts/shot.sh" -o "$out" -s "$here/m9.1.steps" -- \
  env HOME="$fix/home" DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
  READER_SIDECAR_CMD="sh $here/m9.1-fake-sidecar.sh" \
  "$app" "$fix/proj/paper.pdf" || true

last="$(ls "$out"/*-permission-denied.png 2>/dev/null | head -n 1 || true)"
[ -n "$last" ] || { echo "m9.1.sh: FAIL run did not reach its last shot" >&2; exit 1; }
echo "m9.1.sh: all shots taken, check each against its comment in m9.1.steps"
