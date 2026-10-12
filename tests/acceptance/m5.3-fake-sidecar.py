#!/usr/bin/env python3
"""Fake sidecar for tests/acceptance/m5.3.sh. Never contacts the network.

Speaks just enough of sidecar/PROTOCOL.md for the chat pane:
  start                    -> ready (version "fake-m5.3")
  open_project             -> no reply (marks a project open)
  new_chat, resume_chat    -> chat_started, or error no_project before any
                              open_project
  prompt                   -> one text_delta naming the prompt's number and the
                              context fields it carried, then a result
  anything else            -> no reply

When READER_FAKE_LOG is set, appends every `prompt` it receives to that file
as one line of compact JSON (the message exactly as parsed, sorted keys), so
the driver can compare the text and the context with what it expects.
stderr (sidecar.log) gets "fake-m5.3: <line>" for every line received.
"""

import json
import os
import sys


def emit(msg):
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def note(msg):
    path = os.environ.get("READER_FAKE_LOG")
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(json.dumps(msg, sort_keys=True, ensure_ascii=False) + "\n")


def describe(context):
    if not isinstance(context, dict) or not context:
        return "no context"
    parts = []
    for key in ("file", "page", "page_count"):
        if context.get(key) is not None:
            value = context[key]
            if key == "file":
                value = os.path.basename(str(value))
            parts.append(f"{key} {value}")
    selection = context.get("selection")
    if selection:
        parts.append(f"selection of {len(selection)} chars")
    else:
        parts.append("no selection")
    return "context: " + ", ".join(parts)


def main():
    project = False
    prompts = 0
    emit({"type": "ready", "version": "fake-m5.3"})
    for raw in sys.stdin.buffer:
        line = raw.decode("utf-8", "replace").rstrip("\r\n")
        print(f"fake-m5.3: {line}", file=sys.stderr, flush=True)
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        if not isinstance(msg, dict):
            continue
        kind = msg.get("type")
        if kind == "open_project":
            project = True
        elif kind in ("new_chat", "resume_chat"):
            chat = msg.get("chat_id")
            if project:
                emit({"type": "chat_started", "chat_id": chat, "session_id": msg.get("session_id")})
            else:
                emit({"type": "error", "chat_id": chat, "kind": "no_project",
                      "message": "no project is open", "seq": msg.get("seq")})
        elif kind == "prompt":
            prompts += 1
            note(msg)
            chat = msg.get("chat_id")
            text = f"Fake reply to prompt {prompts}, {describe(msg.get('context'))}."
            emit({"type": "text_delta", "chat_id": chat, "text": text})
            emit({"type": "result", "chat_id": chat, "session_id": "fake-session-m53",
                  "usage": {"input_tokens": 10, "output_tokens": 5}, "cost_usd": 0.001})


if __name__ == "__main__":
    main()
