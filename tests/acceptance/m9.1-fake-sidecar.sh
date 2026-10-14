#!/bin/sh
# Fake agent sidecar for tests/acceptance/m9.1.steps (keyboard shortcuts). Speaks
# sidecar/PROTOCOL.md over stdin/stdout and never calls Claude.
#
# Use it through SidecarClient's override (tests/acceptance/m9.1.sh does this):
#   READER_SIDECAR_CMD="sh /abs/path/tests/acceptance/m9.1-fake-sidecar.sh"
#
# Behaviour:
#   start          writes `ready` (version "fake-m9.1").
#   open_project   remembers the path. No reply.
#   new_chat       `chat_started` with session_id null, or `error` no_project
#                  before any open_project, or chat_exists.
#   prompt         `error` unknown_chat or busy when they apply. Otherwise a
#                  reply runs in the background:
#     text containing "permit"  text "Checking permission. ", then
#                  `permission_request` Write {"file_path":"notes.md"} and waits
#                  up to 60 s for permission_reply. Then text
#                  "Permission reply: allow|deny|none." and `result`.
#     otherwise    text_delta "Slow part N of 40. " every READER_FAKE_DELAY
#                  seconds (default 0.5), 40 pieces, then `result`
#                  (cost_usd 0.0010, input_tokens 100, output_tokens 40).
#   interrupt      stops the running reply before its next delta (and drops its
#                  pending permission request) with `result`. Does nothing when
#                  no reply runs.
#   permission_reply  delivers the answer to a pending request.
#   anything else  `error` kind protocol.
# Exits on stdin EOF.

delay="${READER_FAKE_DELAY:-0.5}"
state="$(mktemp -d)"
project=""
chats=" "
streamer=""
reqn=0

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
  emit "{\"type\":\"result\",\"chat_id\":\"$1\",\"session_id\":\"fake-session-1\",\"usage\":{\"input_tokens\":100,\"output_tokens\":40},\"cost_usd\":0.0010}"
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

# stream_permit CHAT REQ: asks for a Write permission and reports the answer.
stream_permit() {
  c="$1"
  req="$2"
  sleep "$delay"
  emit "{\"type\":\"text_delta\",\"chat_id\":\"$c\",\"text\":\"Checking permission. \"}"
  : >"$state/pending-$req"
  emit "{\"type\":\"permission_request\",\"req_id\":\"$req\",\"chat_id\":\"$c\",\"tool\":\"Write\",\"input\":{\"file_path\":\"notes.md\"}}"
  answer=none
  ticks=600
  while [ "$ticks" -gt 0 ]; do
    if [ -e "$state/reply-$req" ]; then
      answer="$(cat "$state/reply-$req")"
      break
    fi
    if [ -e "$state/stop" ]; then
      answer=stop
      break
    fi
    sleep 0.1
    ticks=$((ticks - 1))
  done
  rm -f "$state/pending-$req" "$state/reply-$req"
  if [ "$answer" != stop ]; then
    emit "{\"type\":\"text_delta\",\"chat_id\":\"$c\",\"text\":\"Permission reply: $answer.\"}"
  fi
  finish "$c"
}

start_chat() { # line session_json
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

emit '{"type":"ready","version":"fake-m9.1"}'

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
    prompt)
      id="$(str_field chat_id "$line")"
      if [ "${chats#* "$id" }" = "$chats" ]; then
        error "\"$id\"" unknown_chat "chat is not open" "$seq"
      elif [ -e "$state/busy" ]; then
        error "\"$id\"" busy "chat is busy" "$seq"
      else
        rm -f "$state/stop"
        : >"$state/busy"
        if printf '%s\n' "$line" | grep -q '"text":"[^"]*permit'; then
          reqn=$((reqn + 1))
          stream_permit "$id" "fake-req-$$-$reqn" &
        else
          stream_slow "$id" &
        fi
        streamer=$!
      fi
      ;;
    interrupt)
      [ -e "$state/busy" ] && : >"$state/stop"
      ;;
    permission_reply)
      req="$(str_field req_id "$line")"
      answer=deny
      printf '%s\n' "$line" | grep -q '"allow":true' && answer=allow
      if [ -e "$state/pending-$req" ]; then
        printf '%s' "$answer" >"$state/reply-$req"
      fi
      ;;
    *)
      error null protocol "not a valid app message" "$seq"
      ;;
  esac
done
