"""Implementer tests for M4.3 cases that test_chats.py leaves out.

Chats before open_project, duplicate and unknown chat IDs, failed replies,
closing a chat mid-reply, a failing disconnect, and path normalization.
"""

import asyncio
from pathlib import Path

import pytest

from test_chats import SESSION, Sidecar, delta, result, short_turn

pytestmark = pytest.mark.asyncio


@pytest.fixture
def project(tmp_path: Path) -> Path:
    path = tmp_path / "project"
    path.mkdir()
    return path


def errors(sc: Sidecar, chat_id: str) -> list[dict]:
    return [m for m in sc.replies(chat_id) if m["type"] == "error"]


@pytest.mark.parametrize("kind", ["new_chat", "resume_chat"])
async def test_chat_without_a_project_is_refused_without_a_client(kind):
    async with Sidecar() as sc:
        sc.send(type=kind, chat_id="c1", session_id=SESSION, seq=2)
        await sc.wait_for("error", "c1")
        assert errors(sc, "c1") == [
            {"type": "error", "chat_id": "c1", "kind": "no_project", "message": "no project is open", "seq": 2}
        ]
        assert sc.clients == []


async def test_duplicate_chat_id_is_refused(project):
    async with Sidecar() as sc:
        sc.open(project)
        await sc.new_chat("c1")
        sc.send(type="resume_chat", chat_id="c1", session_id=SESSION, seq=3)
        await sc.wait_for("error", "c1")
        [error] = errors(sc, "c1")
        assert (error["kind"], error["seq"]) == ("chat_exists", 3)
        assert len(sc.clients) == 1
        assert sc.count("chat_started", "c1") == 1


async def test_prompt_to_unknown_chat_is_refused(project):
    async with Sidecar() as sc:
        sc.open(project)
        sc.prompt("nope", seq=5)
        await sc.wait_for("error", "nope")
        [error] = errors(sc, "nope")
        assert (error["kind"], error["seq"]) == ("unknown_chat", 5)


async def test_failed_reply_sends_internal_error_and_frees_the_chat(project):
    async with Sidecar(short_turn("Recovered.")) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        real_query = client.query

        async def failing_query(prompt, session_id="default"):
            client.query = real_query
            raise RuntimeError("secret detail")

        client.query = failing_query
        sc.prompt("c1", seq=4)
        await sc.wait_for("error", "c1")
        [error] = errors(sc, "c1")
        assert error == {
            "type": "error",
            "chat_id": "c1",
            "kind": "internal",
            "message": "reply failed: RuntimeError",
            "seq": 4,
        }

        await sc.ask("c1", seq=6)
        assert [m["text"] for m in sc.replies("c1") if m["type"] == "text_delta"] == ["Recovered."]


async def test_reply_that_ends_without_a_result_sends_an_error(project):
    async with Sidecar([delta("Half a reply")]) as sc:
        sc.open(project)
        await sc.new_chat("c1")
        sc.prompt("c1", seq=4)
        await sc.wait_for("error", "c1")
        [error] = errors(sc, "c1")
        assert (error["kind"], error["message"], error["seq"]) == (
            "internal",
            "reply ended without a result",
            4,
        )


async def test_eof_mid_reply_stops_the_reply_and_disconnects(project):
    gate = asyncio.Event()
    async with Sidecar([delta("Slow "), gate, delta("never"), result()]) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        sc.prompt("c1")
        await sc.wait_for("text_delta", "c1")
    assert client.disconnects == 1
    assert [m["type"] for m in sc.replies("c1")] == ["text_delta"]


async def test_a_failing_disconnect_does_not_stop_other_chats_closing(project):
    async with Sidecar(short_turn()) as sc:
        sc.open(project)
        c1 = await sc.new_chat("c1")
        c2 = await sc.new_chat("c2")
        await sc.ask("c1")
        await sc.ask("c2")

        async def broken_disconnect():
            raise RuntimeError("disconnect failed")

        c1.disconnect = broken_disconnect
    assert c2.disconnects == 1
    assert not c2.connected


async def test_same_project_by_another_spelling_keeps_chats(project):
    async with Sidecar(short_turn()) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        sc.send(type="open_project", path=f"{project}/../{project.name}/")
        await sc.ask("c1")
        assert client.disconnects == 0
        assert sc.count("error", "c1") == 0
