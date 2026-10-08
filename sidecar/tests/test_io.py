"""Spec tests for the stdio loop (task M4.2).

The task spec fixes Writer.send, the daemon stdin thread, `ready` on startup,
protocol errors for malformed lines, a clean exit on EOF, and a Router with one
handler per inbound type. This file fixes the rest. The implementation must
follow these conventions.

1. `reader_sidecar.io.Writer(stream)` wraps a binary stream, meaning an object
   with `write(bytes)` and `flush()` such as `sys.stdout.buffer` or
   `io.BytesIO`. `async def send(msg: Outbound) -> None` takes an
   asyncio.Lock, writes the UTF-8 bytes of `encode(msg) + "\\n"`, and flushes.

2. `reader_sidecar.main.Router(writer)` has one coroutine method per inbound
   type. Each method is named after the wire type and takes the decoded
   message: open_project, prompt, new_chat, resume_chat, interrupt,
   permission_reply. It also has `async def close() -> None`, which closes
   all chats (M4.3). In M4.2 every method is a stub.

3. `async def reader_sidecar.main.serve(stdin, writer, router) -> int` runs the
   loop.
   a. `stdin` is a binary stream. serve calls `stdin.readline()` on a daemon
      thread and hands each line to the event loop through an asyncio.Queue
      with `loop.call_soon_threadsafe`. `b""` means EOF.
   b. serve sends `ready {version: reader_sidecar.__version__}` before it
      handles any line.
   c. Each line is decoded as UTF-8 and passed to `protocol.decode`. A line
      that is not UTF-8, or that decode rejects, gets
      `error {kind: "protocol", seq: ProtocolError.seq}` (seq is null for a
      line that is not UTF-8). serve then handles the next line.
   d. A valid message goes to `getattr(router, msg.type)(msg)`. serve awaits
      that call before it handles the next line.
   e. On EOF serve awaits `router.close()` once and returns 0.
   `router` is any object with the methods in item 2. These tests pass a fake.

4. `reader_sidecar.main.run()` is the `reader-sidecar` console script. It sets
   up logging and the streams (spec items 2 and 4), runs serve on
   `sys.stdin.buffer` with `Writer(sys.stdout.buffer)` and a real Router, and
   exits with serve's return value.

Not covered here: the log level from READER_SIDECAR_LOG, SIGTERM, and Windows
binary mode.
"""

import asyncio
import inspect
import io
import json
import os
import queue
import shutil
import subprocess
import sysconfig
import threading
import time
import typing

import pytest

from reader_sidecar import __version__
from reader_sidecar.io import Writer
from reader_sidecar.main import Router, serve
from reader_sidecar.protocol import (
    Context,
    Error,
    Inbound,
    Interrupt,
    NewChat,
    OpenProject,
    PermissionReply,
    Prompt,
    Ready,
    ResumeChat,
    TextDelta,
    encode,
)

TIMEOUT = 5.0
READY = {"type": "ready", "version": __version__}
ERROR_KEYS = {"type", "chat_id", "kind", "message", "seq"}
NEW_CHAT_LINE = b'{"type":"new_chat","chat_id":"c1"}\n'
INBOUND_TYPES = [cls.type for cls in typing.get_args(Inbound)]


class FakeRouter:
    """Records each handler call as (handler name, message) after it finishes."""

    def __init__(self) -> None:
        self.calls: list[tuple[str, object]] = []

    async def _record(self, name: str, msg: object) -> None:
        await asyncio.sleep(0)
        self.calls.append((name, msg))

    async def open_project(self, msg: OpenProject) -> None:
        await self._record("open_project", msg)

    async def prompt(self, msg: Prompt) -> None:
        await self._record("prompt", msg)

    async def new_chat(self, msg: NewChat) -> None:
        await self._record("new_chat", msg)

    async def resume_chat(self, msg: ResumeChat) -> None:
        await self._record("resume_chat", msg)

    async def interrupt(self, msg: Interrupt) -> None:
        await self._record("interrupt", msg)

    async def permission_reply(self, msg: PermissionReply) -> None:
        await self._record("permission_reply", msg)

    async def close(self) -> None:
        await self._record("close", None)


class FeedStdin:
    """Binary stdin whose readline blocks until the test feeds a line.

    It has no fileno, so the loop cannot read it with connect_read_pipe.
    After close, every readline returns b"".
    """

    def __init__(self) -> None:
        self._lines: queue.Queue[bytes] = queue.Queue()
        self.reader_threads: list[threading.Thread] = []

    def feed(self, data: bytes) -> None:
        self._lines.put(data)

    def close(self) -> None:
        self._lines.put(b"")

    def readline(self, size: int = -1) -> bytes:
        self.reader_threads.append(threading.current_thread())
        data = self._lines.get()
        if data == b"":
            self._lines.put(b"")
        return data


class FlushRecorder:
    """Binary stream that keeps a copy of the written bytes at every flush."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.flushed: list[bytes] = []

    def write(self, data: bytes) -> int:
        self.data += data
        return len(data)

    def flush(self) -> None:
        self.flushed.append(bytes(self.data))


def read_lines(data: bytes) -> list[dict]:
    """Parse sidecar output. Every line must be complete and end with a bare \\n."""
    assert b"\r" not in data, data
    assert data == b"" or data.endswith(b"\n"), data
    return [json.loads(line) for line in data.split(b"\n")[:-1]]


async def run_serve(data: bytes) -> tuple[int, list[dict], FakeRouter]:
    out = io.BytesIO()
    router = FakeRouter()
    rc = await asyncio.wait_for(serve(io.BytesIO(data), Writer(out), router), TIMEOUT)
    return rc, read_lines(out.getvalue()), router


async def wait_until(condition: typing.Callable[[], object]) -> None:
    deadline = time.monotonic() + TIMEOUT
    while not condition():
        assert time.monotonic() < deadline, "timed out"
        await asyncio.sleep(0.01)


def assert_protocol_error(line: dict, seq: int | None) -> None:
    assert set(line) == ERROR_KEYS, line
    assert line["type"] == "error"
    assert line["kind"] == "protocol"
    assert line["seq"] == seq
    assert isinstance(line["message"], str) and line["message"]


# Writer


@pytest.mark.asyncio
async def test_writer_send_writes_one_line_per_message():
    out = io.BytesIO()
    writer = Writer(out)
    msgs = [
        TextDelta(chat_id="c1", text="line one\nline two, café ∑"),
        Ready(version="0.1.0"),
        Error(kind="protocol", message="bad line"),
    ]
    for msg in msgs:
        await writer.send(msg)
    assert out.getvalue() == b"".join((encode(m) + "\n").encode("utf-8") for m in msgs)
    assert out.getvalue().count(b"\n") == len(msgs)


@pytest.mark.asyncio
async def test_writer_send_flushes_after_writing():
    stream = FlushRecorder()
    await Writer(stream).send(Ready(version="0.1.0"))
    assert stream.data == b'{"type":"ready","version":"0.1.0"}\n'
    assert stream.flushed and stream.flushed[-1] == bytes(stream.data)


@pytest.mark.asyncio
async def test_writer_concurrent_sends_keep_lines_whole_and_ordered():
    out = io.BytesIO()
    writer = Writer(out)
    msgs = [TextDelta(chat_id="c1", text=f"part {i} " + "x" * 200) for i in range(100)]
    await asyncio.gather(*(writer.send(m) for m in msgs))
    assert read_lines(out.getvalue()) == [json.loads(encode(m)) for m in msgs]


# Router


def test_router_has_one_handler_per_inbound_type():
    router = Router(Writer(io.BytesIO()))
    for name in [*INBOUND_TYPES, "close"]:
        assert inspect.iscoroutinefunction(getattr(router, name, None)), name


# Loop


@pytest.mark.asyncio
async def test_ready_is_sent_before_any_input():
    stdin = FeedStdin()
    out = io.BytesIO()
    router = FakeRouter()
    task = asyncio.create_task(serve(stdin, Writer(out), router))
    try:
        await wait_until(lambda: out.getvalue().endswith(b"\n"))
        assert read_lines(out.getvalue()) == [READY]

        stdin.feed(NEW_CHAT_LINE)
        await wait_until(lambda: router.calls)
        assert router.calls == [("new_chat", NewChat(chat_id="c1"))]

        stdin.close()
        rc = await asyncio.wait_for(task, TIMEOUT)
    finally:
        stdin.close()
        task.cancel()
    assert rc == 0
    assert read_lines(out.getvalue()) == [READY]


@pytest.mark.asyncio
async def test_stdin_is_read_on_a_daemon_thread():
    stdin = FeedStdin()
    stdin.feed(NEW_CHAT_LINE)
    stdin.close()
    rc = await asyncio.wait_for(serve(stdin, Writer(io.BytesIO()), FakeRouter()), TIMEOUT)
    assert rc == 0
    loop_thread = threading.current_thread()
    assert stdin.reader_threads
    for thread in stdin.reader_threads:
        assert thread is not loop_thread
        assert thread.daemon


@pytest.mark.asyncio
async def test_eof_exits_cleanly():
    rc, lines, router = await run_serve(b"")
    assert rc == 0
    assert lines == [READY]
    assert router.calls == [("close", None)]


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "line",
    [
        pytest.param(b"not json", id="not_json"),
        pytest.param(b"[1, 2]", id="not_object"),
        pytest.param(b'{"chat_id":"c1"}', id="no_type"),
        pytest.param(b'{"type":"bogus"}', id="unknown_type"),
        pytest.param(b'{"type":"new_chat"}', id="missing_field"),
        pytest.param(b'{"type":"prompt","chat_id":"c1","text":"t","context":5}', id="bad_context"),
        pytest.param(b'\xff\xfe{"type":"new_chat","chat_id":"c1"}', id="not_utf8"),
    ],
)
async def test_malformed_line_sends_protocol_error_and_loop_continues(line):
    rc, lines, router = await run_serve(line + b"\n" + NEW_CHAT_LINE)
    assert rc == 0
    assert len(lines) == 2, lines
    assert lines[0] == READY
    assert_protocol_error(lines[1], seq=None)
    assert router.calls == [("new_chat", NewChat(chat_id="c1")), ("close", None)]


@pytest.mark.asyncio
async def test_protocol_error_echoes_seq_when_available():
    data = (
        b'{"type":"bogus","seq":7}\n'
        b'{"type":"new_chat","seq":8}\n'
        b'{"type":"interrupt","seq":"9"}\n'
    )
    rc, lines, router = await run_serve(data)
    assert rc == 0
    assert lines[0] == READY
    assert len(lines) == 4, lines
    assert_protocol_error(lines[1], seq=7)
    assert_protocol_error(lines[2], seq=8)
    assert_protocol_error(lines[3], seq=None)
    assert router.calls == [("close", None)]


@pytest.mark.asyncio
async def test_each_message_reaches_the_handler_for_its_type():
    context = {"file": "papers/a.pdf", "page": 2, "page_count": 10, "selection": "Σ attention"}
    wire = [
        ({"type": "open_project", "path": "/tmp/project", "seq": 1}, OpenProject(path="/tmp/project", seq=1)),
        ({"type": "new_chat", "chat_id": "c1", "seq": 2}, NewChat(chat_id="c1", seq=2)),
        (
            {"type": "resume_chat", "chat_id": "c2", "session_id": "s2", "seq": 3},
            ResumeChat(chat_id="c2", session_id="s2", seq=3),
        ),
        (
            {"type": "prompt", "chat_id": "c1", "text": "café ∑", "context": context, "seq": 4},
            Prompt(chat_id="c1", text="café ∑", context=Context(**context), seq=4),
        ),
        ({"type": "interrupt", "chat_id": "c1", "seq": 5}, Interrupt(chat_id="c1", seq=5)),
        (
            {"type": "permission_reply", "req_id": "r1", "allow": True, "seq": 6},
            PermissionReply(req_id="r1", allow=True, seq=6),
        ),
    ]
    assert sorted(obj["type"] for obj, _ in wire) == sorted(INBOUND_TYPES)
    # Raw UTF-8 on stdin, and one line ended by \r\n, which the protocol accepts.
    lines = [json.dumps(obj, ensure_ascii=False).encode("utf-8") for obj, _ in wire]
    data = b"\n".join(lines[:4]) + b"\n" + lines[4] + b"\r\n" + lines[5] + b"\n"

    rc, out, router = await run_serve(data)
    assert rc == 0
    assert out == [READY]
    assert router.calls == [(obj["type"], msg) for obj, msg in wire] + [("close", None)]


# Console script


def sidecar_command() -> list[str]:
    exe = shutil.which("reader-sidecar", path=sysconfig.get_path("scripts")) or shutil.which(
        "reader-sidecar"
    )
    if exe is None:
        pytest.fail("reader-sidecar is not installed. Run pip install -e '.[dev]' in sidecar/.")
    return [exe]


def test_console_script_answers_bogus_line_and_exits_zero():
    env = {
        key: value
        for key, value in os.environ.items()
        if key != "CLAUDECODE" and not key.startswith(("ANTHROPIC_", "CLAUDE_CODE_"))
    }
    proc = subprocess.run(
        sidecar_command(),
        input=b'{"type":"bogus"}\n',
        capture_output=True,
        env=env,
        timeout=60,
    )
    assert proc.returncode == 0, proc.stderr.decode(errors="replace")
    lines = read_lines(proc.stdout)
    assert len(lines) == 2, proc.stdout
    assert lines[0] == READY
    assert_protocol_error(lines[1], seq=None)
