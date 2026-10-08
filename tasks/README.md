# Tasks

Plan: `/home/zaid/.claude/plans/sorted-coalescing-tulip.md`. Read it before starting any task. It holds the architecture, sidecar protocol, project format, and risks R1–R4.

## Layout

One folder per milestone, one file per task: `tasks/m<milestone>-<name>/m<id>-<slug>.md`. The task ID is the file name prefix and the file's `#` heading.

Find work with grep:

```
grep -l "status: todo" tasks/*/*.md
grep -l "status: doing" tasks/*/*.md
```

## How to work a task

Agents also follow `tasks/GIT.md` (branches, commits, review, merge) and `tasks/VERIFY.md` (independent tests, screenshots, live checks).

1. Pick the lowest-ID task with `status: todo` whose `depends` are all `done`.
2. Set `status: doing` in that task's file.
3. Do only what the task describes. Out-of-scope work you notice goes in `notes` or a new task file, not into the change.
4. Check every `done when` item. Run every `verify` command.
5. Fill `evidence` with a commit hash, CI link, or test output. Then set `status: done`.
6. If you cannot finish, set `status: blocked` and name the blocker in `notes`, either a task ID or a question for the user.

Status values: `todo | doing | blocked | done`. IDs are `M<milestone>.<n>`. Never renumber or reuse an ID. Tasks marked `owner: human` need the user and an agent must not attempt them.

## Conventions

- C++20, wxWidgets 3.2+, CMake 3.25+, vcpkg manifest mode.
- C++ tests use doctest and run through `ctest`. Python tests use pytest + pytest-asyncio.
- C++ naming: `PascalCase` types, `camelCase` methods, `m_` member prefix (wx style). One class per `.h/.cpp` pair.
- The UI thread is the only thread that touches wx windows or `wxBitmap`. Worker threads pass `wxImage` or plain data via `wxThreadEvent`.
- The sidecar writes protocol messages to stdout only. All logging goes to stderr.
- Never log or print the API key.
- Product name is "Agentic Reader". Never use "Claude Code" in UI text or branding (R4).

Build commands (from M1.1 onward):

```
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
ctest --test-dir build --output-on-failure
cd sidecar && python -m venv .venv && .venv/bin/pip install -e '.[dev]' && .venv/bin/pytest
```
