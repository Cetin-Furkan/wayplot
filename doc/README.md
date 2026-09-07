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
| [graphics.md](graphics.md) | Vulkan 1.4 device, BDA, Slang pipelines, offscreen frame, DMA-BUF slots (index line was stale: this is implemented; the remaining hole is `make run` composition — see [open-problems.md](open-problems.md)) |
| [kernel-notes.md](kernel-notes.md) | Kernel 7.2 facts that bit us; read before changing uring code |
| [open-problems.md](open-problems.md) | 2026-09-07 punch list: uncommitted R2/R3, composition gap, data-path honesty, nits, doc drift. Read before starting the next edit. |

Agent memory lives at the repository root, **not** in this directory. All three are **append-only**. Never delete, overwrite, or truncate prior notes. Do not merge them.

- [`../grok.ai`](../grok.ai) — Grok session log.
- [`../muse.ai`](../muse.ai) — Muse session log.
- [`../gemini.ai`](../gemini.ai) — Gemini session log.

Operating rules for agents: [`../AGENTS.md`](../AGENTS.md).
