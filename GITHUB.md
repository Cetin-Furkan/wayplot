# GitHub Contribution, Staging & Upload Guidelines

This document explains our strict Git & GitHub workflow: how to give each individual file a dedicated staging and commit command when uploading, and the strict rules governing what must **never** be committed.

---

## 1. The Core Philosophy: Atomic, Explicit Commits

### Why Generic Commits Are Prohibited
Never run blanket commands like:
```bash
# FORBIDDEN:
git add .
git commit -m "update"
git push
```
**Why this is banned:**
1. It commits untracked temporary or binary files by accident.
2. It obscures git history, making bisecting regressions impossible.
3. It violates auditability—every architectural change must have an isolated, explainable rationale.

---

## 2. Dedicated Commands Per File / Component

When uploading updates, each file (or tightly coupled pair) must receive its own dedicated command and descriptive commit message following the Conventional Commits specification: `type(scope): explicit description`.

### Dedicated Command Table

| Target File | Dedicated Git Staging & Commit Command | Purpose & Description |
|---|---|---|
| `include/engine.h` | `git add include/engine.h && git commit -m "feat(include): add C23 engine declarations with single-quote digit separators"` | Public engine header and C23 version guards |
| `src/engine.c` | `git add src/engine.c && git commit -m "feat(src): implement engine startup routine with _GNU_SOURCE check"` | Core engine banner and initialization logic |
| `main.c` | `git add main.c && git commit -m "feat(core): call engine_init from main entrypoint via include directory"` | Engine main entrypoint |
| `include/khoros/uring/` & `src/uring/` | `git add include/khoros/uring/ src/uring/ && git commit -m "feat(uring): implement raw kernel io_uring syscalls and Ring A mmap without liburing"` | Raw io_uring Ring A setup, single mmap, and registered ring FDs |
| `include/khoros/wayland/` & `src/wayland/` | `git add include/khoros/wayland/ src/wayland/ && git commit -m "feat(wayland): implement pure C23 Wayland wire protocol and client over raw io_uring"` | Zero-dependency Wayland wire encoder/decoder and event dispatch |
| `tests/` | `git add tests/ && git commit -m "test(wayland): add wire protocol unit test and live io_uring roundtrip verification"` | Dedicated unit test suite and Wayland live tests |
| `Makefile` | `git add Makefile && git commit -m "build: support recursive sources, make lint, and make sanitize"` | Build automation with `-O3`, sanitizers, and audit targets |
| `gemini.ai` | `git add gemini.ai && git commit -m "docs(agent): append Wayland and raw io_uring implementation to AI notes"` | Persistent agent operation record (append-only) |
| `AGENTS.md` | `git add AGENTS.md && git commit -m "docs(governance): codify AI agent directives and banned library rules"` | Rules of engagement for AI models |
| `DEVELOPER.md` | `git add DEVELOPER.md && git commit -m "docs(dev): add developer architecture and C23 guidelines"` | Architecture and technical specs |
| `README.md` | `git add README.md && git commit -m "docs(readme): establish project overview, prerequisites and quickstart"` | Public repository landing documentation |
| `.gitignore` | `git add .gitignore && git commit -m "chore(git): exclude build artifacts, logs, and test results"` | Protection against uploading untracked files |
| `.github/workflows/ci.yml` | `git add .github/workflows/ci.yml && git commit -m "ci(github): add strict C23 and banned library enforcement pipeline"` | Automated CI checks with explicit commands |
| `GITHUB.md` | `git add GITHUB.md && git commit -m "docs(git): document atomic per-file upload commands and contribution rules"` | This guide |

---

## 3. GitHub Upload Rules: What Must NEVER Be Uploaded

### ⛔ Strictly Prohibited From Being Committed:
1. **Binaries & Compiled Objects**:
   - `build/`, `bin/`, `engine`, `test_runner`, `*.o`, `*.so`, `*.a`.
   - Never commit compiled artifacts. Users and CI must compile from source.
2. **Test Logs & Test Results**:
   - `test_results.log`, `logs/`, `*.log`.
   - Test results are local runtime artifacts and must never pollute source history.
3. **Debug & Core Dump Files**:
   - `core`, `core.*`, `vgcore.*`, `*.gdb`.
4. **Tooling & IDE Cache**:
   - `.cache/`, `.clangd/`, `.vscode/`, `.idea/`, `compile_commands.json`.
5. **Secrets & Credentials**:
   - Private SSH keys, GPG keys, personal access tokens (PATs), `.env` files.
6. **Banned Dependencies or Foreign Wrappers**:
   - Do not vendor or link `liburing`, `libwayland`, `epoll`, or OpenGL source trees.

---

## 4. Pre-Upload Verification Checklist

Before pushing to remote, run these verification commands:

### Step 1: Check for Staged Forbidden Files
```bash
git status --short
```
Ensure no files in `build/` or binary files are staged.

### Step 2: Run Banned Library Audit
```bash
# Must return 0 lines:
grep -rnE "<stdbool\.h>|<stdalign\.h>|liburing|libwayland|<sys/epoll\.h>|<poll\.h>" include/ src/ main.c
```

### Step 3: Run Local Build & Test
```bash
make clean
make all
make run
```

### Step 4: Push to Remote
Once each file has been committed with its dedicated command:
```bash
git push origin <branch-name>
```
