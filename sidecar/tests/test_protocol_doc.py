"""Checks that sidecar/PROTOCOL.md matches reader_sidecar/protocol.py.

Each `### ` section holds a field table and, for a message, one example line
in a code fence. Field names, their order and their types must match the
dataclass. Inbound examples must decode and set every field. Outbound examples
must equal what encode writes.
"""

import dataclasses
import json
import re
import types
import typing
from pathlib import Path

import pytest

from reader_sidecar.protocol import Context, Inbound, Outbound, decode, encode

DOC = Path(__file__).resolve().parent.parent / "PROTOCOL.md"
INBOUND = typing.get_args(Inbound)
OUTBOUND = typing.get_args(Outbound)
CONTEXT_HEADING = "Context object"
TYPE_NAMES = {str: "string", int: "integer", bool: "boolean", float: "number", Context: "object"}
ROW = re.compile(r"\| `(\w+)` \| ([^|]+?) \|")


def parse_sections(text):
    sections = {}
    name = None
    in_fence = False
    for line in text.splitlines():
        if line.startswith("```"):
            in_fence = not in_fence
        elif in_fence:
            if name is not None:
                sections[name]["examples"].append(line)
        elif line.startswith("### "):
            name = line[4:].replace("`", "")
            sections[name] = {"fields": [], "examples": []}
        elif line.startswith("## "):
            name = None
        elif name is not None and (row := ROW.match(line)):
            sections[name]["fields"].append((row[1], row[2]))
    return sections


SECTIONS = parse_sections(DOC.read_text(encoding="utf-8"))


def doc_type(hint):
    if isinstance(hint, types.UnionType):
        (inner,) = [arg for arg in typing.get_args(hint) if arg is not type(None)]
        return f"{doc_type(inner)} or null"
    if typing.get_origin(hint) is dict:
        return "object"
    return TYPE_NAMES[hint]


def expected_fields(cls):
    hints = typing.get_type_hints(cls)
    return [(f.name, doc_type(hints[f.name])) for f in dataclasses.fields(cls) if f.name != "type"]


def heading(cls):
    return CONTEXT_HEADING if cls is Context else cls.type


def test_doc_has_one_section_per_message():
    assert set(SECTIONS) == {cls.type for cls in INBOUND + OUTBOUND} | {CONTEXT_HEADING}


@pytest.mark.parametrize("cls", INBOUND + OUTBOUND + (Context,), ids=lambda cls: cls.__name__)
def test_doc_fields_match_dataclass(cls):
    assert SECTIONS[heading(cls)]["fields"] == expected_fields(cls)


@pytest.mark.parametrize("cls", INBOUND, ids=lambda cls: cls.__name__)
def test_inbound_example_decodes_with_every_field(cls):
    (line,) = SECTIONS[cls.type]["examples"]

    msg = decode(line)

    assert type(msg) is cls
    assert dataclasses.asdict(msg) == json.loads(line)


@pytest.mark.parametrize("cls", OUTBOUND, ids=lambda cls: cls.__name__)
def test_outbound_example_is_encoder_output(cls):
    (line,) = SECTIONS[cls.type]["examples"]
    fields = {key: value for key, value in json.loads(line).items() if key != "type"}

    assert encode(cls(**fields)) == line
