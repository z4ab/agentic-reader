"""Implementer tests for M4.4 cases the spec tests leave out.

Interrupt on an idle or unknown chat, an interrupted reply that ends without a
result, interrupt while the client connects, and a file outside the project
root.
"""

import asyncio
from pathlib import Path

import pytest

from reader_sidecar.context import build_prefix
from reader_sidecar.protocol import Context
from claude_agent_sdk import ClaudeAgentOptions

from fakes import Turn
from test_chats import Sidecar, delta, short_turn
from test_permissions import STOP, InterruptibleClient, InterruptibleFactory, interruptible


@pytest.fixture
def project(tmp_path: Path) -> Path:
    path = tmp_path / "project"
    path.mkdir()
    return path


@pytest.mark.asyncio
async def test_interrupt_on_an_idle_chat_does_nothing(project):
    async with interruptible(short_turn()) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        sc.send(type="interrupt", chat_id="c1", seq=3)
        await sc.ask("c1")
        assert client.interrupts == 0
        assert sc.count("error", "c1") == 0


@pytest.mark.asyncio
async def test_interrupt_on_an_unknown_chat_is_an_error(project):
    async with Sidecar() as sc:
        sc.open(project)
        sc.send(type="interrupt", chat_id="nope", seq=3)
        await sc.wait_for("error", "nope")
        [error] = sc.replies("nope")
        assert (error["kind"], error["seq"]) == ("unknown_chat", 3)


@pytest.mark.asyncio
async def test_interrupted_reply_without_a_result_sends_interrupted(project):
    async with interruptible([delta("Partial "), STOP], short_turn()) as sc:
        sc.open(project)
        await sc.new_chat("c1")
        sc.prompt("c1", seq=4)
        await sc.wait_for("text_delta", "c1")
        sc.send(type="interrupt", chat_id="c1", seq=5)
        await sc.wait_for("error", "c1")
        [error] = [m for m in sc.replies("c1") if m["type"] == "error"]
        assert (error["kind"], error["seq"]) == ("interrupted", 4)


class SlowConnectClient(InterruptibleClient):
    """connect() waits until the test sets `released`."""

    def __init__(self, options: ClaudeAgentOptions, turns: list[Turn]) -> None:
        super().__init__(options, turns)
        self.connecting = asyncio.Event()
        self.released = asyncio.Event()

    async def connect(self, prompt=None) -> None:
        self.connecting.set()
        await self.released.wait()
        await super().connect(prompt)


class SlowConnectFactory(InterruptibleFactory):
    def __call__(self, options: ClaudeAgentOptions) -> SlowConnectClient:
        client = SlowConnectClient(options, self.turns)
        self.clients.append(client)
        return client


@pytest.mark.asyncio
async def test_interrupt_while_connecting_stops_before_the_query(project):
    sc = Sidecar()
    sc.factory = SlowConnectFactory(short_turn("Again."), [RuntimeError("boom")])
    async with sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        sc.prompt("c1", seq=4)
        await asyncio.wait_for(client.connecting.wait(), 5)

        sc.send(type="interrupt", chat_id="c1", seq=5)
        await sc.new_chat("c2")  # serve handles lines in order, so the interrupt was handled.
        assert sc.count("error", "c1") == 0
        client.released.set()
        await sc.wait_for("error", "c1")

        assert client.interrupts == 0
        assert client.queries == []
        assert [(m["type"], m["kind"], m["seq"]) for m in sc.replies("c1")] == [("error", "interrupted", 4)]

        # The flag does not leak: the next reply runs, and a later failure is not "interrupted".
        await sc.ask("c1", "Next.", seq=6)
        assert client.queries == ["Next."]
        sc.prompt("c1", seq=7)
        await sc.wait_for("error", "c1", 2)
        assert sc.replies("c1")[-1]["kind"] == "internal"


def test_absolute_file_outside_root_is_kept(tmp_path: Path):
    outside = str(tmp_path / "elsewhere" / "paper.pdf")
    prefix = build_prefix(Context(file=outside), root=tmp_path / "project")
    assert prefix == f"<reader_context>\nfile: {outside}\n</reader_context>\n"
