"""Spec tests for reader_sidecar.protocol (task M4.1).

The task spec fixes decode, encode, ProtocolError, and one dataclass per
message with a `type` field. This file fixes the rest. The implementation
must follow these conventions.

1. Module `reader_sidecar.protocol` exports the classes below, plus
   `decode(line: str) -> Inbound`, `encode(msg: Outbound) -> str` and
   `ProtocolError` (an Exception subclass).

2. One dataclass per message. Wire type to class name:
     inbound   open_project OpenProject, prompt Prompt, new_chat NewChat,
               resume_chat ResumeChat, interrupt Interrupt,
               permission_reply PermissionReply
     outbound  ready Ready, text_delta TextDelta, tool_use ToolUse,
               permission_request PermissionRequest, result Result,
               error Error, chat_started ChatStarted

3. `type` is a regular dataclass field holding the wire name. Callers never
   pass it. It has a default (for example `field(default="ready",
   init=False)`), so `dataclasses.asdict(msg)` includes it.

4. Messages are built with keyword arguments. Field names equal the wire keys.
   A field marked `?` is optional and defaults to None. Others are required.
     open_project        path, seq?
     prompt              chat_id, text, context?, seq?
     new_chat            chat_id, seq?
     resume_chat         chat_id, session_id, seq?
     interrupt           chat_id, seq?
     permission_reply    req_id, allow, seq?
     ready               version
     text_delta          chat_id, text
     tool_use            chat_id, name, input
     permission_request  req_id, chat_id, tool, input
     result              chat_id, session_id, usage, cost_usd
     error               chat_id?, kind, message, seq?
     chat_started        chat_id, session_id (str or None)
   `input` and `usage` are plain dicts. `cost_usd` is a float.
   resume_chat carries chat_id (M4.3, M6.4) so chat_started can answer it.
   permission_request carries chat_id (M4.4).

5. `Context` is a dataclass without a `type` field. Its fields are file, page,
   page_count and selection, each optional with default None. decode turns
   the wire `context` object into a Context. A missing or null `context`
   decodes to None.

6. decode: a missing or null optional field decodes to None. It raises
   ProtocolError for invalid JSON, JSON that is not an object, a missing
   `type`, a `type` that is not an inbound message, or a missing required
   field. A trailing newline on the line is accepted.

7. encode: returns one JSON object holding `type` and every dataclass field,
   with None written as null. The result has no newline anywhere, including
   at the end. The writer (M4.2) appends "\\n".
"""

import dataclasses
import json

import pytest

from reader_sidecar.protocol import (
    ChatStarted,
    Context,
    Error,
    Interrupt,
    NewChat,
    OpenProject,
    PermissionReply,
    PermissionRequest,
    Prompt,
    ProtocolError,
    Ready,
    Result,
    ResumeChat,
    TextDelta,
    ToolUse,
    decode,
    encode,
)

CHAT = "6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10"
SESSION = "b2d4e6f8-1a3c-4e5f-8a9b-0c1d2e3f4a5b"
REQ = "9e8d7c6b-5a49-4382-a1b0-c9d8e7f6a5b4"

FULL_CONTEXT = {
    "file": "papers/attention.pdf",
    "page": 12,
    "page_count": 15,
    "selection": "Scaled dot-product attention",
}

# (wire object without seq, expected decoded message with seq None)
INBOUND = [
    (
        {"type": "open_project", "path": "/home/u/MyProject"},
        OpenProject(path="/home/u/MyProject"),
    ),
    (
        {
            "type": "prompt",
            "chat_id": CHAT,
            "text": "Explain this.\nBriefly.",
            "context": FULL_CONTEXT,
        },
        Prompt(
            chat_id=CHAT,
            text="Explain this.\nBriefly.",
            context=Context(**FULL_CONTEXT),
        ),
    ),
    (
        {"type": "new_chat", "chat_id": CHAT},
        NewChat(chat_id=CHAT),
    ),
    (
        {"type": "resume_chat", "chat_id": CHAT, "session_id": SESSION},
        ResumeChat(chat_id=CHAT, session_id=SESSION),
    ),
    (
        {"type": "interrupt", "chat_id": CHAT},
        Interrupt(chat_id=CHAT),
    ),
    (
        {"type": "permission_reply", "req_id": REQ, "allow": True},
        PermissionReply(req_id=REQ, allow=True),
    ),
    (
        {"type": "permission_reply", "req_id": REQ, "allow": False},
        PermissionReply(req_id=REQ, allow=False),
    ),
]
INBOUND_IDS = [
    "open_project",
    "prompt",
    "new_chat",
    "resume_chat",
    "interrupt",
    "permission_reply_allow",
    "permission_reply_deny",
]

# (message, expected wire object)
OUTBOUND = [
    (
        Ready(version="0.1.0"),
        {"type": "ready", "version": "0.1.0"},
    ),
    (
        TextDelta(chat_id=CHAT, text="Attention, café π"),
        {"type": "text_delta", "chat_id": CHAT, "text": "Attention, café π"},
    ),
    (
        ToolUse(
            chat_id=CHAT,
            name="Read",
            input={"file_path": "papers/attention.pdf", "pages": "12"},
        ),
        {
            "type": "tool_use",
            "chat_id": CHAT,
            "name": "Read",
            "input": {"file_path": "papers/attention.pdf", "pages": "12"},
        },
    ),
    (
        PermissionRequest(req_id=REQ, chat_id=CHAT, tool="Bash", input={"command": "ls"}),
        {
            "type": "permission_request",
            "req_id": REQ,
            "chat_id": CHAT,
            "tool": "Bash",
            "input": {"command": "ls"},
        },
    ),
    (
        Result(
            chat_id=CHAT,
            session_id=SESSION,
            usage={"input_tokens": 1200, "output_tokens": 340},
            cost_usd=0.0123,
        ),
        {
            "type": "result",
            "chat_id": CHAT,
            "session_id": SESSION,
            "usage": {"input_tokens": 1200, "output_tokens": 340},
            "cost_usd": 0.0123,
        },
    ),
    (
        Error(chat_id=CHAT, kind="busy", message="chat is busy", seq=4),
        {"type": "error", "chat_id": CHAT, "kind": "busy", "message": "chat is busy", "seq": 4},
    ),
    (
        Error(kind="protocol", message="unknown type: bogus"),
        {
            "type": "error",
            "chat_id": None,
            "kind": "protocol",
            "message": "unknown type: bogus",
            "seq": None,
        },
    ),
    (
        ChatStarted(chat_id=CHAT, session_id=SESSION),
        {"type": "chat_started", "chat_id": CHAT, "session_id": SESSION},
    ),
    (
        ChatStarted(chat_id=CHAT, session_id=None),
        {"type": "chat_started", "chat_id": CHAT, "session_id": None},
    ),
]
OUTBOUND_IDS = [
    "ready",
    "text_delta",
    "tool_use",
    "permission_request",
    "result",
    "error",
    "error_without_chat",
    "chat_started_resumed",
    "chat_started_new",
]

# (inbound type, required field)
REQUIRED = [
    ("open_project", "path"),
    ("prompt", "chat_id"),
    ("prompt", "text"),
    ("new_chat", "chat_id"),
    ("resume_chat", "chat_id"),
    ("resume_chat", "session_id"),
    ("interrupt", "chat_id"),
    ("permission_reply", "req_id"),
    ("permission_reply", "allow"),
]
INBOUND_WIRE = {wire["type"]: wire for wire, _ in INBOUND}


@pytest.mark.parametrize("seq", [None, 7], ids=["no_seq", "seq"])
@pytest.mark.parametrize(("wire", "expected"), INBOUND, ids=INBOUND_IDS)
def test_inbound_round_trip(wire, expected, seq):
    line = json.dumps(wire if seq is None else {**wire, "seq": seq})

    msg = decode(line)

    assert type(msg) is type(expected)
    assert msg == dataclasses.replace(expected, seq=seq)
    assert msg.type == wire["type"]
    assert dataclasses.asdict(msg) == {**wire, "seq": seq}


def test_decode_null_seq_is_none():
    msg = decode(json.dumps({"type": "interrupt", "chat_id": CHAT, "seq": None}))

    assert msg == Interrupt(chat_id=CHAT)
    assert msg.seq is None


def test_decode_partial_context():
    line = json.dumps(
        {"type": "prompt", "chat_id": CHAT, "text": "hi", "context": {"file": "a.pdf", "page": 2}}
    )

    msg = decode(line)

    assert msg.context == Context(file="a.pdf", page=2)
    assert msg.context.page_count is None
    assert msg.context.selection is None


@pytest.mark.parametrize("extra", [{}, {"context": None}], ids=["missing", "null"])
def test_decode_prompt_without_context(extra):
    msg = decode(json.dumps({"type": "prompt", "chat_id": CHAT, "text": "hi", **extra}))

    assert msg == Prompt(chat_id=CHAT, text="hi")
    assert msg.context is None


def test_decode_accepts_trailing_newline():
    line = json.dumps({"type": "new_chat", "chat_id": CHAT}) + "\n"

    assert decode(line) == NewChat(chat_id=CHAT)


@pytest.mark.parametrize(("msg", "wire"), OUTBOUND, ids=OUTBOUND_IDS)
def test_outbound_round_trip(msg, wire):
    line = encode(msg)

    assert isinstance(line, str)
    assert "\n" not in line
    assert "\r" not in line
    assert json.loads(line) == wire
    assert msg.type == wire["type"]
    assert dataclasses.asdict(msg) == wire
    fields = {key: value for key, value in wire.items() if key != "type"}
    assert type(msg)(**fields) == msg


@pytest.mark.parametrize(
    "msg",
    [
        TextDelta(chat_id=CHAT, text="first line\nsecond line\r\nthird\n"),
        Error(chat_id=CHAT, kind="internal", message="Traceback:\n  line 1\n", seq=3),
        ToolUse(chat_id=CHAT, name="Write", input={"content": "a\nb"}),
    ],
    ids=["text_delta", "error", "tool_use"],
)
def test_encode_has_no_embedded_newline(msg):
    line = encode(msg)

    assert "\n" not in line
    assert "\r" not in line
    assert json.loads(line) == dataclasses.asdict(msg)


@pytest.mark.parametrize(
    "line",
    ["", "   ", "{not json", '{"type": "new_chat", "chat_id": ', "{'type': 'new_chat'}"],
    ids=["empty", "blank", "garbage", "truncated", "single_quotes"],
)
def test_decode_rejects_invalid_json(line):
    with pytest.raises(ProtocolError):
        decode(line)


@pytest.mark.parametrize(
    "line",
    ["[]", "42", "null", '"new_chat"'],
    ids=["array", "number", "null", "string"],
)
def test_decode_rejects_non_object(line):
    with pytest.raises(ProtocolError):
        decode(line)


@pytest.mark.parametrize("type_name", ["bogus", "", "text_delta", "chat_started"])
def test_decode_rejects_unknown_type(type_name):
    with pytest.raises(ProtocolError):
        decode(json.dumps({"type": type_name, "chat_id": CHAT}))


def test_decode_rejects_missing_type():
    with pytest.raises(ProtocolError):
        decode(json.dumps({"chat_id": CHAT}))


@pytest.mark.parametrize(
    ("type_name", "field"),
    REQUIRED,
    ids=[f"{type_name}.{field}" for type_name, field in REQUIRED],
)
def test_decode_rejects_missing_required_field(type_name, field):
    wire = dict(INBOUND_WIRE[type_name])
    del wire[field]

    with pytest.raises(ProtocolError):
        decode(json.dumps(wire))
