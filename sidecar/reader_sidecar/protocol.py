"""Messages between the app and the sidecar, one JSON object per line.

sidecar/PROTOCOL.md describes the same messages for the C++ side. Keep the
two in sync.
"""

import dataclasses
import json
from dataclasses import dataclass, field
from typing import Any, Literal


class ProtocolError(Exception):
    """An inbound line that is not a valid message.

    `seq` holds the line's `seq` when the line was a JSON object with an
    integer `seq`, so the error reply can echo it. Otherwise it is None.
    """

    def __init__(self, message: str, seq: int | None = None) -> None:
        super().__init__(message)
        self.seq = seq


@dataclass(kw_only=True)
class Context:
    """What the user is looking at when they send a prompt."""

    file: str | None = None
    page: int | None = None
    page_count: int | None = None
    selection: str | None = None


# Inbound: app to sidecar.


@dataclass(kw_only=True)
class OpenProject:
    type: Literal["open_project"] = field(default="open_project", init=False)
    path: str
    seq: int | None = None


@dataclass(kw_only=True)
class Prompt:
    type: Literal["prompt"] = field(default="prompt", init=False)
    chat_id: str
    text: str
    context: Context | None = None
    seq: int | None = None


@dataclass(kw_only=True)
class NewChat:
    type: Literal["new_chat"] = field(default="new_chat", init=False)
    chat_id: str
    seq: int | None = None


@dataclass(kw_only=True)
class ResumeChat:
    type: Literal["resume_chat"] = field(default="resume_chat", init=False)
    chat_id: str
    session_id: str
    seq: int | None = None


@dataclass(kw_only=True)
class Interrupt:
    type: Literal["interrupt"] = field(default="interrupt", init=False)
    chat_id: str
    seq: int | None = None


@dataclass(kw_only=True)
class PermissionReply:
    type: Literal["permission_reply"] = field(default="permission_reply", init=False)
    req_id: str
    allow: bool
    seq: int | None = None


Inbound = OpenProject | Prompt | NewChat | ResumeChat | Interrupt | PermissionReply


# Outbound: sidecar to app.


@dataclass(kw_only=True)
class Ready:
    type: Literal["ready"] = field(default="ready", init=False)
    version: str


@dataclass(kw_only=True)
class TextDelta:
    type: Literal["text_delta"] = field(default="text_delta", init=False)
    chat_id: str
    text: str


@dataclass(kw_only=True)
class ToolUse:
    type: Literal["tool_use"] = field(default="tool_use", init=False)
    chat_id: str
    name: str
    input: dict[str, Any]


@dataclass(kw_only=True)
class PermissionRequest:
    type: Literal["permission_request"] = field(default="permission_request", init=False)
    req_id: str
    chat_id: str
    tool: str
    input: dict[str, Any]


@dataclass(kw_only=True)
class Result:
    type: Literal["result"] = field(default="result", init=False)
    chat_id: str
    session_id: str
    usage: dict[str, Any]
    cost_usd: float


@dataclass(kw_only=True)
class Error:
    type: Literal["error"] = field(default="error", init=False)
    chat_id: str | None = None
    kind: str
    message: str
    seq: int | None = None


@dataclass(kw_only=True)
class ChatStarted:
    type: Literal["chat_started"] = field(default="chat_started", init=False)
    chat_id: str
    session_id: str | None


Outbound = Ready | TextDelta | ToolUse | PermissionRequest | Result | Error | ChatStarted

_INBOUND: dict[str, type[Inbound]] = {
    cls.type: cls
    for cls in (OpenProject, Prompt, NewChat, ResumeChat, Interrupt, PermissionReply)
}
_CONTEXT_FIELDS = tuple(f.name for f in dataclasses.fields(Context))


def decode(line: str) -> Inbound:
    """Parse one inbound line. Raises ProtocolError if it is not a valid message.

    A missing or null optional field becomes None. Unknown keys are ignored.
    """
    try:
        obj = json.loads(line)
    except json.JSONDecodeError as exc:
        raise ProtocolError(f"invalid JSON: {exc}") from None
    if not isinstance(obj, dict):
        raise ProtocolError("message is not a JSON object")

    seq = obj.get("seq")
    if not isinstance(seq, int) or isinstance(seq, bool):
        seq = None

    name = obj.get("type")
    if name is None:
        raise ProtocolError("missing field: type", seq)
    cls = _INBOUND.get(name) if isinstance(name, str) else None
    if cls is None:
        raise ProtocolError(f"unknown type: {name}", seq)

    kwargs: dict[str, Any] = {}
    for f in dataclasses.fields(cls):
        if not f.init:
            continue
        value = obj.get(f.name)
        if value is None:
            if f.default is dataclasses.MISSING:
                raise ProtocolError(f"{name}: missing field: {f.name}", seq)
            continue
        kwargs[f.name] = value

    if "context" in kwargs:
        kwargs["context"] = _decode_context(kwargs["context"], seq)
    return cls(**kwargs)


def _decode_context(value: Any, seq: int | None) -> Context:
    if not isinstance(value, dict):
        raise ProtocolError("prompt: context is not a JSON object", seq)
    return Context(**{key: value.get(key) for key in _CONTEXT_FIELDS})


def encode(msg: Outbound) -> str:
    """Serialize one outbound message as a single line without a trailing newline.

    The result is pure ASCII. json.dumps escapes control characters and every
    non-ASCII character, so no line separator of any kind can appear in it.
    """
    return json.dumps(dataclasses.asdict(msg), separators=(",", ":"))
