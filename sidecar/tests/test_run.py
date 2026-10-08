"""Implementer tests for M4.2 items that test_io.py does not cover.

Decode failures that are not ProtocolError, a failing handler, cancellation,
the READER_SIDECAR_LOG level, SIGTERM, and the stream setup (UTF-8, Windows
binary mode).
"""

import asyncio
import io
import json
import os
import shutil
import signal
import subprocess
import sys
import sysconfig
import types

import pytest

from reader_sidecar import __version__
from reader_sidecar import io as sidecar_io
from reader_sidecar.io import Writer, setup_streams
from reader_sidecar.main import serve

TIMEOUT = 5.0
READY = {"type": "ready", "version": __version__}
NEW_CHAT_LINE = b'{"type":"new_chat","chat_id":"c1","seq":3}\n'


class Router:
    """Records handler calls. new_chat raises when `fail` is set."""

    def __init__(self, fail: bool = False) -> None:
        self.fail = fail
        self.calls: list[str] = []

    async def new_chat(self, msg: object) -> None:
        self.calls.append("new_chat")
        if self.fail:
            raise RuntimeError("boom")

    async def close(self) -> None:
        self.calls.append("close")


async def run_serve(data: bytes, router: Router) -> tuple[int, list[dict]]:
    out = io.BytesIO()
    rc = await asyncio.wait_for(serve(io.BytesIO(data), Writer(out), router), TIMEOUT)
    return rc, [json.loads(line) for line in out.getvalue().splitlines()]


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "line",
    [
        pytest.param(b'{"type":"new_chat","chat_id":"c1","n":' + b"1" * 5000 + b"}", id="huge_int"),
        pytest.param(b"[" * 200_000 + b"]" * 200_000, id="deep_nesting"),
    ],
)
async def test_decode_value_and_recursion_errors_are_protocol_errors(line):
    router = Router()
    rc, lines = await run_serve(line + b"\n" + NEW_CHAT_LINE, router)
    assert rc == 0
    assert lines[0] == READY
    assert len(lines) == 2, lines
    assert lines[1]["kind"] == "protocol"
    assert lines[1]["seq"] is None
    assert router.calls == ["new_chat", "close"]


@pytest.mark.asyncio
async def test_failing_handler_sends_internal_error_and_loop_continues():
    router = Router(fail=True)
    rc, lines = await run_serve(NEW_CHAT_LINE + NEW_CHAT_LINE, router)
    assert rc == 0
    error = {"type": "error", "chat_id": "c1", "kind": "internal", "message": "new_chat failed", "seq": 3}
    assert lines == [READY, error, error]
    assert router.calls == ["new_chat", "new_chat", "close"]


@pytest.mark.asyncio
async def test_cancelled_serve_closes_the_router():
    router = Router()
    read_fd, write_fd = os.pipe()
    with open(read_fd, "rb") as stdin:
        out = io.BytesIO()
        task = asyncio.create_task(serve(stdin, Writer(out), router))
        while not out.getvalue():
            await asyncio.sleep(0.01)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await asyncio.wait_for(task, TIMEOUT)
        os.close(write_fd)  # Lets the reader thread see EOF and exit.
    assert router.calls == ["close"]


# Console script


def sidecar(env_updates: dict[str, str | None]) -> subprocess.Popen:
    exe = shutil.which("reader-sidecar", path=sysconfig.get_path("scripts"))
    assert exe, "reader-sidecar is not installed. Run pip install -e '.[dev]' in sidecar/."
    env = {
        key: value
        for key, value in os.environ.items()
        if key != "CLAUDECODE" and not key.startswith(("ANTHROPIC_", "CLAUDE_CODE_", "READER_"))
    }
    env.update({key: value for key, value in env_updates.items() if value is not None})
    return subprocess.Popen(
        [exe],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )


@pytest.mark.parametrize(
    ("value", "present", "absent"),
    [
        pytest.param(None, ["INFO"], ["DEBUG"], id="default_info"),
        pytest.param("debug", ["DEBUG", "INFO"], [], id="debug"),
        pytest.param("WARNING", [], ["INFO", "DEBUG"], id="warning"),
        pytest.param("loud", ["WARNING", "INFO"], ["DEBUG"], id="invalid_falls_back_to_info"),
    ],
)
def test_log_level_comes_from_env(value, present, absent):
    proc = sidecar({"READER_SIDECAR_LOG": value})
    out, err = proc.communicate(NEW_CHAT_LINE, timeout=60)
    assert proc.returncode == 0, err
    assert [json.loads(line) for line in out.splitlines()] == [READY]
    levels = {line.split()[2] for line in err.decode().splitlines()}
    for level in present:
        assert level in levels, err
    for level in absent:
        assert level not in levels, err


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX signal")
def test_sigterm_exits_zero_while_stdin_is_open():
    proc = sidecar({"READER_SIDECAR_LOG": "INFO"})
    try:
        assert json.loads(proc.stdout.readline()) == READY
        proc.send_signal(signal.SIGTERM)
        rc = proc.wait(timeout=TIMEOUT)
        out, err = proc.stdout.read(), proc.stderr.read()
    finally:
        proc.kill()
        proc.stdin.close()
        proc.stdout.close()
        proc.stderr.close()
    assert rc == 0, err
    assert out == b""
    assert b"SIGTERM received" in err
    assert b"Fatal Python error" not in err


# Stream setup


class FakeStdStream:
    def __init__(self, fd: int) -> None:
        self.fd = fd
        self.config: dict = {}

    def reconfigure(self, **kwargs) -> None:
        self.config.update(kwargs)

    def fileno(self) -> int:
        return self.fd


@pytest.mark.parametrize("platform", ["linux", "darwin", "win32"])
def test_setup_streams_forces_utf8_and_binary_mode_on_windows(monkeypatch, platform):
    setmode_calls = []
    msvcrt = types.SimpleNamespace(setmode=lambda fd, mode: setmode_calls.append((fd, mode)))
    monkeypatch.setitem(sys.modules, "msvcrt", msvcrt)
    monkeypatch.setattr(sidecar_io.sys, "platform", platform)
    monkeypatch.setattr(sidecar_io.os, "O_BINARY", 0x8000, raising=False)

    stdin, stdout = FakeStdStream(0), FakeStdStream(1)
    setup_streams(stdin, stdout)

    assert stdin.config["encoding"] == "utf-8"
    assert stdout.config == {"encoding": "utf-8", "newline": "\n"}
    expected = [(0, 0x8000), (1, 0x8000)] if platform == "win32" else []
    assert setmode_calls == expected
