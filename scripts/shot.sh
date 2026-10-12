#!/usr/bin/env bash
# Run a GUI command headless under Xvfb, drive it with xdotool, save screenshots.
#
# usage: scripts/shot.sh -o OUTDIR [-s STEPS] [-g WxH] [-t SECS] [-n TITLE] -- CMD [ARGS...]
#
#   -o OUTDIR  where screenshots and app.log go (created if missing)
#   -s STEPS   steps file, one command per line (see below). Without it: wait 2, shot window.
#   -g WxH     screen size, default 1600x1000
#   -t SECS    how long to wait for the window, default 20
#   -n TITLE   window name to wait for (regex), default "Agentic Reader"
#
# Steps:
#   wait <seconds>
#   key <xdotool keysyms...>        e.g. key ctrl+o, key Next Next
#   type <text>
#   click <x> <y> [button]
#   dblclick <x> <y>
#   tripleclick <x> <y>
#   drag <x1> <y1> <x2> <y2>
#   shot <name>                     writes OUTDIR/NN-<name>.png
#   # comment
#
# The window is moved to the screen origin, so coordinates are window coordinates.
# The command runs through clean-env.sh with only the Xvfb DISPLAY and GDK_BACKEND=x11,
# so nothing can open on the user's desktop.
set -euo pipefail

usage() {
  sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 2
}

out=""
steps=""
geom="1600x1000"
timeout_s=20
title="Agentic Reader"
while getopts "o:s:g:t:n:h" opt; do
  case "$opt" in
    o) out="$OPTARG" ;;
    s) steps="$OPTARG" ;;
    g) geom="$OPTARG" ;;
    t) timeout_s="$OPTARG" ;;
    n) title="$OPTARG" ;;
    *) usage ;;
  esac
done
shift $((OPTIND - 1))
[ "${1:-}" = "--" ] && shift
[ -n "$out" ] && [ $# -gt 0 ] || usage
[ -z "$steps" ] || [ -f "$steps" ] || { echo "shot.sh: steps file not found: $steps" >&2; exit 2; }

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$out"
tmp="$(mktemp -d)"
xvfb_pid=""
app_pid=""

cleanup() {
  if [ -n "$app_pid" ] && kill -0 "$app_pid" 2>/dev/null; then
    kill "$app_pid" 2>/dev/null || true
    for _ in 1 2 3 4 5 6; do kill -0 "$app_pid" 2>/dev/null || break; sleep 0.5; done
    kill -9 "$app_pid" 2>/dev/null || true
  fi
  [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null || true
  [ -f "$tmp/app.err" ] && cp "$tmp/app.err" "$out/app.log"
  rm -rf "$tmp"
}
trap cleanup EXIT

die() {
  echo "shot.sh: $*" >&2
  if [ -s "$tmp/app.err" ]; then
    echo "--- app stderr (last 30 lines) ---" >&2
    tail -n 30 "$tmp/app.err" >&2
  fi
  exit 1
}

# Start Xvfb on a free display. -displayfd prints the chosen number.
Xvfb -displayfd 3 -screen 0 "${geom}x24" -nolisten tcp 3>"$tmp/display" 2>"$tmp/xvfb.log" &
xvfb_pid=$!
for _ in $(seq 50); do
  [ -s "$tmp/display" ] && break
  kill -0 "$xvfb_pid" 2>/dev/null || die "Xvfb exited: $(cat "$tmp/xvfb.log")"
  sleep 0.1
done
[ -s "$tmp/display" ] || die "Xvfb did not report a display"
display=":$(head -n 1 "$tmp/display")"
export DISPLAY="$display"
unset WAYLAND_DISPLAY

"$here/clean-env.sh" env DISPLAY="$display" GDK_BACKEND=x11 "$@" >"$tmp/app.out" 2>"$tmp/app.err" &
app_pid=$!

win="$(timeout "$timeout_s" xdotool search --sync --onlyvisible --name "$title" 2>/dev/null | head -n 1 || true)"
[ -n "$win" ] || die "no visible window matching '$title' within ${timeout_s}s"
xdotool windowmove --sync "$win" 0 0 2>/dev/null || xdotool windowmove "$win" 0 0
xdotool windowfocus --sync "$win" 2>/dev/null || true
sleep 0.5

if command -v import >/dev/null; then
  capture() { import -window root "$1"; }
else
  capture() { magick import -window root "$1"; }
fi

# mousemove --sync waits for a motion event, which never comes when the
# pointer is already there, so skip the move in that case.
moveto() {
  local x y
  eval "$(xdotool getmouselocation --shell | grep -E '^[XY]=' | tr 'XY' 'xy')"
  if [ "$x" != "$1" ] || [ "$y" != "$2" ]; then
    xdotool mousemove --sync "$1" "$2"
  fi
}

n=0
shots=()
run_step() {
  local cmd="$1"; shift
  case "$cmd" in
    wait) sleep "$1" ;;
    key) xdotool key --clearmodifiers "$@" ;;
    type) xdotool type --delay 20 -- "$*" ;;
    click) moveto "$1" "$2"; xdotool click "${3:-1}" ;;
    dblclick) moveto "$1" "$2"; xdotool click --repeat 2 --delay 80 1 ;;
    tripleclick) moveto "$1" "$2"; xdotool click --repeat 3 --delay 80 1 ;;
    drag)
      moveto "$1" "$2"; xdotool mousedown 1
      sleep 0.1
      xdotool mousemove --sync $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 ))
      sleep 0.1
      moveto "$3" "$4"; xdotool mouseup 1
      ;;
    shot)
      n=$((n + 1))
      local file
      file="$out/$(printf '%02d' "$n")-$1.png"
      capture "$file"
      shots+=("$file")
      ;;
    *) die "unknown step: $cmd" ;;
  esac
  sleep 0.3
  kill -0 "$app_pid" 2>/dev/null || die "app exited during step: $cmd $*"
}

if [ -n "$steps" ]; then
  while IFS= read -r line || [ -n "$line" ]; do
    line="${line#"${line%%[![:space:]]*}"}"
    [ -z "$line" ] || [ "${line:0:1}" = "#" ] && continue
    read -r -a words <<<"$line"
    run_step "${words[@]}"
  done <"$steps"
else
  run_step wait 2
  run_step shot window
fi

printf '%s\n' "${shots[@]}"
