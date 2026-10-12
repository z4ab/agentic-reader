#!/usr/bin/env bash
# M6.2 acceptance: open a folder as a project, browse and open its PDFs, see
# changes made outside the app, an unsaved project and "Save as project", then
# restart and get the same project and file back. Two app sessions share one
# scratch HOME and talk to a logging fake sidecar.
#
# usage (from the worktree root):
#   tests/acceptance/m6.2.sh <outdir> [app]
#   e.g. tests/acceptance/m6.2.sh <repo>/.verify/m6.2/impl
#
# app defaults to ./build/app/agentic-reader. Screenshots go to
# <outdir>/run1 (tests/acceptance/m6.2.steps) and <outdir>/run2
# (tests/acceptance/m6.2-restart.steps).
#
# The steps type absolute paths, so the fixture lives at a fixed place,
# /tmp/agentic-reader-m6.2 (layout in m6.2.steps). Also there:
#   home/            HOME of both runs (wxConfig lives here)
#   fake-run1.log    what the fake sidecar received in run 1
#   fake-run2.log    the same for run 2
#   xdnd-drop        built from tests/acceptance/xdnd_drop.c (needs cc and
#                    the libX11 headers)
#
# Run 1 ends with Ctrl+Q so the app saves its state the normal way. shot.sh
# then stops with "app exited during step: ..." and exit status 1. That is the
# expected end of run 1; this script checks that the last shot exists instead.
#
# After each run the script checks the sidecar log and the .reader/ folders
# and prints "m6.2.sh: FAIL ..." and exits 1 on the first mismatch:
#   run 1: open_project went to proj, then loose, then proj (repeats of the
#          same path in a row count once), and to no other folder.
#          proj/.reader/project.json has open_file "papers/long.pdf",
#          loose/.reader/project.json has open_file "single.pdf". No other
#          .reader/ exists under proj or loose.
#   run 2: open_project went to proj, then loose, then proj, and nowhere else.
#          Still no other .reader/.
# Pass: both checks print "ok", run 2's shot.sh exits 0, and every shot
# matches the comment above it in the steps files.
set -euo pipefail

[ $# -ge 1 ] || { sed -n '2,36p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
out="$1"
app="${2:-./build/app/agentic-reader}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
fix=/tmp/agentic-reader-m6.2
fake="$here/m6.2-fake-sidecar.sh"

fail() {
  echo "m6.2.sh: FAIL $*" >&2
  exit 1
}

# open_project paths from a fake log, repeats in a row collapsed, one per line.
projects_sent() {
  sed -n 's/^open_project //p' "$1" | uniq
}

check_projects() { # log expected...
  local log="$1"; shift
  [ -f "$log" ] || fail "no fake sidecar log at $log"
  local got want
  got="$(projects_sent "$log")"
  want="$(printf '%s\n' "$@")"
  [ "$got" = "$want" ] || fail "open_project in $log was:
$got
expected:
$want"
}

check_open_file() { # project rel
  local file="$1/.reader/project.json"
  [ -f "$file" ] || fail "missing $file"
  grep -Eq "\"open_file\"[[:space:]]*:[[:space:]]*\"$2\"" "$file" \
    || fail "$file does not have open_file \"$2\": $(cat "$file")"
}

check_reader_dirs() {
  local got want
  got="$(find "$fix/proj" "$fix/loose" -name .reader | sort)"
  want="$(printf '%s\n' "$fix/loose/.reader" "$fix/proj/.reader")"
  [ "$got" = "$want" ] || fail ".reader/ folders are:
$got
expected:
$want"
}

rm -rf "$fix"
mkdir -p "$fix/home" "$fix/src" "$fix/proj/papers/deep" "$fix/proj/empty" \
  "$fix/proj/misc" "$fix/proj/.hidden" "$fix/loose"
data="$root/tests/data"
cp "$data/sample.pdf" "$fix/src/sample.pdf"
printf 'not a pdf\n' >"$fix/src/notes.txt"
cp "$data/sample.pdf" "$fix/proj/alpha.pdf"
cp "$data/sample.pdf" "$fix/proj/beta.PDF"
printf 'notes\n' >"$fix/proj/notes.txt"
printf '# readme\n' >"$fix/proj/readme.md"
cp "$data/long.pdf" "$fix/proj/papers/long.pdf"
printf 'a,b\n1,2\n' >"$fix/proj/papers/data.csv"
cp "$data/color.pdf" "$fix/proj/papers/deep/nested.pdf"
printf 'todo\n' >"$fix/proj/misc/todo.txt"
cp "$data/sample.pdf" "$fix/proj/.hidden/secret.pdf"
cp "$data/sample.pdf" "$fix/loose/single.pdf"
printf 'other\n' >"$fix/loose/other.txt"
cc -O2 -o "$fix/xdnd-drop" "$here/xdnd_drop.c" -lX11

rm -rf "$out/run1" "$out/run2"

echo "== run 1: Open Project, tree, refresh, unsaved project, Recent Projects"
"$root/scripts/shot.sh" -o "$out/run1" -s "$here/m6.2.steps" -- \
  env HOME="$fix/home" READER_SIDECAR_CMD="$fake" READER_FAKE_LOG="$fix/fake-run1.log" \
  "$app" || true
last="$(ls "$out/run1"/*-tree-open-long.png 2>/dev/null || true)"
[ -n "$last" ] || fail "run 1 did not reach its last shot"

check_projects "$fix/fake-run1.log" "$fix/proj" "$fix/loose" "$fix/proj"
check_open_file "$fix/proj" "papers/long.pdf"
check_open_file "$fix/loose" "single.pdf"
check_reader_dirs
echo "m6.2.sh: run 1 checks ok"

echo "== run 2: restart with the same HOME"
"$root/scripts/shot.sh" -o "$out/run2" -s "$here/m6.2-restart.steps" -- \
  env HOME="$fix/home" READER_SIDECAR_CMD="$fake" READER_FAKE_LOG="$fix/fake-run2.log" \
  "$app"

check_projects "$fix/fake-run2.log" "$fix/proj" "$fix/loose" "$fix/proj"
check_reader_dirs
echo "m6.2.sh: run 2 checks ok"
