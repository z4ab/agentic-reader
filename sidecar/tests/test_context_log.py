"""Spec tests for the M5.3 debug check: at DEBUG the sidecar log shows the
<reader_context> block of each prompt with the right values.

The task's "done when" says: sidecar log at DEBUG shows the `<reader_context>`
block with the right values (verify: a run with READER_SIDECAR_LOG=DEBUG, whose
level handling tests/test_run.py covers). These tests fix the parts it leaves
open:

1. When a prompt with a context arrives, a logger under `reader_sidecar` writes
   one DEBUG record whose message contains the block exactly as Claude gets it,
   i.e. `build_prefix(context, project root)` (relative file, page "of" count,
   the selection in quotes, every line of a multi-line selection), without
   needing its trailing newline.
2. The block is logged at DEBUG only: no record above DEBUG contains
   "<reader_context>" or the selected text, so the default INFO log keeps the
   user's reading out.
3. A prompt without context logs no block.
"""

import logging
from pathlib import Path

import pytest

from reader_sidecar.context import build_prefix
from reader_sidecar.protocol import Context
from test_chats import Sidecar, short_turn

SELECTION = (
    "Scaled dot-product attention computes a weighted sum of the values,\n"
    "where each weight comes from a naïve softmax"
)
TEXT = "what does this mean?"


@pytest.fixture
def project(tmp_path: Path) -> Path:
    path = tmp_path / "project"
    (path / "papers").mkdir(parents=True)
    return path


def sidecar_records(caplog: pytest.LogCaptureFixture) -> list[logging.LogRecord]:
    return [r for r in caplog.records if r.name == "reader_sidecar" or r.name.startswith("reader_sidecar.")]


async def send_prompt(project: Path, context: dict | None) -> None:
    async with Sidecar(short_turn()) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        sc.send(type="prompt", chat_id="c1", text=TEXT, context=context, seq=7)
        await sc.wait_for("result", "c1")
    assert len(client.queries) == 1


@pytest.mark.asyncio
async def test_debug_log_shows_the_reader_context_block(project, caplog):
    caplog.set_level(logging.DEBUG, logger="reader_sidecar")
    path = project / "papers" / "attention.pdf"
    await send_prompt(project, {"file": str(path), "page": 3, "page_count": 4, "selection": SELECTION})

    block = build_prefix(
        Context(file=str(path), page=3, page_count=4, selection=SELECTION), project.resolve()
    ).rstrip("\n")
    assert block == (
        "<reader_context>\n"
        "file: papers/attention.pdf (page 3 of 4)\n"
        f'selection: "{SELECTION}"\n'
        "</reader_context>"
    )
    debug = [r.getMessage() for r in sidecar_records(caplog) if r.levelno == logging.DEBUG]
    assert any(block in message for message in debug), f"no DEBUG record holds the block; DEBUG records: {debug}"


@pytest.mark.asyncio
async def test_debug_log_shows_a_block_without_selection(project, caplog):
    caplog.set_level(logging.DEBUG, logger="reader_sidecar")
    path = project / "papers" / "attention.pdf"
    await send_prompt(project, {"file": str(path), "page": 3, "page_count": 4, "selection": None})

    block = "<reader_context>\nfile: papers/attention.pdf (page 3 of 4)\n</reader_context>"
    debug = [r.getMessage() for r in sidecar_records(caplog) if r.levelno == logging.DEBUG]
    assert any(block in message for message in debug), f"no DEBUG record holds the block; DEBUG records: {debug}"


@pytest.mark.asyncio
async def test_block_is_not_logged_above_debug(project, caplog):
    caplog.set_level(logging.DEBUG, logger="reader_sidecar")
    path = project / "papers" / "attention.pdf"
    await send_prompt(project, {"file": str(path), "page": 3, "page_count": 4, "selection": SELECTION})

    louder = [r.getMessage() for r in sidecar_records(caplog) if r.levelno > logging.DEBUG]
    for message in louder:
        assert "<reader_context>" not in message
        assert "naïve softmax" not in message


@pytest.mark.asyncio
@pytest.mark.parametrize("context", [None, {}], ids=["null", "empty"])
async def test_prompt_without_context_logs_no_block(project, caplog, context):
    caplog.set_level(logging.DEBUG, logger="reader_sidecar")
    await send_prompt(project, context)

    assert not any("<reader_context>" in r.getMessage() for r in sidecar_records(caplog))
