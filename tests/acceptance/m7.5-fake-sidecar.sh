#!/bin/sh
# Fake sidecar for tests/acceptance/m7.5.steps. Never contacts the network.
#
# At start it appends one line to $READER_FAKE_LOG (when set) saying which
# credential variables reached it, the READER_MODEL value, and the value of
# ENABLE_CLAUDEAI_MCP_SERVERS:
#   start ANTHROPIC_API_KEY=set|unset CLAUDE_CODE_OAUTH_TOKEN=set|unset ANTHROPIC_AUTH_TOKEN=set|unset READER_MODEL=<value>|unset ENABLE_CLAUDEAI_MCP_SERVERS=<value>|unset
# It never prints, logs, or stores a credential value.
# It then waits $READER_FAKE_READY_DELAY seconds (default 3), so a screenshot
# can catch the app before the restarted sidecar is ready, and prints `ready`.
# It reads stdin and answers nothing. On stdin EOF it appends "exit" to
# $READER_FAKE_LOG and exits.

state() {
  # Prints "set" when the variable named $1 is set and not empty, else "unset".
  eval "v=\${$1:-}"
  if [ -n "$v" ]; then echo set; else echo unset; fi
}

value() {
  # Prints the value of the variable named $1, or "unset" when it is empty.
  # Only for variables that are not credentials.
  eval "v=\${$1:-}"
  if [ -n "$v" ]; then echo "$v"; else echo unset; fi
}

note() {
  [ -n "${READER_FAKE_LOG:-}" ] && printf '%s\n' "$1" >> "$READER_FAKE_LOG"
  echo "fake-m7.5: $1" >&2
}

note "start ANTHROPIC_API_KEY=$(state ANTHROPIC_API_KEY) CLAUDE_CODE_OAUTH_TOKEN=$(state CLAUDE_CODE_OAUTH_TOKEN) ANTHROPIC_AUTH_TOKEN=$(state ANTHROPIC_AUTH_TOKEN) READER_MODEL=$(value READER_MODEL) ENABLE_CLAUDEAI_MCP_SERVERS=$(value ENABLE_CLAUDEAI_MCP_SERVERS)"
sleep "${READER_FAKE_READY_DELAY:-3}"
echo '{"type":"ready","version":"fake-m7.5"}'
while IFS= read -r line; do :; done
note "exit"
