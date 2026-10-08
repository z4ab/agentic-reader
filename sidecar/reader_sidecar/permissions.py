"""Tool approval through the app.

The CLI runs the read-only tools without asking. For every other tool the SDK
awaits a chat's can_use_tool, which sends `permission_request` and waits for
the app's `permission_reply` with the same req_id.
"""

import asyncio
import logging
import uuid
from typing import Any

from claude_agent_sdk import (
    PermissionResult,
    PermissionResultAllow,
    PermissionResultDeny,
    ToolPermissionContext,
)
from claude_agent_sdk.types import CanUseTool

from .io import Writer
from .protocol import PermissionRequest

PERMISSION_TIMEOUT = 300
"""Seconds to wait for a reply. No reply in time counts as deny."""

USER_DENIED = "User denied"
TIMED_OUT = "No answer from the user in time"
CHAT_CLOSED = "The chat was closed"

log = logging.getLogger(__name__)


class Permissions:
    """Pending permission requests of every chat, keyed by req_id."""

    def __init__(self, writer: Writer) -> None:
        self._writer = writer
        self._pending: dict[str, tuple[str, asyncio.Future[PermissionResult]]] = {}

    def handler(self, chat_id: str) -> CanUseTool:
        """The can_use_tool callback for one chat."""

        async def can_use_tool(
            tool_name: str, tool_input: dict[str, Any], context: ToolPermissionContext
        ) -> PermissionResult:
            return await self.ask(chat_id, tool_name, tool_input)

        return can_use_tool

    async def ask(self, chat_id: str, tool: str, tool_input: dict[str, Any]) -> PermissionResult:
        """Send a permission_request and wait for its answer."""
        req_id = str(uuid.uuid4())
        future: asyncio.Future[PermissionResult] = asyncio.get_running_loop().create_future()
        self._pending[req_id] = (chat_id, future)
        try:
            await self._writer.send(PermissionRequest(req_id=req_id, chat_id=chat_id, tool=tool, input=tool_input))
            return await asyncio.wait_for(future, PERMISSION_TIMEOUT)
        except TimeoutError:
            log.info("chat %s: permission request %s timed out", chat_id, req_id)
            return PermissionResultDeny(message=TIMED_OUT)
        finally:
            self._pending.pop(req_id, None)

    def reply(self, req_id: str, allow: Any) -> None:
        """Answer a pending request. Only `allow is True` allows.

        A reply to an unknown or finished request changes nothing.
        """
        entry = self._pending.get(req_id)
        if entry is None or entry[1].done():
            log.warning("permission reply for unknown request %s", req_id)
            return
        entry[1].set_result(PermissionResultAllow() if allow is True else PermissionResultDeny(message=USER_DENIED))

    def deny_chat(self, chat_id: str) -> None:
        """Deny every pending request of `chat_id`."""
        for owner, future in list(self._pending.values()):
            if owner == chat_id and not future.done():
                future.set_result(PermissionResultDeny(message=CHAT_CLOSED))
