#!/usr/bin/env bash
# M6.4 acceptance: the chat list, chats that survive a restart, resume,
# delete, a failed resume with "Continue in new chat", a chat whose first
# reply failed, the resume after a sidecar crash, and an unsaved project that
# keeps its chats in memory only. Three app sessions share one scratch HOME
# and talk to a logging fake sidecar (tests/acceptance/m6.4-fake-sidecar.py).
#
# usage (from the worktree root):
#   tests/acceptance/m6.4.sh <outdir> [app]
#   e.g. tests/acceptance/m6.4.sh <repo>/.verify/m6.4/impl
#
# app defaults to ./build/app/agentic-reader. Screenshots go to
#   <outdir>/run1  tests/acceptance/m6.4.steps          saved project, chats
#   <outdir>/run2  tests/acceptance/m6.4-restart.steps  restart and resume
#   <outdir>/run3  tests/acceptance/m6.4-unsaved.steps  unsaved project
#
# Fixture, at a fixed place F=/tmp/agentic-reader-m6.4:
#   proj/paper.pdf        tests/data/sample.pdf (3 pages, "Page One")
#   proj/.reader/         empty, so proj is a saved project with no chats
#   loose/single.pdf      sample.pdf in a folder without .reader/ (unsaved)
#   home/                 HOME of all runs. home/.config/agentic-reader.conf
#                         turns on the developer login ([Agent]
#                         UseLocalLogin=1), so the first-run banner does not
#                         replace the chat input. The session bus is
#                         unreachable, so no keyring is touched.
#   state/                the fake's session memory (READER_FAKE_STATE)
#   fake-run<N>.log       what the fake received and did in run N
#
# Each run ends with Ctrl+Q so the app saves its state the normal way.
# shot.sh then stops with "app exited during step: ..." and exit status 1.
# That is the expected end; this script checks the run's last shot exists.
#
# After each run tests/acceptance/m6.4-check.py checks the fake's log,
# project.json and the JSONL transcripts (see its run1/run2/run3), prints
# "m6.4-check: runN ok" or "m6.4-check: FAIL ..." and this script stops at
# the first failure. Pass: all three checks print "ok" and every shot matches
# the comment above it in the steps files.
#
# The live check (a real follow-up question answered from the earlier
# conversation after a restart) needs a Claude login: it is a human check.
set -euo pipefail

[ $# -ge 1 ] || { sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
out="$1"
app="${2:-./build/app/agentic-reader}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
fix=/tmp/agentic-reader-m6.4
fake="$here/m6.4-fake-sidecar.py"
check="$here/m6.4-check.py"

fail() {
  echo "m6.4.sh: FAIL $*" >&2
  exit 1
}

rm -rf "$fix"
mkdir -p "$fix/proj/.reader" "$fix/loose" "$fix/home/.config" "$fix/state"
cp "$root/tests/data/sample.pdf" "$fix/proj/paper.pdf"
cp "$root/tests/data/sample.pdf" "$fix/loose/single.pdf"
printf '[Agent]\nUseLocalLogin=1\n' >"$fix/home/.config/agentic-reader.conf"

rm -rf "$out/run1" "$out/run2" "$out/run3"

run() { # n steps pdf last-shot
  local n="$1" steps="$2" pdf="$3" last="$4"
  "$root/scripts/shot.sh" -o "$out/run$n" -s "$here/$steps" -- \
    env HOME="$fix/home" DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
    READER_SIDECAR_CMD="$fake" READER_FAKE_LOG="$fix/fake-run$n.log" \
    READER_FAKE_STATE="$fix/state" \
    "$app" "$pdf" || true
  ls "$out/run$n"/*-"$last".png >/dev/null 2>&1 || fail "run $n did not reach its last shot ($last)"
}

echo "== run 1: chat list, persistence, rename, delete (saved project)"
run 1 m6.4.steps "$fix/proj/paper.pdf" list-after-delete
python3 -I "$check" run1 "$fix"

echo "== run 2: restart, resume, cannot resume, sidecar crash"
run 2 m6.4-restart.steps "$fix/proj/paper.pdf" alpha-after-restart
python3 -I "$check" run2 "$fix"

echo "== run 3: unsaved project"
run 3 m6.4-unsaved.steps "$fix/loose/single.pdf" unsaved-deleted
python3 -I "$check" run3 "$fix"

echo "m6.4.sh: all checks ok"
