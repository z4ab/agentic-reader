#!/bin/sh
# Fake sidecar for tests/acceptance/m6.2.sh. Never contacts the network.
# Speaks just enough of sidecar/PROTOCOL.md to keep the chat pane quiet:
#   start                    -> ready (version "fake-m6.2")
#   open_project             -> no reply (marks a project open)
#   new_chat, resume_chat    -> chat_started, or error no_project before any
#                               open_project
#   prompt                   -> the reply "Echo: <text>" and a result
#   anything else            -> no reply
#
# When READER_FAKE_LOG is set, appends one line per message received:
#   open_project <path>      the "path" field exactly as sent
#   <type>                   for every other message type
#   eof                      stdin closed
# stderr (sidecar.log) gets "fake-m6.2: <line>" for every line received.
# Paths and prompts must be plain ASCII without quotes or backslashes.

note() {
  if [ -n "${READER_FAKE_LOG:-}" ]; then
    printf '%s\n' "$1" >>"$READER_FAKE_LOG"
  fi
}

# str_field NAME LINE: the value of a simple string field (no escaped quotes).
str_field() {
  printf '%s\n' "$2" | sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p"
}

# seq_of LINE: the integer seq, or null.
seq_of() {
  s="$(printf '%s\n' "$1" | sed -n 's/.*"seq"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p')"
  if [ -n "$s" ]; then printf '%s' "$s"; else printf 'null'; fi
}

project=0
printf '%s\n' '{"type":"ready","version":"fake-m6.2"}'

while IFS= read -r line; do
  line="$(printf '%s' "$line" | tr -d '\r')"
  printf 'fake-m6.2: %s\n' "$line" >&2
  type="$(str_field type "$line")"
  case "$type" in
    open_project)
      project=1
      note "open_project $(str_field path "$line")"
      ;;
    new_chat | resume_chat)
      note "$type"
      id="$(str_field chat_id "$line")"
      if [ "$project" = 1 ]; then
        printf '{"type":"chat_started","chat_id":"%s","session_id":null}\n' "$id"
      else
        printf '{"type":"error","chat_id":"%s","kind":"no_project","message":"no project is open","seq":%s}\n' \
          "$id" "$(seq_of "$line")"
      fi
      ;;
    prompt)
      note "$type"
      id="$(str_field chat_id "$line")"
      printf '{"type":"text_delta","chat_id":"%s","text":"Echo: %s"}\n' "$id" "$(str_field text "$line")"
      printf '{"type":"result","chat_id":"%s","session_id":"fake-session-m62","usage":{"input_tokens":10,"output_tokens":5},"cost_usd":0.0010}\n' "$id"
      ;;
    *)
      note "$type"
      ;;
  esac
done

note "eof"
