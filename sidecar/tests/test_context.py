"""Spec tests for the reader context prefix (task M4.4).

The task spec names `build_prefix(context) -> str` and the block format from
the plan. These tests fix the parts it leaves open:

1. `reader_sidecar.context.build_prefix(context, root=None) -> str`.
   `context` is a `reader_sidecar.protocol.Context` or None. `root` is the
   project folder (a Path). It is optional so `build_prefix(context)` works.

2. The block, with every field present:

       <reader_context>
       file: papers/attention.pdf (page 12 of 15)
       selection: "Scaled dot-product attention"
       </reader_context>

   followed by one "\n". Lines are joined with "\n".

3. The page belongs to the file line. With `page` but no `page_count` the
   line is `file: <file> (page 12)`. Without `page` it is `file: <file>`,
   even when `page_count` is set. Without `file` there is no file line, so
   `page` and `page_count` alone add nothing.

4. A field that is None or "" is missing. Missing fields drop their line. When
   no line is left, or the context is None, the result is "" (no block).

5. `page` is already 1-based (sidecar/PROTOCOL.md) and is printed as given.

6. With `root`, an absolute `file` inside `root` is printed relative to
   `root` with forward slashes. A relative `file` is printed as given.

7. A selection longer than 8,000 chars keeps its first 8,000 chars, followed
   by a marker of at most 20 chars that contains "[truncated]". The marker
   may sit inside or outside the closing quote. 8,000 chars or fewer pass
   unchanged.

8. A prompt's query is `build_prefix(context, root) + text`, where root is
   the open project. With no context the query is the text unchanged.
"""

from pathlib import Path
from typing import Any

import pytest

from reader_sidecar.context import build_prefix
from reader_sidecar.protocol import Context
from test_chats import Sidecar, short_turn

FILE = "papers/attention.pdf"
SELECTION = "Scaled dot-product attention"
TEXT = "Explain this paragraph."
FULL = (
    "<reader_context>\n"
    "file: papers/attention.pdf (page 12 of 15)\n"
    'selection: "Scaled dot-product attention"\n'
    "</reader_context>\n"
)


def block(*lines: str) -> str:
    return "<reader_context>\n" + "".join(line + "\n" for line in lines) + "</reader_context>\n"


@pytest.fixture
def project(tmp_path: Path) -> Path:
    path = tmp_path / "project"
    path.mkdir()
    return path


# Formatting


def test_full_context():
    ctx = Context(file=FILE, page=12, page_count=15, selection=SELECTION)
    assert build_prefix(ctx) == FULL


def test_no_context_adds_no_block():
    assert build_prefix(None) == ""


def test_context_with_every_field_missing_adds_no_block():
    assert build_prefix(Context()) == ""


def test_empty_strings_count_as_missing():
    assert build_prefix(Context(file="", selection="")) == ""
    assert build_prefix(Context(file="", page=3, page_count=9, selection=SELECTION)) == block(
        f'selection: "{SELECTION}"'
    )


@pytest.mark.parametrize(
    ("fields", "expected"),
    [
        (
            {"file": FILE, "page": 12, "page_count": 15},
            block("file: papers/attention.pdf (page 12 of 15)"),
        ),
        (
            {"file": FILE, "page": 12, "selection": SELECTION},
            block("file: papers/attention.pdf (page 12)", f'selection: "{SELECTION}"'),
        ),
        (
            {"file": FILE, "page_count": 15, "selection": SELECTION},
            block("file: papers/attention.pdf", f'selection: "{SELECTION}"'),
        ),
        ({"file": FILE}, block("file: papers/attention.pdf")),
        ({"selection": SELECTION}, block(f'selection: "{SELECTION}"')),
        ({"page": 12, "page_count": 15, "selection": SELECTION}, block(f'selection: "{SELECTION}"')),
        ({"page": 12, "page_count": 15}, ""),
    ],
    ids=[
        "no_selection",
        "no_page_count",
        "no_page",
        "file_only",
        "selection_only",
        "page_without_file",
        "page_only",
    ],
)
def test_missing_fields_drop_their_line(fields: dict[str, Any], expected: str):
    assert build_prefix(Context(**fields)) == expected


def test_page_is_printed_as_given():
    assert build_prefix(Context(file=FILE, page=1, page_count=1)) == block(
        "file: papers/attention.pdf (page 1 of 1)"
    )


def test_multiline_selection_is_kept():
    selection = "first line\nsecond line"
    assert build_prefix(Context(selection=selection)) == block(f'selection: "{selection}"')


# Truncation


def test_selection_of_8000_chars_is_not_truncated():
    selection = "s" * 8000
    assert build_prefix(Context(selection=selection)) == block(f'selection: "{selection}"')


@pytest.mark.parametrize("extra", [1, 100, 50_000])
def test_selection_over_8000_chars_is_truncated(extra: int):
    selection = "a" * 8000 + "b" * extra
    prefix = build_prefix(Context(file=FILE, page=12, page_count=15, selection=selection))

    lines = prefix.split("\n")
    assert lines[0] == "<reader_context>"
    assert lines[1] == "file: papers/attention.pdf (page 12 of 15)"
    assert lines[-2:] == ["</reader_context>", ""]
    [line] = lines[2:-2]
    head = 'selection: "' + "a" * 8000
    assert line.startswith(head)
    marker = line[len(head) :]
    assert "[truncated]" in marker
    assert "b" not in marker
    assert len(marker) <= 20


# Paths


def test_absolute_file_inside_root_is_relative(tmp_path: Path):
    root = tmp_path / "project"
    ctx = Context(file=str(root / "papers" / "attention.pdf"), page=12, page_count=15, selection=SELECTION)
    assert build_prefix(ctx, root=root) == FULL


def test_relative_file_is_kept_with_root(tmp_path: Path):
    ctx = Context(file=FILE, page=12, page_count=15, selection=SELECTION)
    assert build_prefix(ctx, root=tmp_path / "project") == FULL


# Prompts carry the prefix


@pytest.mark.asyncio
async def test_prompt_query_starts_with_the_prefix(project):
    async with Sidecar(short_turn()) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        sc.send(
            type="prompt",
            chat_id="c1",
            text=TEXT,
            context={"file": FILE, "page": 12, "page_count": 15, "selection": SELECTION},
            seq=4,
        )
        await sc.wait_for("result", "c1")
    assert client.queries == [FULL + TEXT]


@pytest.mark.asyncio
async def test_prompt_with_absolute_file_is_relative_to_the_project(project):
    async with Sidecar(short_turn()) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        path = project / "papers" / "attention.pdf"
        sc.send(
            type="prompt",
            chat_id="c1",
            text=TEXT,
            context={"file": str(path), "page": 12, "page_count": 15, "selection": SELECTION},
        )
        await sc.wait_for("result", "c1")
    assert client.queries == [FULL + TEXT]


@pytest.mark.asyncio
@pytest.mark.parametrize("context", [None, {}], ids=["null", "empty"])
async def test_prompt_without_context_sends_the_text_unchanged(project, context):
    async with Sidecar(short_turn()) as sc:
        sc.open(project)
        client = await sc.new_chat("c1")
        sc.send(type="prompt", chat_id="c1", text=TEXT, context=context)
        await sc.wait_for("result", "c1")
    assert client.queries == [TEXT]
