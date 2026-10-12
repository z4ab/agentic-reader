#!/usr/bin/env bash
# M7.1 check: the API key never lands in config or log files.
#
# usage: tests/acceptance/m7.1-grep.sh PATH...
#
# Searches every file under each PATH (the config dir, the log dir, or a whole
# isolated HOME, plus the shot.sh output dir with app.log) for the first 10
# characters of the fake key used by tests/acceptance/m7.1.steps.
# Prints only the names of matching files, never their contents.
#
# Exit status: 0 no match, 1 a match, 2 bad usage or a PATH that does not exist
# (a missing dir would make the check pass without looking at anything).
set -euo pipefail

key="sk-ant-test-FAKEKEY0123456789"
prefix="${key:0:10}"

if [ $# -eq 0 ]; then
  sed -n '4,12p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 2
fi

for path in "$@"; do
  if [ ! -e "$path" ]; then
    echo "m7.1-grep: no such file or directory: $path" >&2
    exit 2
  fi
done

status=0
grep -r -a -l -F -e "$prefix" -- "$@" || status=$?
case "$status" in
  0)
    echo "m7.1-grep: FAIL, the key prefix appears in the files above" >&2
    exit 1
    ;;
  1)
    echo "m7.1-grep: OK, no match for the key prefix in: $*"
    ;;
  *)
    echo "m7.1-grep: grep failed (status $status), result unknown" >&2
    exit 2
    ;;
esac
