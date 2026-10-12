#!/usr/bin/env python3
"""Checks after each run of tests/acceptance/m6.4.sh.

usage: m6.4-check.py run1|run2|run3 FIXTURE

FIXTURE is /tmp/agentic-reader-m6.4 (layout in m6.4.sh). Reads the fake
sidecar logs (FIXTURE/fake-run<N>.log, format in m6.4-fake-sidecar.py),
FIXTURE/proj/.reader/project.json and FIXTURE/proj/.reader/chats/*.jsonl.
Prints "m6.4-check: <run> ok" or "m6.4-check: FAIL <reason>" and exits 1 on
the first mismatch. The messages below are the ones the steps files type.
"""

import json
import os
import sys

A1 = "ALPHA one: what does the opening page of this paper say regarding our method?"
A2 = "ALPHA two: and what is on page 2?"
B1 = "BRAVO one: summarize the paper in one line"
B2 = "BRAVO two: back in this chat"
F1 = "FAILFIRST one: this first reply fails #fail"
D1 = "DOOMED one: remember the word PLUM #doom"
C1 = "CHARLIE one: this chat gets deleted"
A3 = "ALPHA three: what did I ask first?"
F2 = "FAILFIRST two: try again"
D2 = "DOOMED two: what word did I ask you to remember?"
N1 = "NEW one: continue where we left off"
B3 = "BRAVO three: this reply crashes the agent #crash"
B4 = "BRAVO four: are you still there?"
A4 = "ALPHA four: after the restart"
L1 = "LOOSE one: first unsaved chat"
L2 = "LOOSE two: second unsaved chat"

COST = 0.0010


def fail(reason):
    print(f"m6.4-check: FAIL {reason}", file=sys.stderr)
    sys.exit(1)


def expect(cond, reason):
    if not cond:
        fail(reason)


def read_log(path):
    """The log as a list of processes, each a list of JSON objects."""
    expect(os.path.isfile(path), f"no fake sidecar log at {path}")
    procs = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                fail(f"{path}:{n}: the app sent a line that is not JSON: {line[:200]}")
            if obj.get("fake") == "start":
                procs.append([])
                continue
            expect(procs, f"{path}:{n}: message before the fake started")
            procs[-1].append(obj)
    expect(procs, f"{path}: no fake sidecar process started")
    return procs


def of_type(proc, *types):
    return [m for m in proc if m.get("type") in types]


def fake_events(proc, event):
    return [m for m in proc if m.get("fake") == event]


def opens(proc):
    """(type, chat_id, session_id) of every new_chat and resume_chat, in order."""
    return [(m["type"], m.get("chat_id"), m.get("session_id"))
            for m in of_type(proc, "new_chat", "resume_chat")]


def prompts(proc):
    return [(m.get("chat_id"), m.get("text", "")) for m in of_type(proc, "prompt")]


def replies(procs, chat_id):
    return [m["text"] for proc in procs for m in fake_events(proc, "reply")
            if m.get("chat_id") == chat_id]


def errors(proc):
    return [(m.get("kind"), m.get("chat_id")) for m in fake_events(proc, "error")]


class Project:
    def __init__(self, fix):
        self.reader = os.path.join(fix, "proj", ".reader")
        path = os.path.join(self.reader, "project.json")
        expect(os.path.isfile(path), f"missing {path}")
        with open(path, encoding="utf-8") as f:
            self.json = json.load(f)
        self.chats = self.json.get("chats", [])
        expect(isinstance(self.chats, list), "project.json chats is not a list")
        self.dir = os.path.join(self.reader, "chats")

    def transcript(self, chat_id):
        path = os.path.join(self.dir, chat_id + ".jsonl")
        if not os.path.exists(path):
            return []
        out = []
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                try:
                    obj = json.loads(line)
                except ValueError:
                    fail(f"{path}:{n} is not JSON: {line[:200]}")
                expect(isinstance(obj, dict), f"{path}:{n} is not an object")
                out.append(obj)
        return out

    def by_first_message(self, text):
        """The chat whose transcript starts with the user message `text`."""
        found = [c for c in self.chats
                 if (t := self.transcript(c["id"])) and t[0].get("role") == "user"
                 and t[0].get("text") == text]
        expect(len(found) == 1,
               f"expected one chat starting with {text!r}, found {len(found)}")
        return found[0]

    def jsonl_files(self):
        if not os.path.isdir(self.dir):
            return set()
        return {n[:-len(".jsonl")] for n in os.listdir(self.dir) if n.endswith(".jsonl")}


def check_roles_texts(project, chat, expected, label):
    """`expected` is a list of (role, text or None); None means any text."""
    t = project.transcript(chat["id"])
    got = [(m.get("role"), m.get("text")) for m in t]
    expect(len(t) == len(expected),
           f"{label}: transcript has {len(t)} lines, expected {len(expected)}: {got}")
    for i, ((role, text), m) in enumerate(zip(expected, t)):
        expect(m.get("role") == role, f"{label}: line {i + 1} role {m.get('role')!r}, expected {role!r}")
        if text is not None:
            expect(m.get("text") == text,
                   f"{label}: line {i + 1} text {m.get('text')!r}, expected {text!r}")
        expect(isinstance(m.get("ts"), str) and m["ts"], f"{label}: line {i + 1} has no ts")
        if role == "assistant":
            cost = m.get("cost_usd")
            expect(isinstance(cost, (int, float)) and abs(cost - COST) < 1e-9,
                   f"{label}: line {i + 1} cost_usd {cost!r}, expected {COST}")


def check_assistant_lines(project, chat, procs, label):
    """Every assistant line is a whole reply the fake finished with `result`."""
    got = [m.get("text") for m in project.transcript(chat["id"]) if m.get("role") == "assistant"]
    want = replies(procs, chat["id"])
    expect(got == want, f"{label}: assistant lines {got!r}, the fake's replies were {want!r}")


def no_text_under(roots, needle, label):
    for root in roots:
        for dirpath, _, files in os.walk(root):
            for name in files:
                if name == "sidecar.log":
                    continue  # the app's log of the sidecar's stderr
                path = os.path.join(dirpath, name)
                try:
                    with open(path, "rb") as f:
                        data = f.read()
                except OSError:
                    continue
                expect(needle.encode() not in data, f"{label}: {needle!r} was written to {path}")


def run1(fix):
    procs = read_log(os.path.join(fix, "fake-run1.log"))
    expect(len(procs) == 1, f"run 1: the fake started {len(procs)} times, expected once")
    proc = procs[0]
    project = Project(fix)

    a = project.by_first_message(A1)
    b = project.by_first_message(B1)
    f = project.by_first_message(F1)
    d = project.by_first_message(D1)
    ids = {c["id"] for c in project.chats}
    expect(len(project.chats) == 4 and ids == {a["id"], b["id"], f["id"], d["id"]},
           f"run 1: project.json should hold exactly the ALPHA, BRAVO, FAILFIRST and DOOMED "
           f"chats, it holds {[(c['id'], c.get('title')) for c in project.chats]}")
    expect(project.jsonl_files() == ids,
           f"run 1: chats/ should hold one .jsonl per chat, it holds {sorted(project.jsonl_files())}")
    no_text_under([project.reader], "CHARLIE", "run 1 (deleted chat)")

    expect(a.get("title") == A1[:60], f"run 1: ALPHA title {a.get('title')!r}, expected {A1[:60]!r}")
    expect(b.get("title") == B1, f"run 1: BRAVO title {b.get('title')!r}")
    expect(f.get("title") == F1, f"run 1: FAILFIRST title {f.get('title')!r}")
    expect(d.get("title") == "Doomed renamed", f"run 1: DOOMED title {d.get('title')!r}")

    expect(a.get("session_id") == "sess-" + a["id"], f"run 1: ALPHA session_id {a.get('session_id')!r}")
    expect(b.get("session_id") == "sess-" + b["id"], f"run 1: BRAVO session_id {b.get('session_id')!r}")
    expect(f.get("session_id") is None,
           f"run 1: FAILFIRST never got a result, session_id should be null: {f.get('session_id')!r}")
    expect(d.get("session_id") == "sess-doomed-" + d["id"],
           f"run 1: DOOMED session_id {d.get('session_id')!r}")

    check_roles_texts(project, a, [("user", A1), ("assistant", None), ("user", A2), ("assistant", None)], "ALPHA")
    check_roles_texts(project, b, [("user", B1), ("assistant", None), ("user", B2), ("assistant", None)], "BRAVO")
    check_roles_texts(project, f, [("user", F1)], "FAILFIRST")
    check_roles_texts(project, d, [("user", D1), ("assistant", None)], "DOOMED")
    for chat, label in ((a, "ALPHA"), (b, "BRAVO"), (d, "DOOMED")):
        check_assistant_lines(project, chat, procs, label)

    for chat in project.chats:
        expect(chat.get("created") and chat.get("updated") and chat["created"] <= chat["updated"],
               f"run 1: chat {chat['id']} has created {chat.get('created')!r}, updated {chat.get('updated')!r}")
    expect(a["updated"] < f["updated"] < d["updated"] < b["updated"],
           "run 1: updated should order ALPHA < FAILFIRST < DOOMED < BRAVO: "
           f"{a['updated']} {f['updated']} {d['updated']} {b['updated']}")

    got = opens(proc)
    expect(all(t == "new_chat" for t, _, _ in got),
           f"run 1: every chat lived in this sidecar, so no resume_chat was due: {got}")
    expect(len(got) == 5, f"run 1: expected 5 new_chat (start, BRAVO, FAILFIRST, DOOMED, CHARLIE), got {got}")
    expect(len({cid for _, cid, _ in got}) == 5, f"run 1: new_chat reused a chat_id: {got}")

    sent = prompts(proc)
    texts = [t for _, t in sent]
    expect(texts == [A1, A2, B1, F1, D1, B2, C1], f"run 1: prompts sent were {texts}")
    expect([cid for cid, _ in sent[:6]] == [a["id"], a["id"], b["id"], f["id"], d["id"], b["id"]],
           f"run 1: prompts went to the wrong chats: {sent}")
    expect(sent[6][0] not in ids, "run 1: CHARLIE's prompt went to a chat that still exists")
    expect(errors(proc) == [("result", f["id"])],
           f"run 1: the only sidecar error should be FAILFIRST's result error, got {errors(proc)}")
    print("m6.4-check: run1 ok")


def run2(fix):
    procs = read_log(os.path.join(fix, "fake-run2.log"))
    expect(len(procs) == 2,
           f"run 2: expected the fake to start twice (start, restart after the crash), got {len(procs)}")
    p1, p2 = procs
    run1_procs = read_log(os.path.join(fix, "fake-run1.log"))
    project = Project(fix)

    a = project.by_first_message(A1)
    b = project.by_first_message(B1)
    f = project.by_first_message(F1)
    d = project.by_first_message(D1)
    old = {a["id"], b["id"], f["id"], d["id"]}
    new = [c for c in project.chats if c["id"] not in old]
    expect(len(project.chats) == 5 and len(new) == 1,
           f"run 2: expected the 4 chats of run 1 and one new chat, got "
           f"{[(c['id'], c.get('title')) for c in project.chats]}")
    n = new[0]

    # Process 1: the opens, in order.
    want = [("resume_chat", b["id"], "sess-" + b["id"]),    # at start: newest chat
            ("resume_chat", a["id"], "sess-" + a["id"]),    # End: ALPHA
            ("new_chat", f["id"], None),                    # FAILFIRST, no session_id
            ("resume_chat", d["id"], "sess-doomed-" + d["id"]),  # DOOMED
            ("new_chat", n["id"], None)]                    # Continue in new chat
    expect(opens(p1) == want, f"run 2, first sidecar: opens were\n  {opens(p1)}\nexpected\n  {want}")

    sent = prompts(p1)
    expect([s for s in sent if s[0] != n["id"]] == [(a["id"], A3), (f["id"], F2), (d["id"], D2), (b["id"], B3)],
           f"run 2, first sidecar: prompts were {sent}")
    to_new = [t for cid, t in sent if cid == n["id"]]
    expect(to_new, "run 2: no prompt reached the continued chat")
    first = to_new[0]
    doomed_lines = project.transcript(d["id"])
    expect(len(doomed_lines) == 3, f"run 2: DOOMED should hold 3 lines, holds {len(doomed_lines)}")
    for m in doomed_lines:
        expect(m.get("text") and m["text"] in first,
               f"run 2: the continued chat's first prompt lacks the DOOMED message {m.get('text')!r}:\n{first}")
    expect(any(t.rstrip().endswith(N1) for t in to_new),
           f"run 2: no prompt to the continued chat ends with the typed message: {to_new}")
    expect(errors(p1) == [("process", d["id"])],
           f"run 2, first sidecar: the only error should be DOOMED's failed resume, got {errors(p1)}")
    expect(fake_events(p1, "crash"), "run 2: the first sidecar did not crash on BRAVO three")

    # Process 2: after the crash the selected chat (BRAVO) is resumed once
    # the sidecar is ready, before anything else; ALPHA when selected.
    expect(of_type(p2, "open_project"), "run 2: the restarted sidecar got no open_project")
    want2 = [("resume_chat", b["id"], "sess-" + b["id"]),
             ("resume_chat", a["id"], "sess-" + a["id"])]
    expect(opens(p2) == want2, f"run 2, restarted sidecar: opens were {opens(p2)}, expected {want2}")
    first_chat_msg = next((m for m in p2 if m.get("type") in ("new_chat", "resume_chat", "prompt")), None)
    expect(first_chat_msg and first_chat_msg.get("type") == "resume_chat"
           and first_chat_msg.get("chat_id") == b["id"],
           f"run 2: the restarted sidecar's first chat message should resume BRAVO: {first_chat_msg}")
    expect(prompts(p2) == [(b["id"], B4), (a["id"], A4)], f"run 2, restarted sidecar: prompts were {prompts(p2)}")
    expect(errors(p2) == [], f"run 2, restarted sidecar: errors {errors(p2)}")

    # The records on disk.
    both = run1_procs + procs
    expect(f.get("session_id") == "sess-" + f["id"],
           f"run 2: FAILFIRST's first result should store its session_id: {f.get('session_id')!r}")
    expect(d.get("session_id") == "sess-doomed-" + d["id"], f"run 2: DOOMED session_id {d.get('session_id')!r}")
    expect(n.get("session_id") == "sess-" + n["id"], f"run 2: continued chat session_id {n.get('session_id')!r}")
    check_roles_texts(project, a, [("user", A1), ("assistant", None), ("user", A2), ("assistant", None),
                                   ("user", A3), ("assistant", None), ("user", A4), ("assistant", None)], "ALPHA")
    check_roles_texts(project, b, [("user", B1), ("assistant", None), ("user", B2), ("assistant", None),
                                   ("user", B3), ("user", B4), ("assistant", None)], "BRAVO")
    check_roles_texts(project, f, [("user", F1), ("user", F2), ("assistant", None)], "FAILFIRST")
    check_roles_texts(project, d, [("user", D1), ("assistant", None), ("user", D2)], "DOOMED")
    for chat, label in ((a, "ALPHA"), (b, "BRAVO"), (f, "FAILFIRST"), (d, "DOOMED"), (n, "continued")):
        check_assistant_lines(project, chat, both, label)
    n_lines = project.transcript(n["id"])
    expect(any(m.get("role") == "user" and m.get("text", "").rstrip().endswith(N1) for m in n_lines),
           f"run 2: the continued chat's transcript lacks the typed message: {n_lines}")
    alpha_replies = replies(both, a["id"])
    expect(len(alpha_replies) == 4 and ("first asked: " + A1[:80]) in alpha_replies[2],
           f"run 2: ALPHA three's reply should recall the first question: {alpha_replies}")
    expect(project.jsonl_files() == {c["id"] for c in project.chats},
           f"run 2: chats/ holds {sorted(project.jsonl_files())}")
    order = sorted(project.chats, key=lambda c: c["updated"], reverse=True)
    expect([c["id"] for c in order[:2]] == [a["id"], b["id"]],
           f"run 2: ALPHA then BRAVO should be the most recently updated: {[c.get('title') for c in order]}")
    print("m6.4-check: run2 ok")


def run3(fix):
    procs = read_log(os.path.join(fix, "fake-run3.log"))
    expect(len(procs) == 1, f"run 3: the fake started {len(procs)} times")
    proc = procs[0]
    expect(not os.path.exists(os.path.join(fix, "loose", ".reader")),
           "run 3: an unsaved project got a .reader/ folder")
    for needle in ("LOOSE", "Loose renamed"):
        no_text_under([os.path.join(fix, "loose"), os.path.join(fix, "home"),
                       os.path.join(fix, "proj")], needle, "run 3 (unsaved project)")
    got = opens(proc)
    expect([t for t, _, _ in got] == ["new_chat", "new_chat"],
           f"run 3: expected two new_chat and no resume_chat, got {got}")
    expect([t for _, t in prompts(proc)] == [L1, L2], f"run 3: prompts were {prompts(proc)}")
    expect(errors(proc) == [], f"run 3: errors {errors(proc)}")
    print("m6.4-check: run3 ok")


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("run1", "run2", "run3"):
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    {"run1": run1, "run2": run2, "run3": run3}[sys.argv[1]](sys.argv[2])


if __name__ == "__main__":
    main()
