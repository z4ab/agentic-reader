"""Chat sessions on the Claude Agent SDK, one client per chat.

`Chats` holds the open project and its chats. Each `Chat` owns one client and
runs each reply in its own asyncio task, so the stdin loop keeps reading while
a reply streams.
"""

import asyncio
import logging
import os
import traceback
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

from claude_agent_sdk import (
    AssistantMessage,
    ClaudeAgentOptions,
    ResultMessage,
    StreamEvent,
    ToolUseBlock,
)
from claude_agent_sdk.types import CanUseTool

from .context import build_prefix
from .errors import CRASH_TYPES, error_kind, error_message, redact, turn_kind, turn_message
from .io import Writer
from .permissions import Permissions
from .prompts import READER_PROMPT
from .protocol import Context, Error, Outbound, Result, TextDelta, ToolUse

MODEL_ENV = "READER_MODEL"
READ_ONLY_TOOLS = ["Read", "Glob", "Grep"]
MAX_INPUT_CHARS = 500
TRUNCATED = " [truncated]"

ClientFactory = Callable[[ClaudeAgentOptions], Any]
"""Makes a client from options. claude_agent_sdk.ClaudeSDKClient in production."""

log = logging.getLogger(__name__)


class ChatError(Exception):
    """A request the sidecar refuses. `kind` is the protocol error kind."""

    def __init__(self, kind: str, message: str) -> None:
        super().__init__(message)
        self.kind = kind


def chat_options(root: Path, session_id: str | None, can_use_tool: CanUseTool) -> ClaudeAgentOptions:
    """Options for a chat in `root`. `session_id` resumes a saved session.

    The CLI runs READ_ONLY_TOOLS without asking and awaits `can_use_tool`
    for every other tool.
    """
    return ClaudeAgentOptions(
        cwd=root,
        setting_sources=["project"],
        system_prompt={"type": "preset", "preset": "claude_code", "append": READER_PROMPT},
        include_partial_messages=True,
        allowed_tools=list(READ_ONLY_TOOLS),
        can_use_tool=can_use_tool,
        resume=session_id,
        model=os.environ.get(MODEL_ENV) or None,
    )


def truncate_input(tool_input: dict[str, Any]) -> dict[str, Any]:
    """Cut top-level string values longer than MAX_INPUT_CHARS."""
    return {
        key: value[:MAX_INPUT_CHARS] + TRUNCATED
        if isinstance(value, str) and len(value) > MAX_INPUT_CHARS
        else value
        for key, value in tool_input.items()
    }


def to_outbound(chat_id: str, message: Any) -> Iterator[Outbound]:
    """The protocol messages for one SDK message. Most SDK messages map to none.

    Reply text comes only from stream events. The TextBlocks of an
    AssistantMessage repeat text that already streamed.
    """
    if isinstance(message, StreamEvent):
        event = message.event
        delta = event.get("delta") if event.get("type") == "content_block_delta" else None
        if isinstance(delta, dict) and delta.get("type") == "text_delta":
            yield TextDelta(chat_id=chat_id, text=delta.get("text", ""))
    elif isinstance(message, AssistantMessage):
        for block in message.content:
            if isinstance(block, ToolUseBlock):
                yield ToolUse(chat_id=chat_id, name=block.name, input=truncate_input(block.input))
    elif isinstance(message, ResultMessage):
        yield Result(
            chat_id=chat_id,
            session_id=message.session_id,
            usage=message.usage or {},
            cost_usd=message.total_cost_usd or 0.0,
        )


class Chat:
    """One chat: a client and at most one running reply.

    The client connects at the first prompt, so a chat that is never used
    never starts the CLI.
    """

    def __init__(self, chat_id: str, client: Any, writer: Writer, on_crash: Callable[["Chat"], None]) -> None:
        self.chat_id = chat_id
        self.client = client
        self._writer = writer
        self._on_crash = on_crash
        self._connected = False
        self._interrupted = False
        self._reply: asyncio.Task[None] | None = None

    @property
    def busy(self) -> bool:
        return self._reply is not None and not self._reply.done()

    def start_reply(self, text: str, seq: int | None) -> None:
        self._reply = asyncio.create_task(self._run_reply(text, seq), name=f"reply {self.chat_id}")

    async def _run_reply(self, text: str, seq: int | None) -> None:
        try:
            await self._reply_once(text, seq)
        finally:
            self._interrupted = False

    async def _reply_once(self, text: str, seq: int | None) -> None:
        failure: str | None = None
        try:
            if not self._connected:
                await self.client.connect()
                self._connected = True
            if self._interrupted:
                # Interrupted while connecting, so the query is never sent.
                await self._send_error("reply interrupted", seq, kind="interrupted")
                return
            await self.client.query(text)
            ended = False
            async for message in self.client.receive_response():
                failure = failure or turn_kind(message)
                if isinstance(message, ResultMessage) and message.is_error and failure is not None:
                    if not (self._interrupted and failure == "result"):
                        log.error("chat %s: reply failed with an error result, kind %s", self.chat_id, failure)
                        await self._send_error(turn_message(failure, message), seq, kind=failure)
                        return
                for out in to_outbound(self.chat_id, message):
                    await self._writer.send(out)
                ended = isinstance(message, ResultMessage)
        except Exception as exc:
            if self._interrupted:
                log.info("chat %s: reply raised %s after an interrupt", self.chat_id, type(exc).__name__)
                await self._send_error("reply interrupted", seq, kind="interrupted")
                return
            await self._fail(exc, seq)
            return
        if not ended:
            if self._interrupted:
                await self._send_error("reply interrupted", seq, kind="interrupted")
                return
            log.error("chat %s: reply ended without a result", self.chat_id)
            if failure is not None:
                await self._send_error(turn_message(failure, None), seq, kind=failure)
            else:
                await self._send_error("reply ended without a result", seq)

    async def _fail(self, exc: Exception, seq: int | None) -> None:
        """Send the error for `exc`. A chat whose CLI is gone leaves the registry first.

        The log and the message hold no environment values. The message holds
        no SDK text at all.
        """
        kind = error_kind(exc)
        detail = redact("".join(traceback.format_exception(exc)))
        log.error("chat %s: reply failed, kind %s\n%s", self.chat_id, kind, detail.rstrip())
        crashed = isinstance(exc, CRASH_TYPES)
        if crashed:
            self._on_crash(self)
        await self._send_error(error_message(exc, kind), seq, kind=kind)
        if crashed:
            try:
                await self.client.disconnect()
            except Exception as close_exc:
                log.info("chat %s: disconnect after a crash raised %s", self.chat_id, type(close_exc).__name__)

    async def _send_error(self, message: str, seq: int | None, kind: str = "internal") -> None:
        await self._writer.send(Error(chat_id=self.chat_id, kind=kind, message=message, seq=seq))

    async def interrupt(self) -> None:
        """Ask the CLI to stop the running reply. Does nothing when idle.

        The reply task keeps running and ends with the CLI's result, or with
        an `interrupted` error if the SDK raises. While the client is still
        connecting, only the flag is set, and the reply stops before its query.
        """
        if not self.busy:
            log.info("chat %s: interrupt with no running reply", self.chat_id)
            return
        self._interrupted = True
        if self._connected:
            await self.client.interrupt()

    async def close(self) -> None:
        """Stop the running reply, then disconnect the client and its CLI."""
        if self.busy:
            self._reply.cancel()
            await asyncio.wait([self._reply])
        await self.client.disconnect()


class Chats:
    """The open project and its chats, keyed by the app's chat_id."""

    def __init__(self, writer: Writer, factory: ClientFactory) -> None:
        self._writer = writer
        self._factory = factory
        self._root: Path | None = None
        self._chats: dict[str, Chat] = {}
        self._permissions = Permissions(writer)

    async def open_project(self, path: str) -> None:
        """Make `path` the project. A different project closes every chat."""
        root = Path(path).resolve()
        if root != self._root:
            await self.close()
            self._root = root
            log.info("project: %s", root)

    def add(self, chat_id: str, session_id: str | None) -> None:
        """Create the chat's client. `session_id` resumes a saved session."""
        if self._root is None:
            raise ChatError("no_project", "no project is open")
        if chat_id in self._chats:
            raise ChatError("chat_exists", "chat is already open")
        options = chat_options(self._root, session_id, self._permissions.handler(chat_id))
        client = self._factory(options)
        self._chats[chat_id] = Chat(chat_id, client, self._writer, self._forget)

    def _forget(self, chat: Chat) -> None:
        """Drop a crashed chat so resume_chat can recreate its chat_id."""
        if self._chats.get(chat.chat_id) is chat:
            del self._chats[chat.chat_id]
            self._permissions.deny_chat(chat.chat_id)
            log.info("chat %s: removed after its CLI failed", chat.chat_id)

    def prompt(self, chat_id: str, text: str, context: Context | None, seq: int | None) -> None:
        """Start a reply in the background. Raises ChatError if the chat cannot take it.

        The query is the reader_context block for `context`, then `text`.
        """
        chat = self._get(chat_id)
        if chat.busy:
            raise ChatError("busy", "chat is busy")
        chat.start_reply(build_prefix(context, self._root) + text, seq)

    async def interrupt(self, chat_id: str) -> None:
        await self._get(chat_id).interrupt()

    def permission_reply(self, req_id: str, allow: Any) -> None:
        self._permissions.reply(req_id, allow)

    def _get(self, chat_id: str) -> Chat:
        chat = self._chats.get(chat_id)
        if chat is None:
            raise ChatError("unknown_chat", "chat is not open")
        return chat

    async def close(self) -> None:
        """Deny the pending permission requests of every chat, then close and forget it."""
        chats = list(self._chats.values())
        self._chats.clear()
        for chat in chats:
            self._permissions.deny_chat(chat.chat_id)
        log.debug("closing %d chats", len(chats))
        outcomes = await asyncio.gather(*(chat.close() for chat in chats), return_exceptions=True)
        for chat, outcome in zip(chats, outcomes):
            if isinstance(outcome, Exception):
                log.error("chat %s: close failed", chat.chat_id, exc_info=outcome)
