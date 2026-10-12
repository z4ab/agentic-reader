#!/usr/bin/env bash
# M6.5 acceptance: each PDF of a saved project reopens at the page and zoom
# where the user left it. Two app sessions share one scratch HOME and talk to
# the logging fake sidecar of M6.2 (tests/acceptance/m6.2-fake-sidecar.sh).
#
# usage (from the worktree root):
#   tests/acceptance/m6.5.sh <outdir> [app]
#   e.g. tests/acceptance/m6.5.sh <repo>/.verify/m6.5/impl
#
# app defaults to ./build/app/agentic-reader. Screenshots go to
# <outdir>/run1 (tests/acceptance/m6.5.steps, app started with
# /tmp/agentic-reader-m6.5/proj/long.pdf) and <outdir>/run2
# (tests/acceptance/m6.5-restart.steps, no file on the command line).
#
# The steps use absolute paths, so the fixture lives at a fixed place,
# /tmp/agentic-reader-m6.5 (F below):
#   F/proj/.reader/          empty: proj is a saved project with no
#                            project.json yet
#   F/proj/long.pdf          tests/data/long.pdf (520 letter pages,
#                            "Page N of 520" heading)
#   F/proj/book/other.pdf    12 pages of 300 x 500 pt, "Chapter N of 12"
#                            heading, blue border (m6.5-state.py make-pdf)
#   F/clamp/.reader/         saved project, project.json written by a run
#                            step in run 2
#   F/clamp/short.pdf        a copy of other.pdf (12 pages)
#   F/blank/blank.pdf        tests/data/sample.pdf, no .reader/ (dropped as a
#                            folder in run 2, becomes a saved project with no
#                            open_file)
#   F/bad/.reader            a regular FILE, so Project::open fails there
#   F/bad/bad.pdf            tests/data/sample.pdf
#   F/src/added.pdf          tests/data/sample.pdf, copied into proj in run 2
#   F/home/                  HOME of both runs (wxConfig lives here)
#   F/state                  tests/acceptance/m6.5-state.py (project.json
#                            checks and edits for the run steps)
#   F/xdnd-drop              built from tests/acceptance/xdnd_drop.c (needs cc
#                            and the libX11 headers)
#   F/fake-run1.log, F/fake-run2.log   what the fake sidecar received
#
# Run 1 ends with Ctrl+Q so the app saves its state the normal way. shot.sh
# then stops with "app exited during step: ..." and exit status 1. That is the
# expected end of run 1; this script checks that the last shot exists instead.
#
# After each run the script checks the .reader/ state and prints
# "m6.5.sh: FAIL ..." and exits 1 on the first mismatch:
#   run 1: proj/.reader/project.json has open_file "book/other.pdf",
#          long.pdf at last_page 39 (p. 40) with a custom zoom 0.75, and
#          book/other.pdf at last_page 4 (p. 5) with "fit-width". p. 5 was
#          entered right before Ctrl+Q, so only the save at close records it.
#   run 2: proj still has long.pdf 39 / 0.75 and book/other.pdf 4 /
#          fit-width, and open_file "long.pdf". clamp/.reader/project.json has
#          short.pdf at the clamped last_page 11 with "fit-page" (saved when
#          the app left clamp). bad/.reader is still a regular file and no
#          .reader/ appeared anywhere else.
# Pass: both checks print "ok", run 2's shot.sh exits 0, and every shot
# matches the comment above it in the steps files.
set -euo pipefail

[ $# -ge 1 ] || { sed -n '2,50p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
out="$1"
app="${2:-./build/app/agentic-reader}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
fix=/tmp/agentic-reader-m6.5
fake="$here/m6.2-fake-sidecar.sh"
state="$fix/state"

fail() {
  echo "m6.5.sh: FAIL $*" >&2
  exit 1
}

check() { # what, then the state command
  local what="$1"; shift
  "$state" "$@" || fail "$what"
}

check_reader_dirs() {
  local got want
  got="$(find "$fix/proj" "$fix/clamp" "$fix/blank" "$fix/bad" -name .reader -type d | sort)"
  want="$(printf '%s\n' "$@" | sort)"
  [ "$got" = "$want" ] || fail ".reader/ folders are:
$got
expected:
$want"
  [ -f "$fix/bad/.reader" ] || fail "$fix/bad/.reader is no longer a regular file"
}

rm -rf "$fix"
mkdir -p "$fix/home" "$fix/src" "$fix/proj/.reader" "$fix/proj/book" "$fix/clamp/.reader" \
  "$fix/blank" "$fix/bad"
data="$root/tests/data"
cp "$here/m6.5-state.py" "$state"
chmod +x "$state"
cp "$data/long.pdf" "$fix/proj/long.pdf"
"$state" make-pdf "$fix/proj/book/other.pdf" 12 300 500
cp "$fix/proj/book/other.pdf" "$fix/clamp/short.pdf"
cp "$data/sample.pdf" "$fix/blank/blank.pdf"
cp "$data/sample.pdf" "$fix/bad/bad.pdf"
printf 'not a folder\n' >"$fix/bad/.reader"
cp "$data/sample.pdf" "$fix/src/added.pdf"
cc -O2 -o "$fix/xdnd-drop" "$here/xdnd_drop.c" -lX11

rm -rf "$out/run1" "$out/run2"

echo "== run 1: positions of two files, debounce, switch, quit"
"$root/scripts/shot.sh" -o "$out/run1" -s "$here/m6.5.steps" -- \
  env HOME="$fix/home" READER_SIDECAR_CMD="$fake" READER_FAKE_LOG="$fix/fake-run1.log" \
  "$app" "$fix/proj/long.pdf" || true
last="$(ls "$out/run1"/*-other-restored.png 2>/dev/null || true)"
[ -n "$last" ] || fail "run 1 did not reach its last shot"

check "run 1: open_file" check-open "$fix/proj" book/other.pdf
check "run 1: long.pdf position" check "$fix/proj" long.pdf 39 0.75
check "run 1: other.pdf position saved at quit" check "$fix/proj" book/other.pdf 4 fit-width
check_reader_dirs "$fix/proj/.reader" "$fix/clamp/.reader"
echo "m6.5.sh: run 1 checks ok"

echo "== run 2: restart restores both, clamp, tree state, closeFile, failed open"
"$root/scripts/shot.sh" -o "$out/run2" -s "$here/m6.5-restart.steps" -- \
  env HOME="$fix/home" READER_SIDECAR_CMD="$fake" READER_FAKE_LOG="$fix/fake-run2.log" \
  "$app"

check "run 2: proj open_file" check-open "$fix/proj" long.pdf
check "run 2: long.pdf position" check "$fix/proj" long.pdf 39 0.75
check "run 2: other.pdf position" check "$fix/proj" book/other.pdf 4 fit-width
check "run 2: clamped short.pdf position" check "$fix/clamp" short.pdf 11 fit-page
check_reader_dirs "$fix/proj/.reader" "$fix/clamp/.reader" "$fix/blank/.reader"
echo "m6.5.sh: run 2 checks ok"
