#!/usr/bin/env bash
# M2.5 acceptance: opening files every way, run as two app sessions that share
# one scratch HOME, so File > Recent must survive a restart.
#
# usage (from the worktree root):
#   tests/acceptance/m2.5.sh <outdir> [app]
#   e.g. tests/acceptance/m2.5.sh <repo>/.verify/m2.5/impl
#
# app defaults to ./build/app/agentic-reader. Screenshots go to
# <outdir>/run1 (tests/acceptance/m2.5.steps) and <outdir>/run2
# (tests/acceptance/m2.5-restart.steps).
#
# The steps type and drop absolute paths, so the fixture lives at a fixed place:
#   /tmp/agentic-reader-m2.5/       sample.pdf long.pdf locked.pdf not-a-pdf.pdf
#                                   doomed.pdf (a copy of sample.pdf)
#   /tmp/agentic-reader-m2.5/home/  HOME of both runs (wxConfig lives here)
#   /tmp/agentic-reader-m2.5/xdnd-drop  built from tests/acceptance/xdnd_drop.c
#                                   (needs cc and the libX11 headers)
# Between the runs this script deletes doomed.pdf, so Recent holds a missing
# file in run 2. Run 2 starts with missing.pdf, which never exists.
#
# Run 1 ends with Ctrl+Q so the app saves its config the normal way. shot.sh
# then stops with "app exited during step: ..." and exit status 1. That is the
# expected end of run 1; this script checks that the last shot exists instead.
set -euo pipefail

[ $# -ge 1 ] || { sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
out="$1"
app="${2:-./build/app/agentic-reader}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
fix=/tmp/agentic-reader-m2.5

rm -rf "$fix"
mkdir -p "$fix/home"
for f in sample long locked not-a-pdf; do
  cp "$root/tests/data/$f.pdf" "$fix/"
done
cp "$root/tests/data/sample.pdf" "$fix/doomed.pdf"
cc -O2 -o "$fix/xdnd-drop" "$here/xdnd_drop.c" -lX11

rm -rf "$out/run1" "$out/run2"

echo "== run 1: command line, Ctrl+O, password, invalid file, drop, Recent"
"$root/scripts/shot.sh" -o "$out/run1" -s "$here/m2.5.steps" -- \
  env HOME="$fix/home" "$app" "$fix/sample.pdf" || true
last="$(ls "$out/run1"/*-recent-menu.png 2>/dev/null || true)"
[ -n "$last" ] || { echo "m2.5.sh: run 1 did not reach its last shot" >&2; exit 1; }

rm -f "$fix/doomed.pdf"

echo "== run 2: restart with the same HOME, missing files"
"$root/scripts/shot.sh" -o "$out/run2" -s "$here/m2.5-restart.steps" -- \
  env HOME="$fix/home" "$app" "$fix/missing.pdf"
