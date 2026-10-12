#!/bin/sh
# Fake sidecar for tests/acceptance/m7.3.steps. Never contacts the network.
# Speaks sidecar/PROTOCOL.md over stdin/stdout. Every error it sends uses a
# kind and a message the real sidecar can send (reader_sidecar/errors.py),
# except MYSTERY, which sends a kind the app has never heard of.
#
# m7.3.steps runs it as `reader-sidecar` next to a copy of the app binary
# (see the header there), so the app finds it without READER_SIDECAR_CMD and
# a missing file makes SidecarClient::restart() fail.
#
#   start          writes `ready` (version "fake-m7.3").
#   open_project   remembers that a project is open. No reply.
#   new_chat       `chat_started` (session_id null), or `error` no_project
#                  before any open_project, or chat_exists.
#   resume_chat    `chat_started` with the given session_id (same checks).
#   interrupt, permission_reply   ignored.
#   anything else  `error` kind protocol.
#   prompt         `error` unknown_chat for a chat that is not open. Otherwise
#                  keyed on the prompt text (upper-case words, checked in this
#                  order):
#     AUTH         `error` auth "authentication failed: the API key is
#                  missing or invalid".
#     RATELIMIT    the first such prompt this process sees: `error` rate_limit
#                  "rate limit reached, try again later". Every later one: the
#                  reply "Retry worked: <prompt text>" and a `result`.
#     CLIMISSING   `error` cli_missing "the agent CLI was not found". The chat
#                  is closed (later prompts to it get unknown_chat).
#     PROCESSFAIL  `error` process "the agent process failed (exit code 1)".
#                  The chat is closed.
#     BUSY         a reply in two parts with an `error` busy ("chat is busy",
#                  echoing the prompt's seq) between them: "Working on it." ,
#                  then the busy error, then " Still working, done." and a
#                  `result`. An app that ignores busy shows the whole reply
#                  and its cost line.
#     INTERNAL     `error` internal "simulated internal failure".
#     MYSTERY      `error` kind "mystery_kind", "simulated unknown failure".
#     CRASH        the reply "About to crash." and then the process exits
#                  with status 3 (the sidecar itself dies, no `error`).
#     UNINSTALL    deletes this script's own file when it is named
#                  reader-sidecar (never the copy under tests/acceptance), then
#                  replies "Removed reader-sidecar." or, when the name does not
#                  match, "Not removed: <name>".
#     ping         (Test key) when ANTHROPIC_API_KEY contains SLOW: no answer
#                  at all, and on stdin EOF the process lingers 10 s before it
#                  exits, so a blocking shutdown on the UI thread shows up as a
#                  frozen window. Otherwise as any other text.
#     other text   the reply "Echo: <prompt text>" in two deltas.
#   Every reply ends with `result`: session_id "fake-session-m73", cost_usd
#   0.0010, input_tokens 10, output_tokens 5.
#
# When READER_FAKE_LOG is set, appends one line per event to it:
#   start key=set|unset     at launch; whether ANTHROPIC_API_KEY is set
#   prompt <text>           every prompt received, before it is answered
#   eof                     stdin closed
# It never prints, logs, or stores the key. stderr (sidecar.log) gets
# "fake-m7.3: <line>" for every line received.
# Prompts must be plain ASCII without quotes or backslashes.

pause="${READER_FAKE_DELAY:-0.5}"

note() {
  if [ -n "${READER_FAKE_LOG:-}" ]; then
    printf '%s\n' "$1" >>"$READER_FAKE_LOG"
  fi
}

emit() {
  printf '%s\n' "$1"
}

# str_field NAME LINE: the value of a simple string field (no escaped quotes).
str_field() {
  printf '%s\n' "$2" | sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p"
}

# seq_of LINE: the integer seq, or null.
seq_of() {
  s="$(printf '%s\n' "$1" | sed -n 's/.*"seq":\([0-9][0-9]*\).*/\1/p')"
  if [ -n "$s" ]; then printf '%s' "$s"; else printf 'null'; fi
}

error() { # chat_id_json kind message seq
  emit "{\"type\":\"error\",\"chat_id\":$1,\"kind\":\"$2\",\"message\":\"$3\",\"seq\":$4}"
}

delta() { # chat_id text
  emit "{\"type\":\"text_delta\",\"chat_id\":\"$1\",\"text\":\"$2\"}"
}

result() { # chat_id
  emit "{\"type\":\"result\",\"chat_id\":\"$1\",\"session_id\":\"fake-session-m73\",\"usage\":{\"input_tokens\":10,\"output_tokens\":5},\"cost_usd\":0.0010}"
}

reply() { # chat_id text
  sleep "$pause"
  delta "$1" "$2"
  sleep "$pause"
  result "$1"
}

is_open() { # chat_id
  case "$chats" in
    *" $1 "*) return 0 ;;
  esac
  return 1
}

close_chat() { # chat_id
  chats="$(printf '%s' "$chats" | sed "s/ $1 / /")"
}

start_chat() { # line session_json
  id="$(str_field chat_id "$1")"
  seq="$(seq_of "$1")"
  if [ "$project" = 0 ]; then
    error "\"$id\"" no_project "no project is open" "$seq"
  elif is_open "$id"; then
    error "\"$id\"" chat_exists "chat is already open" "$seq"
  else
    chats="$chats$id "
    emit "{\"type\":\"chat_started\",\"chat_id\":\"$id\",\"session_id\":$2}"
  fi
}

slow=0
case "${ANTHROPIC_API_KEY:-}" in
  *SLOW*) slow=1 ;;
esac
if [ -n "${ANTHROPIC_API_KEY:-}" ]; then
  note "start key=set"
else
  note "start key=unset"
fi

project=0
chats=" "
rate_limited=0
emit '{"type":"ready","version":"fake-m7.3"}'

while IFS= read -r line; do
  line="$(printf '%s' "$line" | tr -d '\r')"
  printf 'fake-m7.3: %s\n' "$line" >&2
  seq="$(seq_of "$line")"
  case "$(str_field type "$line")" in
    open_project)
      project=1
      ;;
    new_chat)
      start_chat "$line" null
      ;;
    resume_chat)
      start_chat "$line" "\"$(str_field session_id "$line")\""
      ;;
    interrupt | permission_reply)
      ;;
    prompt)
      id="$(str_field chat_id "$line")"
      text="$(str_field text "$line")"
      note "prompt $text"
      if ! is_open "$id"; then
        error "\"$id\"" unknown_chat "chat is not open" "$seq"
        continue
      fi
      case "$text" in
        *AUTH*)
          sleep "$pause"
          error "\"$id\"" auth "authentication failed: the API key is missing or invalid" "$seq"
          ;;
        *RATELIMIT*)
          if [ "$rate_limited" = 0 ]; then
            rate_limited=1
            sleep "$pause"
            error "\"$id\"" rate_limit "rate limit reached, try again later" "$seq"
          else
            reply "$id" "Retry worked: $text"
          fi
          ;;
        *CLIMISSING*)
          close_chat "$id"
          sleep "$pause"
          error "\"$id\"" cli_missing "the agent CLI was not found" "$seq"
          ;;
        *PROCESSFAIL*)
          close_chat "$id"
          sleep "$pause"
          error "\"$id\"" process "the agent process failed (exit code 1)" "$seq"
          ;;
        *BUSY*)
          sleep "$pause"
          delta "$id" "Working on it."
          sleep "$pause"
          error "\"$id\"" busy "chat is busy" "$seq"
          sleep "$pause"
          delta "$id" " Still working, done."
          sleep "$pause"
          result "$id"
          ;;
        *INTERNAL*)
          sleep "$pause"
          error "\"$id\"" internal "simulated internal failure" "$seq"
          ;;
        *MYSTERY*)
          sleep "$pause"
          error "\"$id\"" mystery_kind "simulated unknown failure" "$seq"
          ;;
        *CRASH*)
          sleep "$pause"
          delta "$id" "About to crash."
          sleep "$pause"
          exit 3
          ;;
        *UNINSTALL*)
          name="$(basename "$0")"
          if [ "$name" = reader-sidecar ]; then
            rm -f "$0"
            reply "$id" "Removed reader-sidecar."
          else
            reply "$id" "Not removed: $name"
          fi
          ;;
        ping)
          if [ "$slow" = 0 ]; then
            reply "$id" "Echo: $text"
          fi
          ;;
        *)
          reply "$id" "Echo: $text"
          ;;
      esac
      ;;
    *)
      error null protocol "not a valid app message" "$seq"
      ;;
  esac
done

note "eof"
if [ "$slow" = 1 ]; then
  sleep 10
fi
