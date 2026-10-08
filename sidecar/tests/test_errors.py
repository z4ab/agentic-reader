"""Spec tests for error mapping and auth failures (task M4.5).

The spec fixes the error kinds and which failure maps to each. It does not fix
the module interface or the auth signal. These tests fix them as follows. The
implementation must follow these conventions.

1. `reader_sidecar.errors.error_kind(exc: Exception) -> str` maps one
   exception to its protocol kind: cli_missing, process, result, auth,
   rate_limit, or internal. CLINotFoundError subclasses CLIConnectionError and
   ResultError subclasses ProcessError, so the subclass checks come first.

2. Auth signal (assumption). The spec leaves the exact signal to the
   implementer. These tests only require that a failure which is clearly an
   auth failure maps to `auth`. Every auth fixture carries all of these
   signals at once, so any one of them is enough to detect it: the text
   "Invalid API key", the text "authentication_error", the HTTP status 401
   (as text, and as `api_error_status` where the SDK type has that field),
   and, on the message path, AssistantMessage.error == "authentication_failed".
   Auth and rate limits win over `result` and `process`, because the live CLI
   reports them as an API error inside an error result.

3. Auth and rate limits arrive two ways and both are covered:
   a. As an exception from connect, query, or receive_response
      (ResultError or ProcessError carrying the text).
   b. Inside a turn, with no exception: an AssistantMessage with `error` set
      and a ResultMessage with `is_error=True`, or a RateLimitEvent whose
      status is "rejected". The chat then sends at least one `error` of that
      kind and none of any other kind. Whether a `result` also follows is not
      tested. A RateLimitEvent with status "allowed_warning" is not a failure
      and sends no error.

4. Every error from a reply carries the chat_id and the prompt's seq. An
   exception from the SDK sends exactly one error. Its `message` is a
   non-empty string. For `result` the message contains the ResultError's
   `terminal_reason` verbatim.

5. No error message contains the API key or the value of any environment
   variable, even when the SDK exception text, stderr, or result payload
   holds them. The tests check the whole stdout of the sidecar for the
   values of ANTHROPIC_API_KEY and one other variable.

6. A chat whose reply raised CLINotFoundError, CLIConnectionError,
   ProcessError, or ResultError (the CLI process is gone) is removed from the
   registry before its error is sent. A later prompt to that chat_id gets
   `unknown_chat`, and resume_chat with that chat_id succeeds and makes a new
   client. Other failures (`internal`) keep the chat, as
   test_chat_edges.py already expects.

7. Carried notes. PROTOCOL.md names every error kind in its `error` section.
   With stdout closed and stdin open, the console script exits without a
   SIGABRT and without "Fatal Python error" on stderr. Its exit code is not
   tested.

Not covered here: the live check with ANTHROPIC_API_KEY=bad, the live_smoke.py
pass-through option, and nested tool input truncation.
"""

import contextlib
import json
import os
import shutil
import subprocess
import sys
import sysconfig
from pathlib import Path
from typing import Any

import pytest
from claude_agent_sdk import (
    AssistantMessage,
    CLIConnectionError,
    CLIJSONDecodeError,
    CLINotFoundError,
    ProcessError,
    RateLimitEvent,
    RateLimitInfo,
    ResultError,
    ResultMessage,
    TextBlock,
)

from fakes import FakeClient
from test_chats import SESSION, Sidecar, delta, short_turn

TIMEOUT = 5.0
DOC = Path(__file__).resolve().parent.parent / "PROTOCOL.md"

FAKE_KEY = "sk-ant-api03-Zq7XfakeKEYm4p9Lw2Rt8Vb6Nc1Hd5Js3Gk0Yx-testONLYxQ"
OTHER_VAR = "READER_TEST_SECRET"
OTHER_VALUE = "env-value-7f3a9c1e5b"

AUTH_TEXT = (
    "Invalid API key · Please run /login "
    '(API Error: 401 {"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})'
)
RATE_TEXT = (
    'API Error: 429 {"type":"error","error":{"type":"rate_limit_error",'
    '"message":"Number of request tokens has exceeded your per-minute rate limit"}}'
)


# SDK failures


def result_error(
    *,
    subtype: str = "success",
    terminal_reason: str | None = "api_error",
    status: int | None = None,
    text: str | None = None,
    errors: list[str] | None = None,
) -> ResultError:
    """A ResultError as ClaudeSDKClient raises it after an error result."""
    data: dict[str, Any] = {
        "type": "result",
        "subtype": subtype,
        "is_error": True,
        "session_id": SESSION,
        "terminal_reason": terminal_reason,
        "api_error_status": status,
        "result": text,
        "errors": errors or [],
    }
    detail = "; ".join(errors) if errors else (text or subtype)
    return ResultError(f"Claude Code returned an error result: {detail}", data=data, exit_code=1)


def max_turns_error() -> ResultError:
    return result_error(
        subtype="error_during_execution",
        terminal_reason="max_turns",
        errors=["Reached maximum number of turns (8)"],
    )


def server_error() -> ResultError:
    return result_error(terminal_reason="api_error", status=500, text="API Error: 500 Internal server error")


def auth_result_error() -> ResultError:
    return result_error(terminal_reason="api_error", status=401, text=AUTH_TEXT)


def auth_process_error() -> ProcessError:
    return ProcessError("Command failed with exit code 1", exit_code=1, stderr=AUTH_TEXT)


def rate_result_error() -> ResultError:
    return result_error(terminal_reason="api_error", status=429, text=RATE_TEXT)


def rate_process_error() -> ProcessError:
    return ProcessError("Command failed with exit code 1", exit_code=1, stderr=RATE_TEXT)


# Turns that report a failure without raising


def error_result(status: int, text: str) -> ResultMessage:
    return ResultMessage(
        subtype="success",
        duration_ms=300,
        duration_api_ms=0,
        is_error=True,
        num_turns=1,
        session_id=SESSION,
        total_cost_usd=0.0,
        usage={},
        result=text,
        api_error_status=status,
        terminal_reason="api_error",
    )


def auth_turn(text: str = AUTH_TEXT) -> list[Any]:
    return [
        AssistantMessage(
            content=[TextBlock(text=text)],
            model="<synthetic>",
            error="authentication_failed",
            session_id=SESSION,
        ),
        error_result(401, text),
    ]


def rate_limit_event(status: str) -> RateLimitEvent:
    info = RateLimitInfo(status=status, resets_at=1_790_000_000, rate_limit_type="five_hour", utilization=1.0)
    return RateLimitEvent(rate_limit_info=info, uuid="rl-1", session_id=SESSION)


def rejected_turn() -> list[Any]:
    return [rate_limit_event("rejected"), error_result(429, RATE_TEXT)]


# Harness


@pytest.fixture
def project(tmp_path: Path) -> Path:
    path = tmp_path / "project"
    path.mkdir()
    return path


def errors(sc: Sidecar, chat_id: str) -> list[dict[str, Any]]:
    return [m for m in sc.replies(chat_id) if m["type"] == "error"]


def fail(client: FakeClient, where: str, exc: Exception) -> None:
    """Make `client` raise `exc` at `where`: connect, query, or receive."""
    if where == "connect":

        async def connect(prompt: Any = None) -> None:
            raise exc

        client.connect = connect
    elif where == "query":

        async def query(prompt: Any, session_id: str = "default") -> None:
            raise exc

        client.query = query
    else:

        async def receive_response():
            yield delta("Partial ")
            raise exc

        client.receive_response = receive_response


async def failed_reply(sc: Sidecar, project: Path, where: str, exc: Exception, seq: int = 4) -> dict[str, Any]:
    """Open a chat c1, make its reply raise `exc` at `where`, and return its one error."""
    sc.open(project)
    client = await sc.new_chat("c1")
    fail(client, where, exc)
    sc.prompt("c1", seq=seq)
    await sc.wait_for("error", "c1")
    [error] = errors(sc, "c1")
    assert error["chat_id"] == "c1"
    assert error["seq"] == seq
    assert isinstance(error["message"], str) and error["message"]
    return error


# 1. error_kind


@pytest.mark.parametrize(
    ("exc", "kind"),
    [
        pytest.param(CLINotFoundError(), "cli_missing", id="cli_not_found"),
        pytest.param(
            CLINotFoundError("Claude Code not found", cli_path="/opt/claude"), "cli_missing", id="cli_not_found_path"
        ),
        pytest.param(CLIConnectionError("Not connected. Call connect() first."), "process", id="cli_connection"),
        pytest.param(
            ProcessError("Command failed with exit code 1", exit_code=1, stderr="segfault"), "process", id="process"
        ),
        pytest.param(max_turns_error(), "result", id="result_max_turns"),
        pytest.param(server_error(), "result", id="result_server_error"),
        pytest.param(auth_result_error(), "auth", id="auth_result_error"),
        pytest.param(auth_process_error(), "auth", id="auth_process_error"),
        pytest.param(rate_result_error(), "rate_limit", id="rate_result_error"),
        pytest.param(rate_process_error(), "rate_limit", id="rate_process_error"),
        pytest.param(RuntimeError("boom"), "internal", id="runtime_error"),
        pytest.param(CLIJSONDecodeError("{not json", ValueError("bad")), "internal", id="json_decode"),
    ],
)
def test_error_kind(exc, kind):
    from reader_sidecar.errors import error_kind

    assert error_kind(exc) == kind


# 2. Each exception through the fake client


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("where", "make", "kind"),
    [
        pytest.param("connect", CLINotFoundError, "cli_missing", id="cli_missing"),
        pytest.param(
            "query", lambda: CLIConnectionError("Not connected. Call connect() first."), "process", id="cli_connection"
        ),
        pytest.param(
            "receive",
            lambda: ProcessError("Command failed with exit code 1", exit_code=1, stderr="segfault"),
            "process",
            id="process",
        ),
        pytest.param("receive", max_turns_error, "result", id="result"),
        pytest.param("connect", auth_result_error, "auth", id="auth_at_connect"),
        pytest.param("receive", auth_process_error, "auth", id="auth_process"),
        pytest.param("receive", rate_result_error, "rate_limit", id="rate_limit_result"),
        pytest.param("query", rate_process_error, "rate_limit", id="rate_limit_process"),
        pytest.param("receive", lambda: RuntimeError("boom"), "internal", id="internal"),
    ],
)
async def test_reply_exception_maps_to_kind(project, where, make, kind):
    async with Sidecar() as sc:
        error = await failed_reply(sc, project, where, make())
        assert error["kind"] == kind


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "make", [max_turns_error, server_error], ids=["max_turns", "api_error"]
)
async def test_result_error_message_carries_terminal_reason(project, make):
    exc = make()
    async with Sidecar() as sc:
        error = await failed_reply(sc, project, "receive", exc)
        assert error["kind"] == "result"
        assert exc.terminal_reason in error["message"]


# 3. Failures reported inside a turn


@pytest.mark.asyncio
async def test_auth_error_result_sends_auth_error(project):
    async with Sidecar(auth_turn()) as sc:
        sc.open(project)
        await sc.new_chat("c1")
        sc.prompt("c1", seq=7)
        await sc.wait_for("error", "c1")
        found = errors(sc, "c1")
        assert {e["kind"] for e in found} == {"auth"}, found
        assert all(e["seq"] == 7 for e in found), found


@pytest.mark.asyncio
async def test_rejected_rate_limit_event_sends_rate_limit_error(project):
    async with Sidecar(rejected_turn()) as sc:
        sc.open(project)
        await sc.new_chat("c1")
        sc.prompt("c1", seq=8)
        await sc.wait_for("error", "c1")
        found = errors(sc, "c1")
        assert {e["kind"] for e in found} == {"rate_limit"}, found
        assert all(e["seq"] == 8 for e in found), found


@pytest.mark.asyncio
async def test_rate_limit_warning_is_not_an_error(project):
    async with Sidecar([rate_limit_event("allowed_warning"), *short_turn("Fine.")]) as sc:
        sc.open(project)
        await sc.new_chat("c1")
        await sc.ask("c1", seq=9)
        assert errors(sc, "c1") == []
        assert [m["text"] for m in sc.replies("c1") if m["type"] == "text_delta"] == ["Fine."]


# 4. No secrets in error messages


def leaky_failures() -> list[Any]:
    env_dump = f"ANTHROPIC_API_KEY={FAKE_KEY} {OTHER_VAR}={OTHER_VALUE} PATH=/usr/bin"
    return [
        pytest.param("connect", lambda: CLINotFoundError(f"Claude Code not found ({env_dump})"), id="cli_missing"),
        pytest.param("query", lambda: CLIConnectionError(f"Failed to start Claude Code: {env_dump}"), id="cli_connection"),
        pytest.param(
            "receive",
            lambda: ProcessError(f"Command failed: {env_dump}", exit_code=1, stderr=f"fatal: {env_dump}"),
            id="process",
        ),
        pytest.param(
            "receive",
            lambda: result_error(
                subtype="error_during_execution",
                terminal_reason="max_turns",
                text=f"result {env_dump}",
                errors=[f"error {env_dump}"],
            ),
            id="result",
        ),
        pytest.param(
            "receive",
            lambda: result_error(status=401, text=f"{AUTH_TEXT} key={FAKE_KEY} {env_dump}"),
            id="auth",
        ),
        pytest.param(
            "receive",
            lambda: ProcessError("Command failed", exit_code=1, stderr=f"{RATE_TEXT} {env_dump}"),
            id="rate_limit",
        ),
        pytest.param("receive", lambda: RuntimeError(f"unexpected {env_dump}"), id="internal"),
    ]


@pytest.fixture
def secrets(monkeypatch):
    monkeypatch.setenv("ANTHROPIC_API_KEY", FAKE_KEY)
    monkeypatch.setenv(OTHER_VAR, OTHER_VALUE)


def assert_no_secrets(sc: Sidecar) -> None:
    out = sc.out.getvalue().decode("utf-8")
    for value in (FAKE_KEY, FAKE_KEY[14:40], OTHER_VALUE):
        assert value not in out, out


@pytest.mark.asyncio
@pytest.mark.parametrize(("where", "make"), leaky_failures())
async def test_error_message_never_holds_key_or_env(project, secrets, where, make):
    async with Sidecar() as sc:
        await failed_reply(sc, project, where, make())
        assert_no_secrets(sc)


@pytest.mark.asyncio
async def test_auth_turn_error_never_holds_key_or_env(project, secrets):
    text = f"{AUTH_TEXT} key={FAKE_KEY} {OTHER_VAR}={OTHER_VALUE}"
    async with Sidecar(auth_turn(text)) as sc:
        sc.open(project)
        await sc.new_chat("c1")
        sc.prompt("c1", seq=7)
        await sc.wait_for("error", "c1")
        assert_no_secrets(sc)


# 5. A crashed chat leaves the registry


CRASHES = [
    pytest.param("connect", CLINotFoundError, id="cli_missing"),
    pytest.param("query", lambda: CLIConnectionError("Not connected. Call connect() first."), id="cli_connection"),
    pytest.param("receive", lambda: ProcessError("Command failed", exit_code=1, stderr="segfault"), id="process"),
    pytest.param("receive", max_turns_error, id="result"),
    pytest.param("receive", auth_result_error, id="auth"),
]


@pytest.mark.asyncio
@pytest.mark.parametrize(("where", "make"), CRASHES)
async def test_crashed_chat_is_forgotten(project, where, make):
    async with Sidecar() as sc:
        await failed_reply(sc, project, where, make())
        sc.prompt("c1", seq=5)
        await sc.wait_for("error", "c1", 2)
        later = errors(sc, "c1")[1]
        assert (later["kind"], later["seq"]) == ("unknown_chat", 5)


@pytest.mark.asyncio
@pytest.mark.parametrize(("where", "make"), CRASHES)
async def test_resume_chat_recreates_a_crashed_chat(project, where, make):
    async with Sidecar(short_turn("Back again.")) as sc:
        await failed_reply(sc, project, where, make())
        crashed = sc.clients[0]
        client = await sc.resume_chat("c1", SESSION, seq=6)
        assert client is not crashed
        assert client.options.resume == SESSION
        await sc.ask("c1", seq=7)
        assert [m["text"] for m in sc.replies("c1") if m["type"] == "text_delta"][-1] == "Back again."


# 6. Carried notes


def test_protocol_doc_lists_every_error_kind():
    text = DOC.read_text(encoding="utf-8")
    start = text.index("### `error`")
    end = text.find("\n## ", start)
    section = text[start : end if end != -1 else None]
    kinds = [
        "protocol",
        "busy",
        "no_project",
        "chat_exists",
        "unknown_chat",
        "interrupted",
        "internal",
        "cli_missing",
        "process",
        "result",
        "auth",
        "rate_limit",
    ]
    missing = [kind for kind in kinds if f"`{kind}`" not in section]
    assert missing == []


def sidecar_process() -> subprocess.Popen:
    exe = shutil.which("reader-sidecar", path=sysconfig.get_path("scripts"))
    assert exe, "reader-sidecar is not installed. Run pip install -e '.[dev]' in sidecar/."
    env = {
        key: value
        for key, value in os.environ.items()
        if key != "CLAUDECODE" and not key.startswith(("ANTHROPIC_", "CLAUDE_CODE_", "READER_"))
    }
    return subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX pipes and signals")
def test_closed_stdout_with_open_stdin_exits_without_abort():
    proc = sidecar_process()
    try:
        assert json.loads(proc.stdout.readline())["type"] == "ready"
        proc.stdout.close()
        # The protocol error for this line cannot be written, so serve raises
        # while the reader thread still blocks in readline on the open stdin.
        proc.stdin.write(b'{"type":"bogus"}\n')
        proc.stdin.flush()
        rc = proc.wait(timeout=TIMEOUT)
        err = proc.stderr.read()
    finally:
        proc.kill()
        with contextlib.suppress(BrokenPipeError):
            proc.stdin.close()
        proc.stderr.close()
    assert rc >= 0, err.decode(errors="replace")
    assert b"Fatal Python error" not in err
