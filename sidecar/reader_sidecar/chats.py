"""Chat sessions on the Claude Agent SDK, one client per chat.

`Chats` holds the open project and its chats. Each `Chat` owns one client and
runs each reply in its own asyncio task, so the stdin loop keeps reading while
a reply streams.
"""

import asyncio
import logging
import os
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

from claude_agent_sdk import (
    AssistantMessage,
    ClaudeAgentOptions,
    PermissionResult,
    PermissionResultAllow,
    PermissionResultDeny,
    ResultMessage,
    StreamEvent,
    ToolPermissionContext,
    ToolUseBlock,
)

from .io import Writer
from .prompts import READER_PROMPT
from .protocol import Error, Outbound, Result, TextDelta, ToolUse

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


async def deny_unlisted_tools(
    tool_name: str, tool_input: dict[str, Any], context: ToolPermissionContext
) -> PermissionResult:
    """Placeholder permission handler until M4.4 asks the user.

    The CLI runs READ_ONLY_TOOLS without asking, so this only sees other
    tools, and it denies them all.
    """
    if tool_name in READ_ONLY_TOOLS:
        return PermissionResultAllow()
    return PermissionResultDeny(message=f"Agentic Reader does not allow {tool_name} yet.")


def chat_options(root: Path, session_id: str | None) -> ClaudeAgentOptions:
    """Options for a chat in `root`. `session_id` resumes a saved session."""
    return ClaudeAgentOptions(
        cwd=root,
        setting_sources=["project"],
        system_prompt={"type": "preset", "preset": "claude_code", "append": READER_PROMPT},
        include_partial_messages=True,
        allowed_tools=list(READ_ONLY_TOOLS),
        can_use_tool=deny_unlisted_tools,
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

    def __init__(self, chat_id: str, client: Any, writer: Writer) -> None:
        self.chat_id = chat_id
        self.client = client
        self._writer = writer
        self._connected = False
        self._reply: asyncio.Task[None] | None = None

    @property
    def busy(self) -> bool:
        return self._reply is not None and not self._reply.done()

    def start_reply(self, text: str, seq: int | None) -> None:
        self._reply = asyncio.create_task(self._run_reply(text, seq), name=f"reply {self.chat_id}")

    async def _run_reply(self, text: str, seq: int | None) -> None:
        try:
            if not self._connected:
                await self.client.connect()
                self._connected = True
            await self.client.query(text)
            ended = False
            async for message in self.client.receive_response():
                for out in to_outbound(self.chat_id, message):
                    await self._writer.send(out)
                ended = isinstance(message, ResultMessage)
        except Exception as exc:
            # M4.5 maps SDK errors to finer kinds. The message names only the
            # exception type, so no CLI output or environment reaches the app.
            log.exception("chat %s: reply failed", self.chat_id)
            await self._send_error(f"reply failed: {type(exc).__name__}", seq)
            return
        if not ended:
            log.error("chat %s: reply ended without a result", self.chat_id)
            await self._send_error("reply ended without a result", seq)

    async def _send_error(self, message: str, seq: int | None) -> None:
        await self._writer.send(Error(chat_id=self.chat_id, kind="internal", message=message, seq=seq))

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
        client = self._factory(chat_options(self._root, session_id))
        self._chats[chat_id] = Chat(chat_id, client, self._writer)

    def prompt(self, chat_id: str, text: str, seq: int | None) -> None:
        """Start a reply in the background. Raises ChatError if the chat cannot take it."""
        chat = self._chats.get(chat_id)
        if chat is None:
            raise ChatError("unknown_chat", "chat is not open")
        if chat.busy:
            raise ChatError("busy", "chat is busy")
        chat.start_reply(text, seq)

    async def close(self) -> None:
        """Close and forget every chat."""
        chats = list(self._chats.values())
        self._chats.clear()
        log.debug("closing %d chats", len(chats))
        outcomes = await asyncio.gather(*(chat.close() for chat in chats), return_exceptions=True)
        for chat, outcome in zip(chats, outcomes):
            if isinstance(outcome, Exception):
                log.error("chat %s: close failed", chat.chat_id, exc_info=outcome)
