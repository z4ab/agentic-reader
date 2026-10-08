"""The <reader_context> block that starts each prompt.

It tells Claude what the user sees: the open file, the page, and the
selected text. READER_PROMPT explains the block to Claude.
"""

from pathlib import Path
from typing import Any

from .protocol import Context

MAX_SELECTION_CHARS = 8000
TRUNCATED = " [truncated]"


def build_prefix(context: Context | None, root: Path | None = None) -> str:
    """The block for `context`, ending in a newline, or "" when it has no fields.

    A field that is None or "" is missing and drops its line. The page belongs
    to the file line, so without a file the page is dropped too. An absolute
    file inside `root` is printed relative to it.
    """
    if context is None:
        return ""
    lines = []
    if _present(context.file):
        line = f"file: {_display_path(context.file, root)}"
        if _present(context.page):
            if _present(context.page_count):
                line += f" (page {context.page} of {context.page_count})"
            else:
                line += f" (page {context.page})"
        lines.append(line)
    if _present(context.selection):
        selection = context.selection
        if len(selection) > MAX_SELECTION_CHARS:
            selection = selection[:MAX_SELECTION_CHARS] + TRUNCATED
        lines.append(f'selection: "{selection}"')
    if not lines:
        return ""
    return "<reader_context>\n" + "".join(line + "\n" for line in lines) + "</reader_context>\n"


def _present(value: Any) -> bool:
    return value is not None and value != ""


def _display_path(file: str, root: Path | None) -> str:
    """`file` relative to `root` with forward slashes, or `file` unchanged.

    The resolved spelling is tried second, so a path through a symlink still
    matches the resolved project root.
    """
    path = Path(file)
    if root is None or not path.is_absolute():
        return file
    for candidate, base in ((path, root), (path.resolve(), root.resolve())):
        try:
            return candidate.relative_to(base).as_posix()
        except ValueError:
            pass
    return file
