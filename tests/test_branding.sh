#!/usr/bin/env bash
# M7.4 branding check (ctest `branding`): no "Claude Code" in user-facing text.
#
# usage: tests/test_branding.sh SOURCE_DIR
#
# Rules (tasks/README.md R4, task M7.4):
#
# 1. app/: `grep -rn "Claude Code" app/` may only match the developer-mode
#    login label, "Use my local Claude Code login", on lines under
#    app/src/settings/ (the dialog's check box, or a comment there quoting the
#    label). Any other occurrence on a matching line, or a match in any other
#    file, fails. The label must still match at least once, so the check is
#    not vacuous (the label is kept until the user reviews it in M8.0).
# 2. app/src: the About text "Powered by Claude." appears in at least one
#    string. The screenshot (tests/acceptance/m7.4.steps) checks the rest of
#    the About dialog.
# 3. sidecar/reader_sidecar/: sidecar strings reach the app as protocol error
#    `message` texts and the app shows them, so they count as UI strings. No
#    Python string literal there (plain or f-string part) may contain
#    "Claude Code". Ignored: comments, docstrings and other bare string
#    statements, arguments of logging calls (log.info, logger.error,
#    logging.warning, ... which go to stderr, not the app). The match is
#    case-sensitive, so identifiers such as CLAUDE_CODE_OAUTH_TOKEN are not
#    flagged. sidecar/tests/ and sidecar/PROTOCOL.md are not UI and are
#    skipped.
#
# Exit status: 0 pass, 1 a rule failed, 2 bad usage or a check could not run.
set -euo pipefail

if [ $# -ne 1 ] || [ ! -d "$1/app/src" ] || [ ! -d "$1/sidecar/reader_sidecar" ]; then
  sed -n '4,4p' "$0" | sed 's/^# \{0,1\}//' >&2
  echo "branding: SOURCE_DIR must hold app/src and sidecar/reader_sidecar" >&2
  exit 2
fi
cd "$1"

phrase="Claude Code"
label="Use my local Claude Code login"
fail=0

# Rule 1.
status=0
matches=$(grep -rn -- "$phrase" app/) || status=$?
if [ "$status" -gt 1 ]; then
  echo "branding: grep over app/ failed (status $status)" >&2
  exit 2
fi
label_hits=0
while IFS= read -r line; do
  [ -n "$line" ] || continue
  file=${line%%:*}
  rest=${line#*:}
  text=${rest#*:}
  if [[ "$file" == app/src/settings/* && "$text" == *"$label"* && "${text//"$label"/}" != *"$phrase"* ]]; then
    label_hits=$((label_hits + 1))
  else
    echo "branding: FAIL, \"$phrase\" outside the dev-mode label: $line"
    fail=1
  fi
done <<< "$matches"
if [ "$label_hits" -eq 0 ]; then
  echo "branding: FAIL, the dev-mode label \"$label\" was not found under app/src/settings/"
  fail=1
fi

# Rule 2.
if ! grep -rqF -- "Powered by Claude." app/src; then
  echo "branding: FAIL, no \"Powered by Claude.\" string in app/src (About dialog)"
  fail=1
fi

# Rule 3.
if ! command -v python3 >/dev/null; then
  echo "branding: python3 not found, cannot check sidecar strings" >&2
  exit 2
fi
status=0
python3 - "$phrase" sidecar/reader_sidecar <<'PY' || status=$?
import ast
import pathlib
import sys

phrase, root = sys.argv[1], pathlib.Path(sys.argv[2])
LOG_METHODS = {"debug", "info", "warning", "warn", "error", "exception", "critical", "log"}


def is_log_call(node):
    func = node.func
    if not isinstance(func, ast.Attribute) or func.attr not in LOG_METHODS:
        return False
    target = func.value
    return isinstance(target, ast.Name) and "log" in target.id.lower()


def skipped_nodes(tree):
    skip = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant):
            skip.add(id(node.value))
        elif isinstance(node, ast.Call) and is_log_call(node):
            for arg in [*node.args, *(k.value for k in node.keywords)]:
                skip.update(id(n) for n in ast.walk(arg))
    return skip


found = 0
files = sorted(root.rglob("*.py"))
if not files:
    print(f"branding: no Python files under {root}", file=sys.stderr)
    sys.exit(2)
for path in files:
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    skip = skipped_nodes(tree)
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Constant)
            and isinstance(node.value, str)
            and phrase in node.value
            and id(node) not in skip
        ):
            print(f'branding: FAIL, "{phrase}" in a sidecar string: {path}:{node.lineno}: {node.value!r}')
            found += 1
sys.exit(1 if found else 0)
PY
case "$status" in
  0) ;;
  1) fail=1 ;;
  *)
    echo "branding: sidecar check failed to run (status $status)" >&2
    exit 2
    ;;
esac

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "branding: OK ($label_hits dev-mode label line(s) in app/, no other \"$phrase\" in UI strings)"
