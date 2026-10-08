# Git guide

Every agent working a task follows this file. `tasks/VERIFY.md` covers tests and screenshots.

## Branches and worktrees

- `main` always builds and passes every test. Only the orchestrator commits to or merges into `main`.
- One task is one branch and one worktree. The orchestrator creates both from the current `main`:
  ```
  git worktree add .worktrees/<id> -b task/<file-slug> main
  ```
  Example: task file `m2.1-pdfdocument-wrapper.md` → branch `task/m2.1-pdfdocument-wrapper`, worktree `.worktrees/m2.1`.
- Work only inside your worktree and use absolute paths. The one exception is screenshots, which go to `<repo>/.verify/<id>/`.
- Each worktree has its own `build/` and `sidecar/.venv/`. Never touch another worktree or the main checkout.
- Never create, remove, or switch branches or worktrees. The orchestrator does that.

## Commits

Format: `<type>(<id>): <imperative summary>`, 72 characters or fewer. Add a body when the reason for the change is not obvious from the diff.

Types: `feat`, `fix`, `test`, `refactor`, `build`, `ci`, `docs`, `chore`.

Examples:

```
test(m2.1): add spec tests for PdfDocument
build(m1.1): add vcpkg manifest and CMake skeleton
feat(m2.1): convert poppler ARGB32 images to wxImage
fix(m2.1): address review 1
docs(m2.1): record evidence and mark done
```

Rules:

1. One logical change per commit. A reviewer should be able to read each commit on its own.
2. Run `git status`, stage explicit paths (`git add <paths>`, never `git add -A` or `git add .`), and read `git diff --staged` before committing.
3. Never commit build outputs, venvs, screenshots, secrets, `.env` files, debug prints, or commented-out code.
4. Never add `Co-Authored-By`, "Generated with", or any other attribution lines.
5. Never push, force-push, use `--no-verify`, or rewrite history once review has started. Review fixes are new commits named `fix(<id>): address review <n>`.
6. Task file updates (`status`, `evidence`, `notes`) go in their own `docs(<id>)` commit.

## Review and merge

1. When the implementer reports done, the orchestrator starts a reviewer that reads `git diff main...HEAD`.
2. The reviewer answers `VERDICT: APPROVE` or `VERDICT: CHANGES` with numbered blockers. The implementer fixes blockers in new commits and the reviewer checks again.
3. On approval, the orchestrator rebases the branch onto `main` if `main` moved. The implementer resolves any conflicts on the branch, never on `main`.
4. The orchestrator merges with `git merge --no-ff -m "Merge <ID>: <title>"`, builds and tests `main`, then removes the worktree and deletes the branch.
5. If `main` breaks after a merge, the orchestrator undoes the merge and sends the branch back.

## Shared files

These files change in many tasks: `app/src/MainFrame.cpp`, `app/CMakeLists.txt`, `tests/CMakeLists.txt`, `.github/workflows/build.yml`. Add new entries one per line in sorted order, and do not reformat lines you did not change.
