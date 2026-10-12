#!/bin/sh
# Fake agent sidecar for tests/acceptance/m5.4.steps: tool activity and
# permission requests. Speaks sidecar/PROTOCOL.md over stdin/stdout and never
# calls Claude.
#
# Use it through SidecarClient's override:
#   READER_SIDECAR_CMD="sh /abs/path/tests/acceptance/m5.4-fake-sidecar.sh"
#
# Behaviour:
#   start          writes `ready` (version "fake-tools-1").
#   open_project   remembers the path. No reply, as in PROTOCOL.md.
#   new_chat       `chat_started` with session_id null, or `error` no_project
#                  before any open_project, or chat_exists.
#   resume_chat    `chat_started` with the given session_id (same checks).
#   interrupt      ends a running reply (and drops its pending permission
#                  request) with `result`. Does nothing when no reply runs.
#   prompt         `error` unknown_chat or busy when they apply. Otherwise a
#                  reply runs in the background, one message every
#                  READER_FAKE_DELAY seconds (default 0.3). The prompt text
#                  picks the reply, first match wins:
#     "tools"     text, then one `tool_use` per formatted tool and one unknown
#                 tool, with text between them, then `result`:
#                   Read      {"file_path":"papers/attention.pdf","pages":"10-12"}
#                   Glob      {"pattern":"**/*.pdf"}
#                   Grep      {"pattern":"softmax","path":"papers"}
#                   Write     {"file_path":"notes/draft.md","content":"DRAFT-BODY ..."}
#                   Edit      {"file_path":"notes/draft.md","old_string":"OLD-TEXT","new_string":"NEW-TEXT"}
#                   Bash      {"command":"grep -c \"<b>\" notes.md && wc -l notes.md","description":"..."}
#                   WebFetch  {"url":"https://arxiv.org/abs/1706.03762","prompt":"PROMPT-TEXT ..."}
#                   WebSearch {"query":"transformer positional encoding"}
#                   mcp__notes__save {"title":"Draft","tags":["a","b"]}
#     "notes.md"  text, `tool_use` Write and a `permission_request` Write for
#                 <project>/notes.md, then waits up to 300 s for the reply.
#                 Allow: writes the file. Deny or no reply: writes nothing.
#                 Then text "Permission reply: allow|deny|none." and
#                 "notes.md exists: yes|no." (checked on disk), then `result`.
#     "timeout"   `permission_request` Bash {"command":"rm -rf build"}; after
#                 READER_FAKE_PERMISSION_WAIT seconds (default 4) without a
#                 reply it gives up as the real sidecar does after 5 minutes:
#                 text "No answer, so the tool was denied." then `result`.
#     "lost"      `permission_request` Bash {"command":"make install"}; after
#                 the same wait without a reply, `error` kind process
#                 ("simulated CLI exit, the chat is closed") and the chat is
#                 closed (later prompts to it get unknown_chat).
#     "crash"     `permission_request` Bash {"command":"git push"}; after the
#                 same wait the fake kills itself (an unexpected exit), so the
#                 app restarts the sidecar while the dialog is open.
#     otherwise   text "OK." then `result`.
#   permission_reply
#                  For a request still pending: delivers the reply.
#                  For a request this process sent that is no longer pending
#                  (answered, timed out, chat lost): ignored, as the real
#                  sidecar ignores a late reply.
#                  For a req_id this process never sent, for example a reply
#                  to a request from before a restart: `error` with chat_id
#                  null, kind internal, message
#                  "unexpected permission_reply <req_id>". The app shows it as
#                  a red notice, which the steps treat as a failure.
#   anything else  `error` kind protocol.
# req_ids are "fake-req-<pid>-<n>", so a restarted fake never reuses one.
# Every result is cost_usd 0.0010, input_tokens 100, output_tokens 10.
# Every received line is logged to stderr (sidecar.log) as "fake: <line>".
# Exits on stdin EOF.

delay="${READER_FAKE_DELAY:-0.3}"
perm_wait="${READER_FAKE_PERMISSION_WAIT:-4}"
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

log() {
  printf 'fake: %s\n' "$1" >&2
}

# str_field NAME LINE: the value of a simple string field (no escaped quotes).
str_field() {
  printf '%s\n' "$2" | sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p"
}

# seq_of LINE: the integer seq, or null.
seq_of() {
  s="$(printf '%s\n' "$1" | sed -n 's/.*"seq":\([0-9][0-9]*\).*/\1/p')"
  [ -n "$s" ] && printf '%s' "$s" || printf 'null'
}

error() { # chat_id_json kind message seq
  emit "{\"type\":\"error\",\"chat_id\":$1,\"kind\":\"$2\",\"message\":\"$3\",\"seq\":$4}"
}

# ---- Reply pieces. Arguments are already JSON-escaped. ----

delta() { # chat text
  sleep "$delay"
  emit "{\"type\":\"text_delta\",\"chat_id\":\"$1\",\"text\":\"$2\"}"
}

tool() { # chat name input_json
  sleep "$delay"
  emit "{\"type\":\"tool_use\",\"chat_id\":\"$1\",\"name\":\"$2\",\"input\":$3}"
}

finish() { # chat
  sleep "$delay"
  rm -f "$state/busy"
  emit "{\"type\":\"result\",\"chat_id\":\"$1\",\"session_id\":\"fake-session-1\",\"usage\":{\"input_tokens\":100,\"output_tokens\":10},\"cost_usd\":0.0010}"
}

ask() { # chat req tool input_json
  sleep "$delay"
  : >"$state/issued-$2"
  : >"$state/pending-$2"
  emit "{\"type\":\"permission_request\",\"req_id\":\"$2\",\"chat_id\":\"$1\",\"tool\":\"$3\",\"input\":$4}"
}

# await REQ SECONDS: prints allow, deny, none (no reply in time) or stop
# (interrupted). The request is no longer pending afterwards.
await() {
  ticks=$(($2 * 10))
  while [ "$ticks" -gt 0 ]; do
    if [ -e "$state/reply-$1" ]; then
      cat "$state/reply-$1"
      rm -f "$state/reply-$1"
      return
    fi
    if [ -e "$state/stop" ]; then
      rm -f "$state/pending-$1"
      printf 'stop'
      return
    fi
    sleep 0.1
    ticks=$((ticks - 1))
  done
  rm -f "$state/pending-$1"
  printf 'none'
}

# ---- Replies. Each runs in the background: stream_<mode> CHAT REQ SEQ ----

stream_tools() {
  c="$1"
  delta "$c" "Let me look through the project first.\\n\\n"
  tool "$c" Read '{"file_path":"papers/attention.pdf","pages":"10-12"}'
  tool "$c" Glob '{"pattern":"**/*.pdf"}'
  tool "$c" Grep '{"pattern":"softmax","path":"papers"}'
  delta "$c" "Found the section. Now a draft and some checks.\\n\\n"
  tool "$c" Write '{"file_path":"notes/draft.md","content":"DRAFT-BODY must not show in the tool line"}'
  tool "$c" Edit '{"file_path":"notes/draft.md","old_string":"OLD-TEXT","new_string":"NEW-TEXT"}'
  tool "$c" Bash '{"command":"grep -c \"<b>\" notes.md && wc -l notes.md","description":"Count bold tags"}'
  tool "$c" WebFetch '{"url":"https://arxiv.org/abs/1706.03762","prompt":"PROMPT-TEXT summarize the abstract"}'
  tool "$c" WebSearch '{"query":"transformer positional encoding"}'
  tool "$c" mcp__notes__save '{"title":"Draft","tags":["a","b"]}'
  delta "$c" "That is everything I needed.\\n"
  finish "$c"
}

stream_notes() {
  c="$1"
  notes="$project/notes.md"
  input="{\"file_path\":\"$notes\",\"content\":\"# Summary\\n\\nThe paper introduces the Transformer.\\n\"}"
  delta "$c" "I will save a short summary.\\n\\n"
  tool "$c" Write "$input"
  ask "$c" "$2" Write "$input"
  answer="$(await "$2" 300)"
  [ "$answer" = stop ] && { finish "$c"; return; }
  if [ "$answer" = allow ]; then
    printf '# Summary\n\nThe paper introduces the Transformer.\n' >"$notes"
  fi
  delta "$c" "Permission reply: $answer.\\n\\n"
  if [ -e "$notes" ]; then
    delta "$c" "notes.md exists: yes.\\n"
  else
    delta "$c" "notes.md exists: no.\\n"
  fi
  finish "$c"
}

stream_timeout() {
  c="$1"
  delta "$c" "I want to clean the build folder.\\n\\n"
  tool "$c" Bash '{"command":"rm -rf build"}'
  ask "$c" "$2" Bash '{"command":"rm -rf build"}'
  answer="$(await "$2" "$perm_wait")"
  if [ "$answer" = none ]; then
    delta "$c" "No answer, so the tool was denied.\\n"
  else
    delta "$c" "Permission reply: $answer.\\n"
  fi
  finish "$c"
}

stream_lost() {
  c="$1"
  delta "$c" "I want to install the package.\\n\\n"
  tool "$c" Bash '{"command":"make install"}'
  ask "$c" "$2" Bash '{"command":"make install"}'
  answer="$(await "$2" "$perm_wait")"
  if [ "$answer" = none ]; then
    : >"$state/closed-$c"
    rm -f "$state/busy"
    error "\"$c\"" process "simulated CLI exit, the chat is closed" "$3"
  else
    delta "$c" "Permission reply: $answer.\\n"
    finish "$c"
  fi
}

stream_crash() {
  c="$1"
  delta "$c" "I want to push the branch.\\n\\n"
  tool "$c" Bash '{"command":"git push"}'
  ask "$c" "$2" Bash '{"command":"git push"}'
  sleep "$perm_wait"
  log "simulating a crash with a permission request open"
  rm -rf "$state"
  kill -9 "$main_pid"
  exit 0
}

stream_plain() {
  delta "$1" "OK.\\n"
  finish "$1"
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

# text_has WORD LINE: the prompt text contains WORD.
text_has() {
  printf '%s\n' "$2" | grep -q "\"text\":\"[^\"]*$1"
}

main_pid=$$
emit '{"type":"ready","version":"fake-tools-1"}'

while IFS= read -r line; do
  line="$(printf '%s' "$line" | tr -d '\r')"
  log "$line"
  seq="$(seq_of "$line")"
  case "$(str_field type "$line")" in
    open_project)
      project="$(str_field path "$line")"
      ;;
    new_chat)
      start_chat "$line" null
      ;;
    resume_chat)
      start_chat "$line" "\"$(str_field session_id "$line")\""
      ;;
    prompt)
      id="$(str_field chat_id "$line")"
      if [ "${chats#* "$id" }" = "$chats" ] || [ -e "$state/closed-$id" ]; then
        error "\"$id\"" unknown_chat "chat is not open" "$seq"
      elif [ -e "$state/busy" ]; then
        error "\"$id\"" busy "chat is busy" "$seq"
      else
        mode=plain
        if text_has tools "$line"; then mode=tools
        elif text_has 'notes\.md' "$line"; then mode=notes
        elif text_has timeout "$line"; then mode=timeout
        elif text_has lost "$line"; then mode=lost
        elif text_has crash "$line"; then mode=crash
        fi
        reqn=$((reqn + 1))
        rm -f "$state/stop"
        : >"$state/busy"
        "stream_$mode" "$id" "fake-req-$$-$reqn" "$seq" &
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
      case "$req" in
        "" | *[!A-Za-z0-9-]*)
          error null internal "unexpected permission_reply $req" "$seq"
          ;;
        *)
          if [ -e "$state/pending-$req" ]; then
            rm -f "$state/pending-$req"
            printf '%s' "$answer" >"$state/reply-$req"
          elif [ -e "$state/issued-$req" ]; then
            log "ignored a late permission_reply for $req"
          else
            error null internal "unexpected permission_reply $req" "$seq"
          fi
          ;;
      esac
      ;;
    *)
      error null protocol "not a valid app message" "$seq"
      ;;
  esac
done
