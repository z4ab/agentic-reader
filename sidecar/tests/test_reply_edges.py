"""Implementer tests for two M4.5 review notes carried into M7.3.

1. A rejected RateLimitEvent followed by a plain (non-error) ResultMessage,
   with no reply text, ends the reply with `error` rate_limit, not `result`.
   A reply that did stream text keeps its `result`.
2. A crash exception after an interrupt sends `interrupted` and keeps the
   chat. The next prompt connects a new CLI that resumes the session.
"""

from pathlib import Path

import pytest
from claude_agent_sdk import CLIConnectionError, ProcessError

from test_chats import SESSION, Sidecar, delta, result, short_turn
from test_errors import errors, rate_limit_event
from test_permissions import STOP, interruptible


@pytest.fixture
def project(tmp_path: Path) -> Path:
    path = tmp_path / "project"
    path.mkdir()
    return path


@pytest.mark.asyncio
async def test_rejected_rate_limit_then_plain_result_sends_rate_limit(project):
    async with Sidecar([rate_limit_event("rejected"), result()], short_turn("Later.")) as sc:
        sc.open(project)
        await sc.new_chat("c1")
        sc.prompt("c1", seq=4)
        await sc.wait_for("error", "c1")
        assert [(m["type"], m["kind"], m["seq"]) for m in sc.replies("c1")] == [("error", "rate_limit", 4)]

        # The chat stays open.
        await sc.ask("c1", seq=5)
        assert sc.count("error", "c1") == 1


@pytest.mark.asyncio
async def test_rejected_rate_limit_with_a_reply_keeps_the_result(project):
    async with Sidecar([rate_limit_event("rejected"), delta("Answer."), result()]) as sc:
        sc.open(project)
        await sc.new_chat("c1")
        await sc.ask("c1", seq=4)
        assert errors(sc, "c1") == []
        assert [m["type"] for m in sc.replies("c1")] == ["text_delta", "result"]


@pytest.mark.parametrize(
    "make_error",
    [
        lambda: ProcessError("Command failed with exit code 143", exit_code=143),
        lambda: CLIConnectionError("CLI process ended"),
    ],
    ids=["process_error", "connection_error"],
)
@pytest.mark.asyncio
async def test_crash_after_interrupt_reconnects_on_the_next_prompt(project, make_error):
    async with interruptible([delta("Partial "), STOP, make_error()], short_turn("Again.")) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        assert client.options.resume is None
        sc.prompt("c1", seq=4)
        await sc.wait_for("text_delta", "c1")
        sc.send(type="interrupt", chat_id="c1", seq=5)
        await sc.wait_for("error", "c1")

        [error] = errors(sc, "c1")
        assert (error["kind"], error["seq"]) == ("interrupted", 4)
        assert client.disconnects == 1
        assert not client.connected

        # The next prompt reaches a new connection that resumes the session.
        await sc.ask("c1", seq=6)
        assert client.connected
        assert client.options.resume == SESSION
        assert sc.count("error", "c1") == 1
        assert sc.count("unknown_chat", "c1") == 0


@pytest.mark.asyncio
async def test_other_exception_after_interrupt_keeps_the_connection(project):
    async with interruptible([delta("Partial "), STOP, RuntimeError("boom")], short_turn("Again.")) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        sc.prompt("c1", seq=4)
        await sc.wait_for("text_delta", "c1")
        sc.send(type="interrupt", chat_id="c1", seq=5)
        await sc.wait_for("error", "c1")
        assert client.disconnects == 0
        await sc.ask("c1", seq=6)
        assert sc.count("error", "c1") == 1
