#!/bin/sh
# Fake sidecar for tests/acceptance/m7.1.steps. Never contacts the network.
# Speaks just enough of sidecar/PROTOCOL.md for "Test key":
#   start          -> ready
#   open_project   -> no reply (marks a project open)
#   new_chat       -> chat_started, or error no_project before any open_project
#   prompt         -> error unknown_chat for a chat not started, else error
#                     kind "auth" when ANTHROPIC_API_KEY is set and not empty
#                     (a bad key), else error kind "internal" saying no key
#                     reached the sidecar
#   anything else  -> no reply
# It echoes chat_id and seq. It never prints, logs, or stores the key.
# Logs (stderr) name only the message types it saw.

field() {
  # Prints the string or integer value of JSON field $2 in line $1, or nothing.
  printf '%s\n' "$1" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\{0,1\}\([^\",}]*\).*/\1/p"
}

project=0
chats=" "
echo '{"type":"ready","version":"fake-m7.1"}'
while IFS= read -r line; do
  type="$(field "$line" type)"
  chat="$(field "$line" chat_id)"
  seq="$(field "$line" seq)"
  [ -n "$seq" ] || seq=null
  echo "fake-sidecar: got $type" >&2
  case "$type" in
    open_project)
      project=1
      ;;
    new_chat)
      if [ "$project" = 1 ]; then
        chats="$chats$chat "
        printf '{"type":"chat_started","chat_id":"%s","session_id":null}\n' "$chat"
      else
        printf '{"type":"error","chat_id":"%s","kind":"no_project","message":"no project is open","seq":%s}\n' "$chat" "$seq"
      fi
      ;;
    prompt)
      case "$chats" in
        *" $chat "*) ;;
        *)
          printf '{"type":"error","chat_id":"%s","kind":"unknown_chat","message":"chat is not open","seq":%s}\n' "$chat" "$seq"
          continue
          ;;
      esac
      if [ -n "${ANTHROPIC_API_KEY:-}" ]; then
        printf '{"type":"error","chat_id":"%s","kind":"auth","message":"invalid x-api-key","seq":%s}\n' "$chat" "$seq"
      else
        printf '{"type":"error","chat_id":"%s","kind":"internal","message":"fake sidecar: no API key in the environment","seq":%s}\n' "$chat" "$seq"
      fi
      ;;
  esac
done
