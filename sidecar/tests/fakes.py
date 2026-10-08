"""A scripted stand-in for ClaudeSDKClient and a factory that makes it (M4.3).

FakeClient mirrors the parts of claude_agent_sdk.ClaudeSDKClient the sidecar
uses: connect, disconnect, the async context manager, query, and
receive_response. It never starts the CLI.

A script is a list of turns. Each turn is a list of real SDK messages
(StreamEvent, AssistantMessage, ResultMessage, ...). Each receive_response()
call plays the next turn and stops after its ResultMessage, as the real client
does. An asyncio.Event inside a turn pauses the turn until the test sets it.
"""

import asyncio
from collections.abc import AsyncIterator
from typing import Any

from claude_agent_sdk import ClaudeAgentOptions, CLIConnectionError, ResultMessage

Turn = list[Any]


class FakeClient:
    def __init__(self, options: ClaudeAgentOptions, turns: list[Turn]) -> None:
        self.options = options
        self.turns = list(turns)
        self.queries: list[Any] = []
        self.connected = False
        self.disconnects = 0

    async def connect(self, prompt: Any = None) -> None:
        self.connected = True

    async def disconnect(self) -> None:
        self.connected = False
        self.disconnects += 1

    async def __aenter__(self) -> "FakeClient":
        await self.connect()
        return self

    async def __aexit__(self, *exc: Any) -> bool:
        await self.disconnect()
        return False

    async def query(self, prompt: Any, session_id: str = "default") -> None:
        if not self.connected:
            raise CLIConnectionError("Not connected. Call connect() first.")
        self.queries.append(prompt)

    async def receive_response(self) -> AsyncIterator[Any]:
        if not self.connected:
            raise CLIConnectionError("Not connected. Call connect() first.")
        if not self.turns:
            raise AssertionError("FakeClient has no scripted turn left")
        for item in self.turns.pop(0):
            if isinstance(item, asyncio.Event):
                await item.wait()
                continue
            yield item
            if isinstance(item, ResultMessage):
                return


class FakeFactory:
    """`factory(options) -> client`. Every client it makes plays `turns`."""

    def __init__(self, *turns: Turn) -> None:
        self.turns = list(turns)
        self.clients: list[FakeClient] = []

    def __call__(self, options: ClaudeAgentOptions) -> FakeClient:
        client = FakeClient(options, self.turns)
        self.clients.append(client)
        return client
