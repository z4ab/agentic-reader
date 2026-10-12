# Verification guide

Every agent working a task follows this file. `tasks/GIT.md` covers branches and commits.

## Roles

Three agents work each task, in order, in the same worktree.

1. Tester. Writes the tests from the task file and the plan only, before any implementation exists. Writes no implementation code.
2. Implementer. Writes the code and makes the tester's tests pass. Never edits tester-owned files.
3. Reviewer. A fresh agent that wrote neither. Never edits code. Checks the spec, the tests, the commits, and the screenshots.

## Tester-owned files

- The test files and test data the task names.
- The test registrations in `tests/CMakeLists.txt` (C++) or the files under `sidecar/tests/` the task names (Python).
- `tests/acceptance/<id>.steps` for checks you can see in the UI.

The tester lists these files in the commit body under `Owned:`. The reviewer confirms `git diff <tester-commit> HEAD -- <owned files>` is empty. The implementer may add more tests in new files.

## Interfaces

- Use the signatures the spec names.
- Where the spec is silent, the tester picks the simplest interface and states it in a comment at the top of the test file.
- The implementer conforms. If a test contradicts the spec or cannot pass, report `TEST DISPUTE: <file:line> <reason>` and stop. The orchestrator decides.
- C++ tests may fail to compile until the implementation exists. That is expected.

## Test names

- A test must match the task's verify command. `ctest -R pdf_document` needs a ctest name containing `pdf_document`.
- App code (everything in `app/src` except `main.cpp`) builds as the static library `reader_core`. Tests link `reader_core`.

## Screenshots

Every task with a visible result is checked with screenshots, taken headless under Xvfb.

```
scripts/shot.sh -o <repo>/.verify/<id>/<impl|review> -s tests/acceptance/<id>.steps -- ./build/app/agentic-reader [args]
```

Steps file, one command per line, `#` for comments:

```
wait 1
key ctrl+o
type hello
click 400 300
dblclick 400 300
drag 100 100 400 300
keydown ctrl
click 400 300 4
keyup ctrl
shot opened-sample
```

`shot <name>` writes `NN-<name>.png`. `click <x> <y> <button>` clicks another button (4 and 5 are the wheel). `keydown` and `keyup` hold and release a key, for example Ctrl around wheel clicks. The window sits at the screen origin, so coordinates are window coordinates. Each `shot` line should be preceded by a comment stating what the image must show.

Rules:

1. Never launch the GUI outside `shot.sh`. Never set `GDK_BACKEND=wayland`. Nothing may open on the user's desktop.
2. The implementer runs the steps and Reads every PNG before reporting done.
3. The reviewer runs the steps again into `review/`, Reads every PNG, states what each one shows, and treats a mismatch with the expected result as a blocker.

## Live Claude checks

- Run the sidecar, `scripts/live_smoke.py`, or the app through `scripts/clean-env.sh`. It strips `CLAUDECODE`, `CLAUDE_CODE_*`, and `ANTHROPIC_*`, so the bundled CLI uses the local `~/.claude` login.
- After M7.2 lands, enable "Use my local Claude Code login" in settings before a live check.
- Keep live prompts short. Never print or log an API key.

## Human checks

Some checks cannot be automated: dragging from a file manager, perceived smoothness, comparing counts with Papers. Record each one in the task's `notes` as `human check: <what to do>` and do not block on it. Use `pdftotext` as the automated proxy where the task compares against Papers.

## Builds

```
export VCPKG_ROOT=$HOME/vcpkg
cmake -B build -S . -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j6
ctest --test-dir build --output-on-failure
```

Run any command that may take longer than 10 minutes (the first vcpkg build) in the background and wait for it to finish. The vcpkg binary cache makes later worktrees fast.
