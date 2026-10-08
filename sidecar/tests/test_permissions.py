"""Spec tests for permissions and interrupt (task M4.4).

The task spec fixes the can_use_tool flow, the 5-minute timeout, the
permission_request and permission_reply messages, denial on chat close, and
what an interrupt does. These tests fix the parts it leaves open:

1. Each chat's `can_use_tool` is the callable in the ClaudeAgentOptions the
   factory receives (`client.options.can_use_tool`). It already knows its
   chat, so it is awaited as `can_use_tool(tool_name, tool_input, ctx)` with
   a `claude_agent_sdk.ToolPermissionContext`. The tests call it directly
   while the sidecar serves, the way the SDK calls it from its own task. It
   must not need a running reply.

2. The timeout is `reader_sidecar.permissions.PERMISSION_TIMEOUT`, in seconds,
   300 by default. can_use_tool reads it on each call, so tests shorten it
   with monkeypatch.

3. Requests and replies go through the protocol: the tests read
   `permission_request {req_id, chat_id, tool, input}` from stdout and answer
   with a `permission_reply` line on stdin. `req_id` is a UUID string. The
   input is small, so it is sent unchanged.

4. A reply allows only when `allow` is the JSON value `true`. `false` and
   every other value ("no", "yes", "true", 1) deny. An explicit deny returns
   `PermissionResultDeny(message="User denied")`. A timeout or a closing chat
   returns some PermissionResultDeny. Allow returns PermissionResultAllow with
   the input unchanged (`updated_input` None or equal to the input).

5. A reply with an unknown or stale req_id changes nothing and the sidecar
   keeps serving. Whether it also sends an error is not checked.

6. Closing a chat (open_project with another path, or EOF) denies its
   pending requests right away, without waiting for the timeout.

7. Interrupt uses `InterruptibleClient` below, a FakeClient with
   `interrupt()`. A STOP item in a scripted turn waits until interrupt() is
   called. An exception instance in a turn is raised from
   receive_response(), the way the SDK may fail after an interrupt.
   a. `interrupt {chat_id}` calls `client.interrupt()` once while the reply
      runs. The reply then ends with its normal `result` and no error.
   b. If receive_response() raises after the interrupt, the chat sends
      `error {chat_id, kind: "interrupted", message}` and no result.
   In both cases the chat takes prompts again afterwards.

Not covered (spec silent): interrupt on an idle or unknown chat, and pending
permission requests at interrupt time.
"""

import asyncio
import uuid
from collections.abc import AsyncIterator
from pathlib import Path
from typing import Any

import pytest
from claude_agent_sdk import (
    ClaudeAgentOptions,
    CLIConnectionError,
    PermissionResultAllow,
    PermissionResultDeny,
    ProcessError,
    ToolPermissionContext,
)

from fakes import FakeClient, FakeFactory, Turn
from reader_sidecar import permissions
from test_chats import TIMEOUT, Sidecar, delta, result, result_line, short_turn

pytestmark = pytest.mark.asyncio

BASH_INPUT = {"command": "ls papers"}
WRITE_INPUT = {"file_path": "notes.md", "content": "Summary of page 4."}
STOP = object()


@pytest.fixture
def project(tmp_path: Path) -> Path:
    path = tmp_path / "project"
    path.mkdir()
    return path


def ask_permission(client: FakeClient, tool: str = "Bash", tool_input: dict[str, Any] = BASH_INPUT) -> asyncio.Task:
    """Call the chat's can_use_tool in its own task, as the SDK does."""
    return asyncio.create_task(client.options.can_use_tool(tool, dict(tool_input), ToolPermissionContext()))


def requests(sc: Sidecar, chat_id: str) -> list[dict[str, Any]]:
    return [m for m in sc.replies(chat_id) if m["type"] == "permission_request"]


def errors(sc: Sidecar) -> list[dict[str, Any]]:
    return [m for m in sc.sent() if m["type"] == "error"]


def reply(sc: Sidecar, req_id: str, allow: Any, seq: int | None = None) -> None:
    sc.send(type="permission_reply", req_id=req_id, allow=allow, seq=seq)


async def next_request(sc: Sidecar, chat_id: str) -> dict[str, Any]:
    """Wait for one more permission_request on `chat_id` and return it."""
    count = len(requests(sc, chat_id))
    await sc.wait_for("permission_request", chat_id, count + 1)
    return requests(sc, chat_id)[count]


# Permission requests


async def test_default_timeout_is_five_minutes():
    assert permissions.PERMISSION_TIMEOUT == 300


async def test_request_is_sent_and_waits_for_a_reply(project):
    async with Sidecar() as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        task = ask_permission(client, "Write", WRITE_INPUT)

        req = await next_request(sc, "c1")
        assert req == {
            "type": "permission_request",
            "req_id": req["req_id"],
            "chat_id": "c1",
            "tool": "Write",
            "input": WRITE_INPUT,
        }
        assert isinstance(req["req_id"], str)
        uuid.UUID(req["req_id"])

        await asyncio.sleep(0.05)
        assert not task.done()

        reply(sc, req["req_id"], True)
        await asyncio.wait_for(task, TIMEOUT)


async def test_allow_reply_allows(project):
    async with Sidecar() as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        task = ask_permission(client)
        req = await next_request(sc, "c1")

        reply(sc, req["req_id"], True, seq=6)
        decision = await asyncio.wait_for(task, TIMEOUT)

        assert isinstance(decision, PermissionResultAllow)
        assert decision.updated_input in (None, BASH_INPUT)
        assert errors(sc) == []


async def test_deny_reply_denies(project):
    async with Sidecar() as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        task = ask_permission(client)
        req = await next_request(sc, "c1")

        reply(sc, req["req_id"], False, seq=6)
        decision = await asyncio.wait_for(task, TIMEOUT)

        assert isinstance(decision, PermissionResultDeny)
        assert decision.message == "User denied"
        assert errors(sc) == []


@pytest.mark.parametrize("allow", ["no", "yes", "true", 1], ids=["no", "yes", "true_string", "one"])
async def test_allow_other_than_true_denies(project, allow):
    async with Sidecar() as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        task = ask_permission(client)
        req = await next_request(sc, "c1")

        reply(sc, req["req_id"], allow)
        decision = await asyncio.wait_for(task, TIMEOUT)
        assert isinstance(decision, PermissionResultDeny)


async def test_no_reply_within_the_timeout_denies(project, monkeypatch):
    monkeypatch.setattr(permissions, "PERMISSION_TIMEOUT", 0.05)
    async with Sidecar(short_turn()) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        task = ask_permission(client)
        req = await next_request(sc, "c1")

        decision = await asyncio.wait_for(task, TIMEOUT)
        assert isinstance(decision, PermissionResultDeny)

        # A late reply changes nothing and the sidecar keeps serving.
        reply(sc, req["req_id"], True, seq=7)
        await sc.ask("c1")
        assert len(requests(sc, "c1")) == 1


async def test_each_reply_resolves_its_own_request(project):
    async with Sidecar() as sc:
        sc.open(project)
        c1 = await sc.new_chat("c1")
        c2 = await sc.new_chat("c2")
        first = ask_permission(c1, "Bash", BASH_INPUT)
        req1 = await next_request(sc, "c1")
        second = ask_permission(c2, "Write", WRITE_INPUT)
        req2 = await next_request(sc, "c2")

        assert req1["req_id"] != req2["req_id"]
        assert (req1["tool"], req1["input"]) == ("Bash", BASH_INPUT)
        assert (req2["tool"], req2["input"]) == ("Write", WRITE_INPUT)

        reply(sc, req2["req_id"], False)
        assert isinstance(await asyncio.wait_for(second, TIMEOUT), PermissionResultDeny)
        assert not first.done()

        reply(sc, req1["req_id"], True)
        assert isinstance(await asyncio.wait_for(first, TIMEOUT), PermissionResultAllow)


async def test_unknown_req_id_changes_nothing(project):
    async with Sidecar() as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        task = ask_permission(client)
        req = await next_request(sc, "c1")

        reply(sc, str(uuid.uuid4()), True, seq=8)
        await sc.new_chat("c2")  # serve handles lines in order, so the reply was handled.
        assert not task.done()

        reply(sc, req["req_id"], False)
        assert isinstance(await asyncio.wait_for(task, TIMEOUT), PermissionResultDeny)


# Closing chats


async def test_project_switch_denies_pending_requests(tmp_path):
    first, second = tmp_path / "first", tmp_path / "second"
    first.mkdir()
    second.mkdir()
    async with Sidecar() as sc:
        sc.open(first)
        client = await sc.new_chat("c1")
        task = ask_permission(client)
        await next_request(sc, "c1")

        sc.open(second)
        await sc.new_chat("c2")
        decision = await asyncio.wait_for(task, TIMEOUT)
        assert isinstance(decision, PermissionResultDeny)


async def test_eof_denies_pending_requests(project):
    async with Sidecar() as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        task = ask_permission(client)
        await next_request(sc, "c1")
    decision = await asyncio.wait_for(task, TIMEOUT)
    assert isinstance(decision, PermissionResultDeny)


# Interrupt


class InterruptibleClient(FakeClient):
    """FakeClient with interrupt(). See point 7 of the module docstring."""

    def __init__(self, options: ClaudeAgentOptions, turns: list[Turn]) -> None:
        super().__init__(options, turns)
        self.interrupts = 0
        self.stopped = asyncio.Event()

    async def interrupt(self) -> None:
        if not self.connected:
            raise CLIConnectionError("Not connected. Call connect() first.")
        self.interrupts += 1
        self.stopped.set()

    async def receive_response(self) -> AsyncIterator[Any]:
        async for item in super().receive_response():
            if item is STOP:
                await self.stopped.wait()
                self.stopped.clear()
            elif isinstance(item, BaseException):
                raise item
            else:
                yield item


class InterruptibleFactory(FakeFactory):
    def __call__(self, options: ClaudeAgentOptions) -> InterruptibleClient:
        client = InterruptibleClient(options, self.turns)
        self.clients.append(client)
        return client


def interruptible(*turns: Turn) -> Sidecar:
    sc = Sidecar()
    sc.factory = InterruptibleFactory(*turns)
    return sc


async def test_interrupt_ends_the_reply_with_its_result(project):
    async with interruptible([delta("Partial "), STOP, result()], short_turn("Again.")) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        sc.prompt("c1", seq=4)
        await sc.wait_for("text_delta", "c1")
        assert client.interrupts == 0

        sc.send(type="interrupt", chat_id="c1", seq=5)
        await sc.wait_for("result", "c1")

        assert client.interrupts == 1
        assert sc.replies("c1") == [
            {"type": "text_delta", "chat_id": "c1", "text": "Partial "},
            result_line("c1"),
        ]

        await sc.ask("c1", seq=6)
        assert sc.count("result", "c1") == 2
        assert errors(sc) == []


@pytest.mark.parametrize(
    "make_error",
    [
        lambda: ProcessError("Command failed with exit code 143", exit_code=143),
        lambda: CLIConnectionError("CLI process ended"),
    ],
    ids=["process_error", "connection_error"],
)
async def test_interrupt_that_makes_the_sdk_raise_sends_interrupted(project, make_error):
    async with interruptible([delta("Partial "), STOP, make_error()], short_turn("Again.")) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        sc.prompt("c1", seq=4)
        await sc.wait_for("text_delta", "c1")

        sc.send(type="interrupt", chat_id="c1", seq=5)
        await sc.wait_for("error", "c1")

        assert client.interrupts == 1
        [error] = [m for m in sc.replies("c1") if m["type"] == "error"]
        assert error["chat_id"] == "c1"
        assert error["kind"] == "interrupted"
        assert isinstance(error["message"], str)
        assert sc.count("result", "c1") == 0

        await sc.ask("c1", seq=6)
        assert sc.count("error", "c1") == 1
