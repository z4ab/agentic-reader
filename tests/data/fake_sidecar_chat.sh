#!/bin/sh
# Fake agent sidecar for the M5.2 chat UI acceptance steps. Speaks
# sidecar/PROTOCOL.md over stdin/stdout and never calls Claude.
#
# Use it through SidecarClient's override:
#   READER_SIDECAR_CMD="sh /abs/path/tests/data/fake_sidecar_chat.sh"
#
# Behaviour:
#   start          writes `ready` (version "fake-chat-1").
#   open_project   remembers the path. No reply, as in PROTOCOL.md.
#   new_chat       `chat_started` with session_id null, or `error` no_project
#                  before any open_project, or chat_exists.
#   resume_chat    `chat_started` with the given session_id (same checks).
#   prompt         `error` unknown_chat or busy when they apply. Otherwise a
#                  reply streams in the background, one `text_delta` every
#                  READER_FAKE_DELAY seconds (default 0.5):
#                    - text containing "fail": one delta, then `error` kind
#                      internal, message "simulated failure from the fake sidecar".
#                    - any other text: a Markdown reply of 18 deltas (heading,
#                      bold, list, Python code block, inline code, link, and a
#                      last italic line naming the project folder), then
#                      `result` with cost_usd 0.0123 and usage
#                      input_tokens 1234, output_tokens 56.
#   interrupt      stops the running reply before its next delta, which then
#                  ends with `result` (cost_usd 0.0042, input_tokens 1234,
#                  output_tokens 12). Does nothing when no reply runs.
#   permission_reply  ignored.
#   anything else  `error` kind protocol.
# Every received line is logged to stderr (sidecar.log) as "fake: <line>".
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

# The reply pieces, already JSON-escaped. Some split mid-word, as real deltas do.
# A space between pieces starts the next line, since trailing spaces get lost.
reply_pieces() {
  cat <<'EOF'
## Attention in brief\n\n
The paper makes **three** main points:\n\n
- Scaled dot-product attention divides by the key size\n
- Multi-head atten
tion runs *several* heads in parallel\n
- Positional encoding adds word order\n\n
A minimal version in Python:\n\n
```python\n
def attention(q, k, v):\n
    w = softmax(q @ k.T / sqrt(d_k))\n
    return w @ v\n
```\n\n
Here `d_k` is the key size. See [the paper](https://arxiv.org/abs/1706.03762)
 for the full derivation.\n\n
*Project folder:
 PROJECT*\n
EOF
}

# stream CHAT_ID MODE SEQ. Runs in the background. Removes the busy flag before
# the last message, so the app can send the next prompt as soon as it sees it.
stream() {
  chat="$1"
  if [ "$2" = fail ]; then
    sleep "$delay"
    emit "{\"type\":\"text_delta\",\"chat_id\":\"$chat\",\"text\":\"Checking the document...\\n\\n\"}"
    sleep "$delay"
    rm -f "$state/busy"
    error "\"$chat\"" internal "simulated failure from the fake sidecar" "$3"
    return
  fi
  reply_pieces | while IFS= read -r piece; do
    sleep "$delay"
    if [ -e "$state/stop" ]; then
      rm -f "$state/busy"
      emit "{\"type\":\"result\",\"chat_id\":\"$chat\",\"session_id\":\"fake-session-1\",\"usage\":{\"input_tokens\":1234,\"output_tokens\":12},\"cost_usd\":0.0042}"
      exit 1
    fi
    [ "$piece" = " PROJECT*\\n" ] && piece=" $project*\\n"
    emit "{\"type\":\"text_delta\",\"chat_id\":\"$chat\",\"text\":\"$piece\"}"
  done || return
  sleep "$delay"
  rm -f "$state/busy"
  emit "{\"type\":\"result\",\"chat_id\":\"$chat\",\"session_id\":\"fake-session-1\",\"usage\":{\"input_tokens\":1234,\"output_tokens\":56},\"cost_usd\":0.0123}"
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

emit '{"type":"ready","version":"fake-chat-1"}'

while IFS= read -r line; do
  line="$(printf '%s' "$line" | tr -d '\r')"
  printf 'fake: %s\n' "$line" >&2
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
      if [ "${chats#* "$id" }" = "$chats" ]; then
        error "\"$id\"" unknown_chat "chat is not open" "$seq"
      elif [ -e "$state/busy" ]; then
        error "\"$id\"" busy "chat is busy" "$seq"
      else
        mode=reply
        printf '%s\n' "$line" | grep -q '"text":"[^"]*fail' && mode=fail
        rm -f "$state/stop"
        : >"$state/busy"
        stream "$id" "$mode" "$seq" &
        streamer=$!
      fi
      ;;
    interrupt)
      [ -e "$state/busy" ] && : >"$state/stop"
      ;;
    permission_reply)
      ;;
    *)
      error null protocol "not a valid app message" "$seq"
      ;;
  esac
done
