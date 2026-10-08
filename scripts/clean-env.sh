#!/usr/bin/env bash
# Run a command with a minimal environment.
# Drops CLAUDECODE, CLAUDE_CODE_*, ANTHROPIC_*, DISPLAY and WAYLAND_DISPLAY so a
# nested sidecar uses the local ~/.claude login and a GUI cannot reach the
# user's desktop. Keeps READER_* vars. shot.sh adds its own Xvfb DISPLAY.
# CLEAN_ENV_KEEP="NAME ..." keeps the named vars too, such as ANTHROPIC_API_KEY
# for a live check with a given key. Values never appear on a command line.
set -euo pipefail

if [ $# -eq 0 ]; then
  echo "usage: scripts/clean-env.sh CMD [ARGS...]" >&2
  exit 2
fi

vars=(HOME="$HOME" PATH="$PATH" USER="${USER:-$(id -un)}" LANG="${LANG:-C.UTF-8}")
for name in XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS VCPKG_ROOT; do
  if [ -n "${!name:-}" ]; then
    vars+=("$name=${!name}")
  fi
done
while IFS='=' read -r name _; do
  vars+=("$name=${!name}")
done < <(env | grep -E '^READER_[A-Z0-9_]*=' || true)
for name in ${CLEAN_ENV_KEEP:-}; do
  if [ -n "${!name+set}" ]; then
    vars+=("$name=${!name}")
  fi
done

exec env -i "${vars[@]}" "$@"
