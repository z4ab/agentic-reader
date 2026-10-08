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

Each task lands on `main` as one squashed commit. Branch commits exist for the reviewer and are discarded at merge, so keep them few: usually one for the tests, one or two for the implementation, one per review round, and one for the task record.

Format: `<type>(<id>): <imperative summary>`, 72 characters or fewer. Add a body when the reason for the change is not obvious from the diff.

Types: `feat`, `fix`, `test`, `refactor`, `build`, `ci`, `docs`, `chore`.

Examples:

```
test(m2.1): add spec tests for PdfDocument
feat(m2.1): add PdfDocument wrapper with rendering and errors
fix(m2.1): address review 1
docs(m2.1): record evidence and mark done
```

Rules:

1. Do not commit every small step. Commit when a coherent piece works.
2. Run `git status`, stage explicit paths (`git add <paths>`, never `git add -A` or `git add .`), and read `git diff --staged` before committing.
3. Never commit build outputs, venvs, screenshots, secrets, `.env` files, debug prints, or commented-out code.
4. Never add `Co-Authored-By`, "Generated with", or any other attribution lines.
5. Never push, force-push, use `--no-verify`, or rewrite history once review has started. Review fixes are new commits named `fix(<id>): address review <n>`.
6. Task file updates (`status`, `evidence`, `notes`) go in one `docs(<id>)` commit at the end.

## Review and merge

1. When the implementer reports done, the orchestrator starts a reviewer that reads `git diff main...HEAD`.
2. The reviewer answers `VERDICT: APPROVE` or `VERDICT: CHANGES` with numbered blockers. The implementer fixes blockers in new commits and the reviewer checks again.
3. On approval, if `main` moved and the squash would conflict, the orchestrator rebases the branch onto `main` first. The implementer resolves any conflicts on the branch, never on `main`.
4. The orchestrator squash-merges: `git merge --squash task/<file-slug>`, then one commit on `main`:
   ```
   feat(m2.1): PdfDocument wrapper

   <2-4 lines: what the task added and anything notable from notes>

   Task: tasks/m2-pdf-view/m2.1-pdfdocument-wrapper.md
   Reviewed: approved after <n> review rounds
   ```
   The type is the one that best describes the task as a whole (`feat` for features, `build` or `ci` for tooling).
5. The orchestrator builds and tests `main`, then removes the worktree and deletes the branch with `git branch -D`.
6. If `main` breaks after a merge, the orchestrator reverts the squash commit and sends the branch back.

## Shared files

These files change in many tasks: `app/src/MainFrame.cpp`, `app/CMakeLists.txt`, `tests/CMakeLists.txt`, `.github/workflows/build.yml`. Add new entries one per line in sorted order, and do not reformat lines you did not change.
