"""The reader-sidecar entry point: read app messages from stdin and route them."""

import asyncio
import logging
import os
import signal
import sys
import threading
from typing import Any, BinaryIO, Callable

from . import __version__
from .io import READER_THREAD, Writer, setup_streams, start_reader
from .protocol import (
    Error,
    Inbound,
    Interrupt,
    NewChat,
    OpenProject,
    PermissionReply,
    Prompt,
    ProtocolError,
    Ready,
    ResumeChat,
    decode,
)

LOG_ENV = "READER_SIDECAR_LOG"

log = logging.getLogger(__name__)


class Router:
    """One handler per inbound message type. The handlers are stubs until M4.3."""

    def __init__(self, writer: Writer) -> None:
        self._writer = writer

    async def open_project(self, msg: OpenProject) -> None:
        _stub(msg)

    async def prompt(self, msg: Prompt) -> None:
        _stub(msg)

    async def new_chat(self, msg: NewChat) -> None:
        _stub(msg)

    async def resume_chat(self, msg: ResumeChat) -> None:
        _stub(msg)

    async def interrupt(self, msg: Interrupt) -> None:
        _stub(msg)

    async def permission_reply(self, msg: PermissionReply) -> None:
        _stub(msg)

    async def close(self) -> None:
        """Close all chats."""
        log.debug("closing chats")


def _stub(msg: Inbound) -> None:
    log.info("%s: not implemented yet", msg.type)


async def serve(stdin: BinaryIO, writer: Writer, router: Any) -> int:
    """Send `ready`, then handle stdin lines until EOF. Returns the exit code.

    Each line is handled to completion before the next. `router.close()` runs
    on EOF and when the loop is cancelled.
    """
    lines: asyncio.Queue[bytes] = asyncio.Queue()
    await writer.send(Ready(version=__version__))
    start_reader(stdin, asyncio.get_running_loop(), lines)
    try:
        while line := await lines.get():
            await _handle(line, writer, router)
        log.info("stdin closed")
    finally:
        await router.close()
    return 0


async def _handle(line: bytes, writer: Writer, router: Any) -> None:
    try:
        msg = decode(line.decode("utf-8"))
    except ProtocolError as exc:
        await writer.send(Error(kind="protocol", message=str(exc), seq=exc.seq))
        return
    except (ValueError, RecursionError) as exc:
        # Not UTF-8, an integer too long to convert, or nesting too deep.
        await writer.send(Error(kind="protocol", message=f"invalid line: {exc}"))
        return

    try:
        await getattr(router, msg.type)(msg)
    except Exception:
        log.exception("%s handler failed", msg.type)
        await writer.send(
            Error(
                chat_id=getattr(msg, "chat_id", None),
                kind="internal",
                message=f"{msg.type} failed",
                seq=msg.seq,
            )
        )


def _setup_logging() -> None:
    name = os.environ.get(LOG_ENV, "INFO").strip().upper()
    level = logging.getLevelNamesMapping().get(name)
    logging.basicConfig(
        stream=sys.stderr,
        level=logging.INFO if level is None else level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    if level is None:
        log.warning("%s=%r is not a log level, using INFO", LOG_ENV, name)


def _on_sigterm(loop: asyncio.AbstractEventLoop, callback: Callable[[], None]) -> None:
    try:
        loop.add_signal_handler(signal.SIGTERM, callback)
    except NotImplementedError:  # Windows event loops
        signal.signal(signal.SIGTERM, lambda signum, frame: loop.call_soon_threadsafe(callback))


async def _main(stdin: BinaryIO, stdout: BinaryIO) -> int:
    writer = Writer(stdout)
    task = asyncio.create_task(serve(stdin, writer, Router(writer)))

    def terminate() -> None:
        # A second SIGTERM must not interrupt router.close().
        if not task.cancelling():
            log.info("SIGTERM received")
            task.cancel()

    _on_sigterm(asyncio.get_running_loop(), terminate)
    try:
        return await task
    except asyncio.CancelledError:
        current = asyncio.current_task()
        if current is not None and current.cancelling():
            raise
        return 0


def run() -> int:
    """Console script `reader-sidecar`. Returns the process exit code."""
    setup_streams(sys.stdin, sys.stdout)
    _setup_logging()
    code = asyncio.run(_main(sys.stdin.buffer, sys.stdout.buffer))
    if any(thread.name == READER_THREAD for thread in threading.enumerate()):
        # After SIGTERM the reader thread is still in readline and holds the
        # lock of sys.stdin.buffer. Interpreter shutdown would abort when it
        # closes stdin, so skip it. Writer and the log handler flush per line.
        logging.shutdown()
        os._exit(code)
    return code
