"""Line IO on the standard streams.

The protocol runs on the binary buffers under sys.stdin and sys.stdout.
Reading happens on a daemon thread instead of loop.connect_read_pipe, which is
unreliable for stdin on Windows.
"""

import asyncio
import logging
import os
import sys
import threading
from typing import BinaryIO, TextIO

from .protocol import Outbound, encode

READER_THREAD = "stdin-reader"

log = logging.getLogger(__name__)


class Writer:
    """Writes outbound messages to a binary stream, one line each."""

    def __init__(self, stream: BinaryIO) -> None:
        self._stream = stream
        self._lock = asyncio.Lock()

    async def send(self, msg: Outbound) -> None:
        data = (encode(msg) + "\n").encode("utf-8")
        async with self._lock:
            self._stream.write(data)
            self._stream.flush()


def start_reader(
    stdin: BinaryIO, loop: asyncio.AbstractEventLoop, lines: "asyncio.Queue[bytes]"
) -> threading.Thread:
    """Put each line of `stdin` on `lines` from a daemon thread.

    The thread puts b"" and stops at EOF. A read error counts as EOF, so the
    loop never waits on a dead reader.
    """

    def pump() -> None:
        while True:
            try:
                line = stdin.readline()
            except Exception:
                log.exception("stdin read failed, treating it as EOF")
                line = b""
            try:
                loop.call_soon_threadsafe(lines.put_nowait, line)
            except RuntimeError:
                return  # The event loop is closed.
            if not line:
                return

    thread = threading.Thread(target=pump, name=READER_THREAD, daemon=True)
    thread.start()
    return thread


def setup_streams(stdin: TextIO, stdout: TextIO) -> None:
    """Force UTF-8 on the standard streams and keep \\r out of stdout.

    The protocol encodes and decodes UTF-8 itself on the binary buffers. This
    covers the text layer too. On Windows the C runtime turns \\n into \\r\\n
    on a text-mode descriptor, so both descriptors switch to binary mode.
    """
    stdin.reconfigure(encoding="utf-8")
    stdout.reconfigure(encoding="utf-8", newline="\n")
    if sys.platform == "win32":
        import msvcrt

        for stream in (stdin, stdout):
            msvcrt.setmode(stream.fileno(), os.O_BINARY)
