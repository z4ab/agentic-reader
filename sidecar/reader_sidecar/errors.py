"""Map Agent SDK failures to protocol error kinds and safe messages.

Error messages never quote SDK exception text, CLI stderr, or result text,
because those can hold environment values. Each message is a fixed sentence,
plus a few short codes (terminal_reason, HTTP status, exit code) that pass a
strict pattern.
"""

import os
import re
from typing import Any

from claude_agent_sdk import (
    CLIConnectionError,
    CLINotFoundError,
    ProcessError,
    RateLimitEvent,
    ResultError,
    ResultMessage,
)

CLI_MISSING = "cli_missing"
PROCESS = "process"
RESULT = "result"
AUTH = "auth"
RATE_LIMIT = "rate_limit"
INTERNAL = "internal"

# Exceptions after which the CLI process is gone. The chat must be recreated
# with resume_chat.
CRASH_TYPES = (CLIConnectionError, ProcessError)

# Signals of an auth failure or a rate limit in SDK text. The CLI reports an
# API error as `API Error: <status> <JSON body>`.
_AUTH_TEXT = re.compile(r"Invalid API key|authentication_error|API Error: 401\b")
_RATE_TEXT = re.compile(r"rate_limit_error|API Error: 429\b")
_AUTH_STATUS = 401
_RATE_STATUS = 429
# AssistantMessage.error values.
_ASSISTANT_KINDS = {"authentication_failed": AUTH, "rate_limit": RATE_LIMIT}

_CODE = re.compile(r"[a-z][a-z0-9_]{0,63}")
_SECRET_NAME = re.compile(r"^ANTHROPIC_|KEY|TOKEN|SECRET|PASSWORD", re.IGNORECASE)
_MIN_SECRET_LEN = 6
REDACTED = "[redacted]"

_MESSAGES = {
    CLI_MISSING: "the agent CLI was not found",
    AUTH: "authentication failed: the API key is missing or invalid",
    RATE_LIMIT: "rate limit reached, try again later",
}


def error_kind(exc: BaseException) -> str:
    """The protocol error kind for an exception raised by a chat's client."""
    if isinstance(exc, CLINotFoundError):
        return CLI_MISSING
    if isinstance(exc, ProcessError):
        status = exc.api_error_status if isinstance(exc, ResultError) else None
        kind = _api_kind(status, _exception_text(exc))
        if kind:
            return kind
        return RESULT if isinstance(exc, ResultError) else PROCESS
    if isinstance(exc, CLIConnectionError):
        return PROCESS
    return INTERNAL


def error_message(exc: BaseException, kind: str) -> str:
    """A message for `exc` that holds no SDK text and no environment values."""
    if kind in _MESSAGES:
        return _MESSAGES[kind]
    if kind == RESULT and isinstance(exc, ResultError):
        return result_message(exc.terminal_reason, exc.subtype)
    if kind == PROCESS:
        code = getattr(exc, "exit_code", None)
        suffix = f" (exit code {code})" if isinstance(code, int) else ""
        return f"the agent process failed{suffix}"
    return f"reply failed: {type(exc).__name__}"


def result_message(terminal_reason: str | None, subtype: str | None) -> str:
    """Message for an error result. Carries terminal_reason when it is a plain code."""
    parts = [code for code in (terminal_reason, subtype) if code and _CODE.fullmatch(code)]
    return "reply ended with an error" + (f": {', '.join(dict.fromkeys(parts))}" if parts else "")


def turn_message(kind: str, result: ResultMessage | None) -> str:
    """Message for a failure reported inside a turn. `result` is its error result, if any."""
    if kind in _MESSAGES:
        return _MESSAGES[kind]
    if result is not None:
        return result_message(result.terminal_reason, result.subtype)
    return "reply failed"


def turn_kind(message: Any) -> str | None:
    """The failure an SDK message reports inside a turn, or None.

    AssistantMessage.error and a rejected RateLimitEvent name auth and rate
    limit failures. A ResultMessage with is_error is `result` unless its
    status or text shows auth or a rate limit.
    """
    if isinstance(message, RateLimitEvent):
        return RATE_LIMIT if message.rate_limit_info.status == "rejected" else None
    if isinstance(message, ResultMessage):
        if not message.is_error:
            return None
        text = " ".join([message.result or "", *(message.errors or [])])
        return _api_kind(message.api_error_status, text) or RESULT
    error = getattr(message, "error", None)
    return _ASSISTANT_KINDS.get(error) if isinstance(error, str) else None


def redact(text: str) -> str:
    """Replace the values of secret-looking environment variables in `text`."""
    for name, value in os.environ.items():
        if len(value) >= _MIN_SECRET_LEN and _SECRET_NAME.search(name):
            text = text.replace(value, REDACTED)
    return text


def _api_kind(status: int | None, text: str) -> str | None:
    if status == _AUTH_STATUS or _AUTH_TEXT.search(text):
        return AUTH
    if status == _RATE_STATUS or _RATE_TEXT.search(text):
        return RATE_LIMIT
    return None


def _exception_text(exc: ProcessError) -> str:
    parts = [str(exc), exc.stderr or ""]
    if isinstance(exc, ResultError):
        parts += [exc.result or "", *exc.errors]
    return "\n".join(parts)
