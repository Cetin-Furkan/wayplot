# Agent Directives & Repository Operating Rules

This file outlines the mandatory operational and architectural rules for all AI agents working in this repository.

## 1. Permission & File Boundary Discipline
- **Explicit Permission Required**: Never create, edit, or delete files outside of the explicitly authorized scope provided by the user in each prompt.
- **No Unsolicited File Creation**: Do not proactively generate scratch files, auxiliary scripts, or modifications unless explicitly asked.

## 2. Persistent Agent Memory Logs
- `gemini.ai` — Gemini session log.
- `grok.ai` — Grok session log.
- Both files live at the project root. They are independent; do not merge or rewrite one into the other.
- **IMMUTABILITY OF PAST NOTES**: **NEVER** overwrite, modify, truncate, or delete previous notes in `gemini.ai` or `grok.ai`.
- **APPEND-ONLY**: When recording notes or progress, append to the bottom of the log that belongs to this agent, prefixed with the current ISO 8601 timestamp (e.g. `[YYYY-MM-DDTHH:MM:SS+TZ]`). Grok appends to `grok.ai`. Gemini appends to `gemini.ai`.
- Subsystem manuals live in `doc/` as Markdown. Prefer updating those docs when behavior changes; still append a dated note to the agent log.

## 3. Mandatory C23 Standard Rules
- **Standard Flag**: Code must compile strictly under `-std=c23` with `-Wall -Wextra -Wpedantic -Werror=vla`.
- **Banned Headers**:
  - `<stdbool.h>` is **STRICTLY BANNED**. Built-in `bool`, `true`, `false` are language keywords.
  - `<stdalign.h>` is **STRICTLY BANNED**. Built-in `alignas` and `alignof` are language keywords.
  - Guard every header and source file against accidental inclusion with `#ifdef _STDBOOL_H` and `#ifdef _STDALIGN_H`.
- **C23 Idioms**:
  - Use `nullptr` instead of `NULL` or `(void*)0`.
  - Use `constexpr` for scalar constants and array bounds.
  - Use empty braces `= {}` for zero initialization.
  - Use standard attributes (`[[nodiscard]]`, `[[maybe_unused]]`).
  - Use native `static_assert(expr, msg)` and `static_assert(expr)`.

## 4. Subsystem Prohibitions
- **I/O Engine**:
  - `liburing` is **BANNED**. Use direct raw syscalls (`__NR_io_uring_setup`, `__NR_io_uring_enter`, `__NR_io_uring_register`).
  - `epoll` (`sys/epoll.h`) and `poll` (`poll.h`) are **STRICTLY BANNED**.
- **Graphics Pipeline**:
  - `libwayland` is **STRICTLY BANNED**.
  - `OpenGL`, `GLX`, `EGL` are **STRICTLY BANNED**.
  - Vulkan 1.4 (`VK_API_VERSION_1_4`) is the exclusive graphics and compute API, utilizing Direct-to-Display (`VK_KHR_display`) and offscreen compute.

## 5. Workflow & Automation
- In all CI configurations and scripts, write explicit, verbose shell commands with flags. Avoid opaque or generic wrappers.
