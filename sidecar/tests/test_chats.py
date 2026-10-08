"""Spec tests for chat sessions on the Agent SDK (task M4.3).

The task spec fixes the ClaudeAgentOptions, the message mapping, the busy
rule, and a ClientFactory for injecting a fake client. It does not fix the
module interface. These tests fix it as follows. The implementation must
follow these conventions.

1. `reader_sidecar.main.Router(writer, factory=...)`. `factory` is the
   ClientFactory: any callable `factory(options: ClaudeAgentOptions) -> client`.
   It defaults to `claude_agent_sdk.ClaudeSDKClient`, so `Router(writer)` still
   works (tests/test_io.py builds it that way). The chat registry may live in
   `reader_sidecar.chats`. These tests do not import it. They run
   `main.serve` with that Router, feed it protocol lines, and read its output.

2. new_chat and resume_chat call the factory exactly once, then send
   `chat_started`. The client may connect then or at the first prompt,
   through `connect()` or `async with`, but before `query`.

3. prompt calls `client.query(text)` with a str that contains the prompt's
   text, then iterates `client.receive_response()` in a background task. The
   handler returns while the reply streams, so serve handles the next line.

4. Mapping of SDK messages to outbound messages:
   a. A StreamEvent whose event is `content_block_delta` with a delta of type
      `text_delta` becomes `text_delta {text: delta.text}`. Every other
      StreamEvent is dropped, including thinking and tool-input deltas.
   b. Each ToolUseBlock of an AssistantMessage becomes `tool_use {name, input}`.
      TextBlocks are dropped, because their text already streamed as deltas.
   c. ResultMessage becomes `result {session_id, usage, cost_usd}`. `usage` is
      ResultMessage.usage unchanged, or `{}` when it is None. `cost_usd` is
      total_cost_usd, or 0.0 when it is None.
   d. SystemMessage and UserMessage are dropped.

5. Tool input truncation applies to top-level string values. A string longer
   than 500 chars keeps its first 500 chars and may be followed by a marker of
   at most 20 chars. Shorter strings and non-string values pass unchanged.

6. A prompt for a chat whose reply is still running gets
   `error {chat_id, kind: "busy", message, seq: <the prompt's seq>}` and never
   reaches the client. The chat accepts prompts again once its `result` is
   sent. Other chats are not affected.

7. open_project with a different path closes every chat (its client is
   disconnected if it was connected) and forgets it. A later prompt to a
   forgotten chat_id gets an `error` (any kind) with that chat_id and seq.
   open_project with the same path keeps the chats. `Router.close()`, which
   serve runs at EOF, closes every chat.

8. Options for each client: `cwd` is the project path (str or Path),
   `setting_sources=["project"]`, `system_prompt={"type": "preset",
   "preset": "claude_code", "append": reader_sidecar.prompts.READER_PROMPT}`,
   `include_partial_messages=True`, `allowed_tools=["Read", "Glob", "Grep"]`,
   `can_use_tool` is set (M4.4 owns its behaviour), `resume` is the session_id
   for resume_chat and None for new_chat, and `model` is $READER_MODEL when
   set, else None.

Not covered here: the reader_context prefix and can_use_tool behaviour (M4.4),
error mapping (M4.5), and new_chat before any open_project (the spec is
silent).
"""

import asyncio
import io
import json
import queue
from pathlib import Path
from typing import Any

import pytest
from claude_agent_sdk import (
    AssistantMessage,
    ResultMessage,
    StreamEvent,
    SystemMessage,
    TextBlock,
    ToolResultBlock,
    ToolUseBlock,
    UserMessage,
)

from fakes import FakeClient, FakeFactory
from reader_sidecar.io import Writer
from reader_sidecar.main import Router, serve
from reader_sidecar.prompts import READER_PROMPT

pytestmark = pytest.mark.asyncio

TIMEOUT = 5.0
SESSION = "b2d4e6f8-1a3c-4e5f-8a9b-0c1d2e3f4a5b"
MODEL = "claude-sonnet-4-5"
USAGE = {
    "input_tokens": 1200,
    "output_tokens": 340,
    "cache_creation_input_tokens": 0,
    "cache_read_input_tokens": 512,
}
TOOL_INPUT = {"file_path": "papers/attention.pdf", "pages": "4"}


# Scripted SDK messages


def stream(event: dict[str, Any]) -> StreamEvent:
    return StreamEvent(uuid="evt", session_id=SESSION, event=event)


def block_start(index: int, block: dict[str, Any]) -> StreamEvent:
    return stream({"type": "content_block_start", "index": index, "content_block": block})


def block_delta(index: int, body: dict[str, Any]) -> StreamEvent:
    return stream({"type": "content_block_delta", "index": index, "delta": body})


def block_stop(index: int) -> StreamEvent:
    return stream({"type": "content_block_stop", "index": index})


def delta(text: str) -> StreamEvent:
    return block_delta(1, {"type": "text_delta", "text": text})


def tool_use(name: str, tool_input: dict[str, Any]) -> AssistantMessage:
    block = ToolUseBlock(id="toolu_1", name=name, input=tool_input)
    return AssistantMessage(content=[block], model=MODEL, session_id=SESSION)


def result(cost: float | None = 0.0123, usage: dict[str, Any] | None = USAGE) -> ResultMessage:
    return ResultMessage(
        subtype="success",
        duration_ms=1500,
        duration_api_ms=1200,
        is_error=False,
        num_turns=2,
        session_id=SESSION,
        stop_reason="end_turn",
        total_cost_usd=cost,
        usage=usage,
        result="Let me read page 4.",
    )


def full_turn() -> list[Any]:
    """A reply as the SDK yields it with include_partial_messages=True."""
    tool_block = {"type": "tool_use", "id": "toolu_1", "name": "Read", "input": {}}
    return [
        SystemMessage(subtype="init", data={"type": "system", "subtype": "init", "session_id": SESSION}),
        stream({"type": "message_start", "message": {"id": "msg_1", "role": "assistant", "content": []}}),
        block_start(0, {"type": "thinking", "thinking": ""}),
        block_delta(0, {"type": "thinking_delta", "thinking": "Hmm."}),
        block_stop(0),
        block_start(1, {"type": "text", "text": ""}),
        delta("Let me read "),
        delta("page 4."),
        block_stop(1),
        AssistantMessage(content=[TextBlock(text="Let me read page 4.")], model=MODEL, session_id=SESSION),
        block_start(2, tool_block),
        block_delta(2, {"type": "input_json_delta", "partial_json": json.dumps(TOOL_INPUT)}),
        block_stop(2),
        tool_use("Read", dict(TOOL_INPUT)),
        stream({"type": "message_delta", "delta": {"stop_reason": "tool_use"}}),
        stream({"type": "message_stop"}),
        UserMessage(content=[ToolResultBlock(tool_use_id="toolu_1", content="Page 4 text.")]),
        result(),
    ]


def short_turn(text: str = "Done.") -> list[Any]:
    return [delta(text), result()]


def result_line(chat_id: str) -> dict[str, Any]:
    return {"type": "result", "chat_id": chat_id, "session_id": SESSION, "usage": USAGE, "cost_usd": 0.0123}


# Harness


class FeedStdin:
    """Binary stdin whose readline blocks until the test feeds a line."""

    def __init__(self) -> None:
        self._lines: queue.Queue[bytes] = queue.Queue()

    def feed(self, data: bytes) -> None:
        self._lines.put(data)

    def readline(self, size: int = -1) -> bytes:
        return self._lines.get()


class Sidecar:
    """serve() with a Router on fake clients. Use as `async with`.

    Leaving the block sends EOF and checks that serve returns 0.
    """

    def __init__(self, *turns: list[Any]) -> None:
        self.out = io.BytesIO()
        self.stdin = FeedStdin()
        self.factory = FakeFactory(*turns)
        self.task: asyncio.Task[int] | None = None

    async def __aenter__(self) -> "Sidecar":
        writer = Writer(self.out)
        router = Router(writer, factory=self.factory)
        self.task = asyncio.create_task(serve(self.stdin, writer, router))
        return self

    async def __aexit__(self, exc_type: Any, *rest: Any) -> None:
        self.stdin.feed(b"")
        if exc_type is not None:
            self.task.cancel()
            return
        assert await asyncio.wait_for(self.task, TIMEOUT) == 0

    @property
    def clients(self) -> list[FakeClient]:
        return self.factory.clients

    def send(self, **msg: Any) -> None:
        self.stdin.feed(json.dumps(msg).encode() + b"\n")

    def sent(self) -> list[dict[str, Any]]:
        return [json.loads(line) for line in self.out.getvalue().splitlines()]

    def replies(self, chat_id: str) -> list[dict[str, Any]]:
        """Messages for `chat_id` other than chat_started."""
        return [m for m in self.sent() if m.get("chat_id") == chat_id and m["type"] != "chat_started"]

    def count(self, kind: str, chat_id: str) -> int:
        return sum(m["type"] == kind and m.get("chat_id") == chat_id for m in self.sent())

    async def wait_for(self, kind: str, chat_id: str, count: int = 1) -> None:
        """Wait until `count` messages of type `kind` were sent for `chat_id`."""
        deadline = asyncio.get_running_loop().time() + TIMEOUT
        while self.count(kind, chat_id) < count:
            if self.task.done() or asyncio.get_running_loop().time() > deadline:
                pytest.fail(f"no {kind} #{count} on {chat_id}. Sent: {self.sent()}")
            await asyncio.sleep(0.01)

    def open(self, path: Path, seq: int | None = None) -> None:
        self.send(type="open_project", path=str(path), seq=seq)

    async def new_chat(self, chat_id: str, seq: int | None = None) -> FakeClient:
        return await self._start(type="new_chat", chat_id=chat_id, seq=seq)

    async def resume_chat(self, chat_id: str, session_id: str, seq: int | None = None) -> FakeClient:
        return await self._start(type="resume_chat", chat_id=chat_id, session_id=session_id, seq=seq)

    async def _start(self, **msg: Any) -> FakeClient:
        before = len(self.clients)
        started = self.count("chat_started", msg["chat_id"])
        self.send(**msg)
        await self.wait_for("chat_started", msg["chat_id"], started + 1)
        assert len(self.clients) == before + 1, f"{msg['type']} must call the factory once"
        return self.clients[-1]

    def prompt(self, chat_id: str, text: str = "Summarize page 4.", seq: int | None = None) -> None:
        self.send(type="prompt", chat_id=chat_id, text=text, context=None, seq=seq)

    async def ask(self, chat_id: str, text: str = "Summarize page 4.", seq: int | None = None) -> None:
        """Send a prompt and wait for one more result on the chat."""
        done = self.count("result", chat_id)
        self.prompt(chat_id, text, seq)
        await self.wait_for("result", chat_id, done + 1)


@pytest.fixture
def project(tmp_path: Path) -> Path:
    path = tmp_path / "project"
    path.mkdir()
    return path


# chat_started


async def test_new_chat_answers_chat_started_with_null_session(project):
    async with Sidecar() as sc:
        sc.open(project, seq=1)
        await sc.new_chat("c1", seq=2)
        assert sc.sent()[1:] == [{"type": "chat_started", "chat_id": "c1", "session_id": None}]


async def test_resume_chat_answers_chat_started_with_its_session(project):
    async with Sidecar() as sc:
        sc.open(project, seq=1)
        await sc.resume_chat("c1", SESSION, seq=2)
        assert sc.sent()[1:] == [{"type": "chat_started", "chat_id": "c1", "session_id": SESSION}]


# Options


@pytest.mark.parametrize("resume", [False, True], ids=["new_chat", "resume_chat"])
async def test_client_options(project, resume, monkeypatch):
    monkeypatch.delenv("READER_MODEL", raising=False)
    async with Sidecar() as sc:
        sc.open(project)
        client = await (sc.resume_chat("c1", SESSION) if resume else sc.new_chat("c1"))
    options = client.options

    assert options.cwd is not None
    assert Path(options.cwd).resolve() == project.resolve()
    assert options.setting_sources == ["project"]
    assert options.include_partial_messages is True
    assert options.allowed_tools == ["Read", "Glob", "Grep"]
    assert isinstance(options.system_prompt, dict)
    assert options.system_prompt["type"] == "preset"
    assert options.system_prompt["preset"] == "claude_code"
    assert options.system_prompt["append"] == READER_PROMPT
    assert callable(options.can_use_tool)
    assert options.resume == (SESSION if resume else None)
    assert options.model is None


@pytest.mark.parametrize("resume", [False, True], ids=["new_chat", "resume_chat"])
async def test_model_comes_from_reader_model(project, resume, monkeypatch):
    monkeypatch.setenv("READER_MODEL", "claude-opus-4-1")
    async with Sidecar() as sc:
        sc.open(project)
        client = await (sc.resume_chat("c1", SESSION) if resume else sc.new_chat("c1"))
    assert client.options.model == "claude-opus-4-1"


# Streaming


async def test_reply_streams_deltas_then_tool_use_then_result(project):
    async with Sidecar(full_turn()) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        await sc.ask("c1", "Summarize page 4.", seq=4)

        assert len(client.queries) == 1
        assert isinstance(client.queries[0], str)
        assert "Summarize page 4." in client.queries[0]
        assert sc.replies("c1") == [
            {"type": "text_delta", "chat_id": "c1", "text": "Let me read "},
            {"type": "text_delta", "chat_id": "c1", "text": "page 4."},
            {"type": "tool_use", "chat_id": "c1", "name": "Read", "input": TOOL_INPUT},
            result_line("c1"),
        ]


async def test_resumed_chat_streams_its_reply(project):
    async with Sidecar(short_turn("Welcome back.")) as sc:
        sc.open(project)
        await sc.resume_chat("c1", SESSION)
        await sc.ask("c1")
        assert sc.replies("c1") == [
            {"type": "text_delta", "chat_id": "c1", "text": "Welcome back."},
            result_line("c1"),
        ]


async def test_tool_use_input_values_over_500_chars_are_truncated(project):
    exact = "e" * 500
    over = "o" * 501
    long = "".join(chr(ord("a") + i % 26) for i in range(5000))
    tool_input = {"pattern": long, "path": over, "glob": exact, "head_limit": 3, "-n": True}
    async with Sidecar([tool_use("Grep", dict(tool_input)), result()]) as sc:
        sc.open(project)
        await sc.new_chat("c1")
        await sc.ask("c1")
        [msg] = [m for m in sc.replies("c1") if m["type"] == "tool_use"]

    assert msg["name"] == "Grep"
    sent = msg["input"]
    assert set(sent) == set(tool_input)
    assert sent["glob"] == exact
    assert sent["head_limit"] == 3
    assert sent["-n"] is True
    for key in ("pattern", "path"):
        value = sent[key]
        assert isinstance(value, str), key
        assert value != tool_input[key], key
        assert value[:500] == tool_input[key][:500], key
        assert len(value) <= 520, key


async def test_result_without_cost_or_usage_sends_zero_and_empty(project):
    async with Sidecar([delta("Hi."), result(cost=None, usage=None)]) as sc:
        sc.open(project)
        await sc.new_chat("c1")
        await sc.ask("c1")
        [msg] = [m for m in sc.replies("c1") if m["type"] == "result"]
    assert msg == {"type": "result", "chat_id": "c1", "session_id": SESSION, "usage": {}, "cost_usd": 0.0}


# Busy


async def test_second_prompt_while_busy_gets_busy_error(project):
    gate = asyncio.Event()
    first = [delta("Let me "), gate, delta("read."), result()]
    async with Sidecar(first, short_turn("Again.")) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")

        sc.prompt("c1", "first", seq=4)
        await sc.wait_for("text_delta", "c1")
        sc.prompt("c1", "second", seq=7)
        await sc.wait_for("error", "c1")

        [busy] = [m for m in sc.replies("c1") if m["type"] == "error"]
        assert busy["kind"] == "busy"
        assert busy["seq"] == 7
        assert isinstance(busy["message"], str)
        assert len(client.queries) == 1
        assert "first" in client.queries[0]

        gate.set()
        await sc.wait_for("result", "c1")
        assert sc.count("result", "c1") == 1
        assert [m["text"] for m in sc.replies("c1") if m["type"] == "text_delta"] == ["Let me ", "read."]

        # The chat takes prompts again after its result.
        await sc.ask("c1", "third", seq=8)
        assert len(client.queries) == 2
        assert "third" in client.queries[1]
        assert sc.count("error", "c1") == 1


async def test_busy_chat_does_not_block_other_chats(project):
    gate = asyncio.Event()
    async with Sidecar() as sc:
        sc.open(project)
        c1 = await sc.new_chat("c1")
        c2 = await sc.new_chat("c2")
        c1.turns = [[delta("Slow "), gate, result()]]
        c2.turns = [short_turn("Fast.")]

        sc.prompt("c1", seq=4)
        await sc.wait_for("text_delta", "c1")
        await sc.ask("c2", seq=5)
        assert sc.replies("c2") == [
            {"type": "text_delta", "chat_id": "c2", "text": "Fast."},
            result_line("c2"),
        ]
        assert sc.count("error", "c1") == 0
        assert sc.count("result", "c1") == 0

        gate.set()
        await sc.wait_for("result", "c1")


# Closing chats


async def test_project_switch_closes_chats(tmp_path):
    first, second = tmp_path / "first", tmp_path / "second"
    first.mkdir()
    second.mkdir()
    async with Sidecar(short_turn(), short_turn()) as sc:
        sc.open(first)
        c1 = await sc.new_chat("c1")
        await sc.ask("c1")
        c2 = await sc.new_chat("c2")

        sc.open(second)
        c3 = await sc.new_chat("c3")  # serve handles lines in order, so the switch is done.
        assert c1.disconnects >= 1
        assert not c1.connected
        assert not c2.connected
        assert Path(c3.options.cwd).resolve() == second.resolve()

        sc.prompt("c1", seq=9)
        await sc.wait_for("error", "c1")
        [error] = [m for m in sc.replies("c1") if m["type"] == "error"]
        assert error["seq"] == 9
        assert len(c1.queries) == 1

        await sc.ask("c3")


async def test_reopening_the_same_project_keeps_chats(project):
    async with Sidecar(short_turn("One."), short_turn("Two.")) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        await sc.ask("c1")

        sc.open(project)
        await sc.ask("c1")
        assert client.disconnects == 0
        assert len(client.queries) == 2
        assert [m["text"] for m in sc.replies("c1") if m["type"] == "text_delta"] == ["One.", "Two."]
        assert sc.count("error", "c1") == 0


async def test_eof_closes_every_chat(project):
    async with Sidecar(short_turn()) as sc:
        sc.open(project)
        clients = [await sc.new_chat("c1"), await sc.new_chat("c2")]
        await sc.ask("c1")
        await sc.ask("c2")

    for client in clients:
        assert client.disconnects >= 1
        assert not client.connected
