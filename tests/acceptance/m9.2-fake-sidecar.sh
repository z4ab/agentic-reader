#!/bin/sh
# Fake agent sidecar for tests/acceptance/m9.2.steps (chat navigation). Speaks
# sidecar/PROTOCOL.md over stdin/stdout and never calls Claude.
#
# Use it through SidecarClient's override (tests/acceptance/m9.2.sh does this):
#   READER_SIDECAR_CMD="sh /abs/path/tests/acceptance/m9.2-fake-sidecar.sh"
#
# Behaviour:
#   start          writes `ready` (version "fake-m9.2").
#   open_project   remembers the path. No reply.
#   new_chat       `chat_started` with session_id null, or `error` no_project
#                  before any open_project, or chat_exists.
#   resume_chat    `chat_started` echoing its session_id, or the same errors as
#                  new_chat, or `error` protocol when session_id is missing.
#   prompt         `error` unknown_chat or busy when they apply. Otherwise a
#                  reply runs in the background:
#     text containing "SLOW"  text_delta "Slow part N of 40. " every
#                  READER_FAKE_DELAY seconds (default 0.5), 40 pieces, then
#                  `result`. Interrupted by `interrupt`.
#     otherwise    text_delta "Quick reply." after 0.2 s, then `result`.
#     result       session_id "fake-session-<chat_id>" (cost_usd 0.0010,
#                  input_tokens 100, output_tokens 40).
#   interrupt      stops the running slow reply before its next delta with
#                  `result`. Does nothing when no reply runs.
#   anything else  `error` kind protocol.
# Exits on stdin EOF.

delay="${READER_FAKE_DELAY:-0.5}"
state="$(mktemp -d)"
project=""
chats=" "
streamer=""

cleanup() {
  [ -n "$streamer" ] && kill "$streamer" 2>/dev/null
  rm -rf "$state"
}
trap cleanup EXIT

emit() {
  printf '%s\n' "$1"
}

str_field() {
  printf '%s\n' "$2" | sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p"
}

seq_of() {
  s="$(printf '%s\n' "$1" | sed -n 's/.*"seq":\([0-9][0-9]*\).*/\1/p')"
  [ -n "$s" ] && printf '%s' "$s" || printf 'null'
}

error() { # chat_id_json kind message seq
  emit "{\"type\":\"error\",\"chat_id\":$1,\"kind\":\"$2\",\"message\":\"$3\",\"seq\":$4}"
}

finish() { # chat
  rm -f "$state/busy"
  emit "{\"type\":\"result\",\"chat_id\":\"$1\",\"session_id\":\"fake-session-$1\",\"usage\":{\"input_tokens\":100,\"output_tokens\":40},\"cost_usd\":0.0010}"
}

# stream_quick CHAT: one short piece, then the result.
stream_quick() {
  sleep 0.2
  emit "{\"type\":\"text_delta\",\"chat_id\":\"$1\",\"text\":\"Quick reply.\"}"
  finish "$1"
}

# stream_slow CHAT: 40 pieces, one every delay seconds, until interrupted.
stream_slow() {
  c="$1"
  i=1
  while [ "$i" -le 40 ]; do
    if [ -e "$state/stop" ]; then
      break
    fi
    sleep "$delay"
    [ -e "$state/stop" ] && break
    emit "{\"type\":\"text_delta\",\"chat_id\":\"$c\",\"text\":\"Slow part $i of 40. \"}"
    i=$((i + 1))
  done
  finish "$c"
}

start_chat() { # line session_id_json
  id="$(str_field chat_id "$1")"
  seq="$(seq_of "$1")"
  if [ -z "$project" ]; then
    error "\"$id\"" no_project "no project is open" "$seq"
  elif [ "${chats#* "$id" }" != "$chats" ]; then
    error "\"$id\"" chat_exists "chat is already open" "$seq"
  else
    chats="$chats$id "
    emit "{\"type\":\"chat_started\",\"chat_id\":\"$id\",\"session_id\":$2}"
  fi
}

emit '{"type":"ready","version":"fake-m9.2"}'

while IFS= read -r line; do
  line="$(printf '%s' "$line" | tr -d '\r')"
  seq="$(seq_of "$line")"
  case "$(str_field type "$line")" in
    open_project)
      project="$(str_field path "$line")"
      ;;
    new_chat)
      start_chat "$line" null
      ;;
    resume_chat)
      sid="$(str_field session_id "$line")"
      if [ -n "$sid" ]; then
        start_chat "$line" "\"$sid\""
      else
        error "\"$(str_field chat_id "$line")\"" protocol "resume_chat needs a session_id" "$seq"
      fi
      ;;
    prompt)
      id="$(str_field chat_id "$line")"
      if [ "${chats#* "$id" }" = "$chats" ]; then
        error "\"$id\"" unknown_chat "chat is not open" "$seq"
      elif [ -e "$state/busy" ]; then
        error "\"$id\"" busy "chat is busy" "$seq"
      else
        rm -f "$state/stop"
        : >"$state/busy"
        if printf '%s\n' "$line" | grep -q '"text":"[^"]*SLOW'; then
          stream_slow "$id" &
        else
          stream_quick "$id" &
        fi
        streamer=$!
      fi
      ;;
    interrupt)
      [ -e "$state/busy" ] && : >"$state/stop"
      ;;
    *)
      error null protocol "not a valid app message" "$seq"
      ;;
  esac
done
