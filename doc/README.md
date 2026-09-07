# Khoros Engine Documentation

Subsystem manuals for the Khoros / Wayplot engine. Each file covers one part of the tree. Read this index first, then the file that matches the code you are touching.

| Document | Covers |
|---|---|
| [architecture.md](architecture.md) | Dual-thread Wayplot topology, process layout, what is implemented vs not |
| [c23.md](c23.md) | ISO C23 rules, banned headers, required idioms |
| [io-uring.md](io-uring.md) | Raw `io_uring` syscalls, Ring A / Ring B, SQE helpers, MSG_RING |
| [pbuf.md](pbuf.md) | Tiered provided-buffer rings and `recvmsg_out` parsing |
| [topology.md](topology.md) | CPU pinning, hugepages, worker thread, BDA IPC, eventfd watch |
| [wayland.md](wayland.md) | Pure-C23 Wayland wire protocol and uring-backed client |
| [ingest.md](ingest.md) | `OPENAT2` + `READ_FIXED` storage hot path |
| [testing.md](testing.md) | Test suites, lint, sanitizers, logging |
| [build.md](build.md) | Makefile, flags, CI |
| [graphics.md](graphics.md) | Vulkan 1.4 / KMS / Slang roadmap (not implemented yet) |
| [kernel-notes.md](kernel-notes.md) | Kernel 7.2 facts that bit us; read before changing uring code |

Agent memory lives at the repository root, **not** in this directory:

- [`../grok.ai`](../grok.ai) — Grok session log. **Append-only.** Never delete, overwrite, or truncate prior notes.
- [`../gemini.ai`](../gemini.ai) — Parallel Gemini session log. Same append-only rule.

Operating rules for agents: [`../AGENTS.md`](../AGENTS.md).
