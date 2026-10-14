#!/usr/bin/env bash
# M10.2 acceptance: the layout mode of a slide deck and of a portrait PDF, with
# the switches, the View menu, and the per-file layout kept in project.json
# across a restart. Three app sessions share one scratch HOME and talk to the
# logging fake sidecar of M6.2 (tests/acceptance/m6.2-fake-sidecar.sh).
#
# usage (from the worktree root):
#   tests/acceptance/m10.2.sh <outdir> [app]
#   e.g. tests/acceptance/m10.2.sh <repo>/.verify/m10.2/impl
#
# app defaults to ./build/app/agentic-reader. Screenshots go to
# <outdir>/run1 (tests/acceptance/m10.2.steps, app started with
# tests/data/slides-16x9.pdf), <outdir>/run2 (m10.2-project.steps, app started
# with /tmp/agentic-reader-m10.2/proj/sample.pdf) and <outdir>/run3
# (m10.2-restart.steps, no file on the command line).
#
# The steps use absolute paths, so the fixture lives at a fixed place,
# /tmp/agentic-reader-m10.2 (F below):
#   F/proj/.reader/          empty: proj is a saved project with no project.json
#   F/proj/sample.pdf        tests/data/sample.pdf (portrait Letter, 3 pages)
#   F/home/                  HOME of all runs (wxConfig lives here)
#   F/state                  tests/acceptance/m10.2-state.py (project.json checks)
#   F/fake-run*.log          what the fake sidecar received
#
# Runs 2 and 3 end with Ctrl+Q so the app saves its state the normal way. shot.sh
# then stops with "app exited during step: ..." and exit status 1. That is the
# expected end of run 2; this script checks that its last shot exists instead.
#
# After each run the script checks .reader/project.json and prints
# "m10.2.sh: FAIL ..." and exits 1 on the first mismatch:
#   run 1: nothing is written for tests/data/slides-16x9.pdf (a file given on the
#          command line is not a saved project), so tests/data/.reader does not
#          exist afterwards.
#   run 2: proj/.reader/project.json has open_file "sample.pdf", and
#          files["sample.pdf"] has last_page 1 and layout "single", both after
#          the mid-run check and after the quit.
#   run 3: files["sample.pdf"] has last_page 1 and no layout key, after the
#          mid-run check. Nothing is written after that, so the file as the app
#          left it is still checked at the end.
#   Each run's last shot exists, and no .reader/ appeared anywhere but proj.
# Pass: all checks print "ok", and every shot matches the comment above it in
# the steps files.
set -euo pipefail

[ $# -ge 1 ] || { sed -n '2,46p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
out="$1"
app="${2:-./build/app/agentic-reader}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
fix=/tmp/agentic-reader-m10.2
fake="$here/m6.2-fake-sidecar.sh"
state="$fix/state"

fail() {
  echo "m10.2.sh: FAIL $*" >&2
  exit 1
}

check() { # what, then the state command
  local what="$1"; shift
  "$state" "$@" || fail "$what"
}

check_reader_dirs() {
  local got want
  got="$(find "$fix" "$root/tests/data" -name .reader -type d | sort)"
  want="$fix/proj/.reader"
  [ "$got" = "$want" ] || fail ".reader/ folders are:
$got
expected only:
$want"
}

rm -rf "$fix"
mkdir -p "$fix/home" "$fix/proj/.reader"
cp "$here/m10.2-state.py" "$state"
chmod +x "$state"
cp "$root/tests/data/sample.pdf" "$fix/proj/sample.pdf"

rm -rf "$out/run1" "$out/run2" "$out/run3"

echo "== run 1: slides-16x9.pdf in Single Page, Ctrl+Shift+L both ways, View menu"
"$root/scripts/shot.sh" -o "$out/run1" -s "$here/m10.2.steps" -- \
  env HOME="$fix/home" READER_SIDECAR_CMD="$fake" READER_FAKE_LOG="$fix/fake-run1.log" \
  "$app" "$root/tests/data/slides-16x9.pdf"
[ -n "$(ls "$out/run1"/*-view-menu-closed.png 2>/dev/null || true)" ] || fail "run 1 did not reach its last shot"
check_reader_dirs
echo "m10.2.sh: run 1 checks ok"

echo "== run 2: sample.pdf in a saved project: Continuous, Single Page, page 2, quit"
"$root/scripts/shot.sh" -o "$out/run2" -s "$here/m10.2-project.steps" -- \
  env HOME="$fix/home" READER_SIDECAR_CMD="$fake" READER_FAKE_LOG="$fix/fake-run2.log" \
  "$app" "$fix/proj/sample.pdf" || true
[ -n "$(ls "$out/run2"/*-sample-single-p2.png 2>/dev/null || true)" ] || fail "run 2 did not reach its last shot"

check "run 2: open_file" check-open "$fix/proj" sample.pdf
check "run 2: sample.pdf position and layout after quit" check "$fix/proj" sample.pdf 1 single
check_reader_dirs
echo "m10.2.sh: run 2 checks ok"

echo "== run 3: restart in Single Page on page 2, switch back, layout removed"
"$root/scripts/shot.sh" -o "$out/run3" -s "$here/m10.2-restart.steps" -- \
  env HOME="$fix/home" READER_SIDECAR_CMD="$fake" READER_FAKE_LOG="$fix/fake-run3.log" \
  "$app"

check "run 3: open_file" check-open "$fix/proj" sample.pdf
check "run 3: sample.pdf position, no layout" check "$fix/proj" sample.pdf 1 absent
check_reader_dirs
echo "m10.2.sh: run 3 checks ok"
