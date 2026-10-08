#!/usr/bin/env python3
"""Live check of the sidecar against the real Agent SDK.

Runs sidecar/.venv/bin/reader-sidecar through scripts/clean-env.sh, so the
bundled CLI uses the local ~/.claude login, then sends open_project, new_chat
and one prompt. Prints every line the sidecar writes, then a summary line.
Exits 0 when the reply ends with a result.

usage: scripts/live_smoke.py PROJECT [--prompt TEXT] [--timeout SECONDS]
"""

import argparse
import json
import queue
import subprocess
import sys
import threading
import time
import uuid
from collections import Counter
from pathlib import Path
from typing import IO

REPO = Path(__file__).resolve().parent.parent
CLEAN_ENV = REPO / "scripts" / "clean-env.sh"
SIDECAR = REPO / "sidecar" / ".venv" / "bin" / "reader-sidecar"


def pump(stream: IO[bytes], lines: "queue.Queue[bytes]") -> None:
    for line in stream:
        lines.put(line)
    lines.put(b"")


def main() -> int:
    parser = argparse.ArgumentParser(description="Ask the sidecar one question about a project.")
    parser.add_argument("project", type=Path, help="project folder")
    parser.add_argument("--prompt", default="Summarize sample.pdf in two sentences.")
    parser.add_argument("--timeout", type=float, default=300.0, help="seconds to wait for the reply")
    args = parser.parse_args()

    proc = subprocess.Popen([str(CLEAN_ENV), str(SIDECAR)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    lines: queue.Queue[bytes] = queue.Queue()
    threading.Thread(target=pump, args=(proc.stdout, lines), daemon=True).start()

    chat_id = str(uuid.uuid4())
    script = [
        {"type": "open_project", "path": str(args.project.resolve()), "seq": 1},
        {"type": "new_chat", "chat_id": chat_id, "seq": 2},
        {"type": "prompt", "chat_id": chat_id, "text": args.prompt, "context": None, "seq": 3},
    ]
    proc.stdin.write("".join(json.dumps(msg) + "\n" for msg in script).encode())
    proc.stdin.flush()

    counts: Counter[str] = Counter()
    reply: list[str] = []
    end = None
    deadline = time.monotonic() + args.timeout
    while end is None:
        try:
            line = lines.get(timeout=max(deadline - time.monotonic(), 0))
        except queue.Empty:
            print("# timed out", file=sys.stderr)
            proc.terminate()
            break
        if not line:
            print("# sidecar closed stdout", file=sys.stderr)
            break
        print(line.decode().rstrip("\n"), flush=True)
        msg = json.loads(line)
        counts[msg["type"]] += 1
        if msg["type"] == "text_delta":
            reply.append(msg["text"])
        if msg["type"] in ("result", "error") and msg.get("chat_id") in (chat_id, None):
            end = msg

    proc.stdin.close()
    code = proc.wait(timeout=60)
    print(f"# reply: {''.join(reply)!r}")
    print(
        f"# text_delta: {counts['text_delta']}, tool_use: {counts['tool_use']}, "
        f"end: {end['type'] if end else None}, sidecar exit: {code}"
    )
    return 0 if end is not None and end["type"] == "result" else 1


if __name__ == "__main__":
    sys.exit(main())
