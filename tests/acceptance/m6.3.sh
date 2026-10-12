#!/usr/bin/env bash
# M6.3 acceptance: Project > Instructions... edits <root>/CLAUDE.md. With no
# project the item is disabled; in a saved project the dialog loads the file,
# Cancel discards an edit, Save writes it and shows the "applies to new
# chats" note; in an unsaved project the dialog starts empty and Save creates
# CLAUDE.md without creating .reader/. One app run on a scratch HOME, talking
# to the logging fake sidecar of M6.2 (tests/acceptance/m6.2-fake-sidecar.sh).
#
# usage (from the worktree root):
#   tests/acceptance/m6.3.sh <outdir> [app]
#   e.g. tests/acceptance/m6.3.sh <repo>/.verify/m6.3/impl
#
# app defaults to ./build/app/agentic-reader. Screenshots go to <outdir>
# (tests/acceptance/m6.3.steps); old NN-*.png there are removed first.
#
# The steps type absolute paths and compare files at absolute paths, so the
# fixture lives at a fixed place, /tmp/agentic-reader-m6.3 (layout in
# m6.3.steps). Also there:
#   home/       HOME of the run (wxConfig lives here)
#   fake.log    what the fake sidecar received
#
# After the run the script prints "m6.3.sh: FAIL ..." and exits 1 on the
# first mismatch:
#   - proj/CLAUDE.md is expect/proj-saved.md byte for byte, and
#     loose/CLAUDE.md is expect/loose-saved.md byte for byte.
#   - proj holds exactly .reader, CLAUDE.md and paper.pdf; loose holds
#     exactly CLAUDE.md and single.pdf (no temporary file, and no .reader/
#     in the unsaved project).
#   - The sidecar is told nothing about the instructions: open_project went
#     to proj, then loose (repeats of the same path in a row count once), and
#     the log holds no message types other than open_project, new_chat,
#     resume_chat and eof (no prompt, no interrupt, no restart of a chat).
# Pass: shot.sh exits 0, the checks print "ok", and every shot matches the
# comment above it in m6.3.steps.
set -euo pipefail

[ $# -ge 1 ] || { sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
out="$1"
app="${2:-./build/app/agentic-reader}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
fix=/tmp/agentic-reader-m6.3
fake="$here/m6.2-fake-sidecar.sh"

fail() {
  echo "m6.3.sh: FAIL $*" >&2
  exit 1
}

same_file() { # actual expected
  [ -f "$1" ] || fail "missing $1"
  cmp -s "$1" "$2" || fail "$1 differs from $2:
$(od -c "$1" | head -n 20)"
}

same_entries() { # dir names...
  local got want
  got="$(ls -A "$1" | sort)"
  want="$(shift; printf '%s\n' "$@" | sort)"
  [ "$got" = "$want" ] || fail "$1 holds:
$got
expected:
$want"
}

rm -rf "$fix"
mkdir -p "$fix/home" "$fix/proj/.reader" "$fix/loose" "$fix/expect"
data="$root/tests/data"
cp "$data/sample.pdf" "$fix/proj/paper.pdf"
cp "$data/sample.pdf" "$fix/loose/single.pdf"
# UTF-8 (e acute, em dash) and two lines whose '|' line up only in a
# monospace font. Ends with a newline.
printf '# Notes du projet\nCitez les num\303\251ros de page \342\200\224 toujours.\niiiiiiiiii|\nMMMMMMMMMM|\n' \
  >"$fix/proj/CLAUDE.md"
cp "$fix/proj/CLAUDE.md" "$fix/expect/proj-original.md"
cp "$fix/proj/CLAUDE.md" "$fix/expect/proj-saved.md"
printf 'Always answer in French.\n' >>"$fix/expect/proj-saved.md"
printf 'Answer briefly.' >"$fix/expect/loose-saved.md"

mkdir -p "$out"
rm -f "$out"/[0-9][0-9]-*.png

echo "== run: Project > Instructions... with no project, a saved and an unsaved project"
"$root/scripts/shot.sh" -o "$out" -s "$here/m6.3.steps" -- \
  env HOME="$fix/home" READER_SIDECAR_CMD="$fake" READER_FAKE_LOG="$fix/fake.log" \
  "$app"

same_file "$fix/proj/CLAUDE.md" "$fix/expect/proj-saved.md"
same_file "$fix/loose/CLAUDE.md" "$fix/expect/loose-saved.md"
same_entries "$fix/proj" .reader CLAUDE.md paper.pdf
same_entries "$fix/loose" CLAUDE.md single.pdf

log="$fix/fake.log"
[ -f "$log" ] || fail "no fake sidecar log at $log"
got="$(sed -n 's/^open_project //p' "$log" | uniq)"
want="$(printf '%s\n' "$fix/proj" "$fix/loose")"
[ "$got" = "$want" ] || fail "open_project in $log was:
$got
expected:
$want"
other="$(grep -Ev '^(open_project .*|new_chat|resume_chat|eof)$' "$log" || true)"
[ -z "$other" ] || fail "the sidecar got unexpected messages:
$other"
echo "m6.3.sh: checks ok"
