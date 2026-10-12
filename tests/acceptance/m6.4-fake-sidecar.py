#!/usr/bin/env python3
"""Fake agent sidecar for tests/acceptance/m6.4.sh. Never contacts the network.

Speaks sidecar/PROTOCOL.md over stdin/stdout and behaves like the real
sidecar where M6.4 depends on it: chat ids are per process (a restarted
sidecar has no open chats), a second new_chat/resume_chat for an open chat_id
gets `chat_exists`, and a resume of a session that cannot be resumed is
accepted with `chat_started` and fails at the first prompt, the way the
Claude CLI fails ("No conversation found with session ID ...").

  start         writes `ready` (version "fake-m6.4").
  open_project  remembers the path. A different path closes every chat. No
                reply.
  new_chat      `chat_started` (session_id null), or `error` no_project
                before any open_project, or chat_exists.
  resume_chat   the same checks, plus `error` kind protocol when session_id
                is missing or empty. Otherwise `chat_started` with the
                session_id. A session_id starting "sess-doomed-" cannot be
                resumed: the chat opens, and its first prompt fails (below).
  prompt        `error` unknown_chat for a chat that is not open. Otherwise,
                first match wins, by the end of the prompt's text:
    chat resumed from a "sess-doomed-" session:
                  `error` kind process, "No conversation found with session
                  ID: <session_id>". The chat is closed.
    "#crash"      one text_delta ("Working on it"), then the process exits
                  with status 3, like a crashed sidecar.
    "#fail"       one text_delta ("Checking the document... "), then `error`
                  kind result ("error_max_turns: the reply hit the turn
                  limit") in place of `result`. The chat stays open and still
                  has no session.
    otherwise     a reply in 3 or 4 text_delta pieces, READER_FAKE_DELAY
                  seconds apart (default 0.2), then `result` with cost_usd
                  0.0010, input_tokens 10, output_tokens 5. The reply reads
                    "Answer <n> in this session. You said: <last line of the
                    text, at most 80 characters>."
                  where n counts the replies of the session across processes,
                  and for a chat opened with resume_chat it goes on
                    " Earlier in this session you first asked: <the
                    session's first prompt, at most 80 characters>."
                  The session_id: the chat's resumed session, else the one
                  its first reply got: "sess-<chat_id>", or
                  "sess-doomed-<chat_id>" when that first prompt ended with
                  "#doom".
  interrupt, permission_reply   ignored (replies are not interruptible).
  anything else `error` kind protocol.

READER_FAKE_STATE (a directory, required for resume memory) keeps each
session's first prompt and reply count across processes and runs.

READER_FAKE_LOG, when set, gets one JSON object per line, appended:
  {"fake": "start", "pid": N}                       a process started
  <each line received, as received>                 objects with "type"
  {"fake": "reply", "chat_id", "session_id", "text"}   a reply ended with
                                                    `result`; text is the
                                                    whole reply
  {"fake": "error", "chat_id", "kind", "message"}   an error was sent
  {"fake": "crash", "pid": N}                       about to exit for "#crash"
  {"fake": "eof", "pid": N}                         stdin closed
stderr (sidecar.log) gets one short line per message, never prompt text.
"""

import json
import os
import sys
import time

DELAY = float(os.environ.get("READER_FAKE_DELAY", "0.2"))
LOG = os.environ.get("READER_FAKE_LOG", "")
STATE = os.environ.get("READER_FAKE_STATE", "")


def log(obj):
    if not LOG:
        return
    line = obj if isinstance(obj, str) else json.dumps(obj)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def emit(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def send_error(chat_id, kind, message, seq):
    log({"fake": "error", "chat_id": chat_id, "kind": kind, "message": message})
    emit({"type": "error", "chat_id": chat_id, "kind": kind, "message": message, "seq": seq})


def session_file(session_id):
    return os.path.join(STATE, session_id + ".json") if STATE else ""


def load_session(session_id):
    path = session_file(session_id)
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    return {"first": None, "replies": 0}


def save_session(session_id, data):
    path = session_file(session_id)
    if not path:
        return
    os.makedirs(STATE, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f)


def short(text, limit=80):
    return text[:limit]


def last_line(text):
    lines = [line for line in text.strip().split("\n") if line.strip()]
    return lines[-1].strip() if lines else ""


class Chat:
    def __init__(self, session_id):
        self.session_id = session_id  # None until the first result
        self.resumed = session_id is not None
        self.doomed = bool(session_id) and session_id.startswith("sess-doomed-")


def main():
    project = None
    chats = {}
    pid = os.getpid()
    log({"fake": "start", "pid": pid})
    emit({"type": "ready", "version": "fake-m6.4"})

    for raw in sys.stdin:
        line = raw.rstrip("\r\n")
        log(line)
        try:
            msg = json.loads(line)
        except ValueError:
            msg = None
        if not isinstance(msg, dict):
            send_error(None, "protocol", "not a valid app message", None)
            continue
        kind = msg.get("type")
        seq = msg.get("seq") if isinstance(msg.get("seq"), int) else None
        chat_id = msg.get("chat_id")
        print(f"fake-m6.4: {kind} {chat_id or ''}", file=sys.stderr, flush=True)

        if kind == "open_project":
            path = msg.get("path")
            if path != project:
                chats.clear()
            project = path
        elif kind in ("new_chat", "resume_chat"):
            session_id = msg.get("session_id") if kind == "resume_chat" else None
            if project is None:
                send_error(chat_id, "no_project", "no project is open", seq)
            elif chat_id in chats:
                send_error(chat_id, "chat_exists", "chat is already open", seq)
            elif kind == "resume_chat" and not session_id:
                send_error(chat_id, "protocol", "resume_chat needs a session_id", seq)
            else:
                chats[chat_id] = Chat(session_id)
                emit({"type": "chat_started", "chat_id": chat_id, "session_id": session_id})
        elif kind == "prompt":
            chat = chats.get(chat_id)
            text = msg.get("text") or ""
            if chat is None:
                send_error(chat_id, "unknown_chat", "chat is not open", seq)
                continue
            if chat.doomed and chat.resumed:
                del chats[chat_id]
                send_error(chat_id, "process",
                           f"No conversation found with session ID: {chat.session_id}", seq)
                continue
            stripped = text.rstrip()
            if stripped.endswith("#crash"):
                time.sleep(DELAY)
                emit({"type": "text_delta", "chat_id": chat_id, "text": "Working on it"})
                time.sleep(DELAY)
                log({"fake": "crash", "pid": pid})
                os._exit(3)
            if stripped.endswith("#fail"):
                time.sleep(DELAY)
                emit({"type": "text_delta", "chat_id": chat_id, "text": "Checking the document... "})
                time.sleep(DELAY)
                send_error(chat_id, "result", "error_max_turns: the reply hit the turn limit", seq)
                continue
            if chat.session_id is None:
                prefix = "sess-doomed-" if stripped.endswith("#doom") else "sess-"
                chat.session_id = prefix + chat_id
            data = load_session(chat.session_id)
            if data["first"] is None:
                data["first"] = text
            data["replies"] += 1
            save_session(chat.session_id, data)
            pieces = [f"Answer {data['replies']} ", "in this session. ",
                      f"You said: {short(last_line(text))}."]
            if chat.resumed:
                pieces.append(f" Earlier in this session you first asked: {short(data['first'])}.")
            for piece in pieces:
                time.sleep(DELAY)
                emit({"type": "text_delta", "chat_id": chat_id, "text": piece})
            time.sleep(DELAY)
            log({"fake": "reply", "chat_id": chat_id, "session_id": chat.session_id,
                 "text": "".join(pieces)})
            emit({"type": "result", "chat_id": chat_id, "session_id": chat.session_id,
                  "usage": {"input_tokens": 10, "output_tokens": 5}, "cost_usd": 0.0010})
        elif kind in ("interrupt", "permission_reply"):
            pass
        else:
            send_error(None, "protocol", "not a valid app message", seq)

    log({"fake": "eof", "pid": pid})


if __name__ == "__main__":
    main()
