# Testing, Lint, Sanitizers

## Commands

```bash
make test       # build/test_runner, log to logs/test_logs/
make sanitize   # ASan + UBSan at -O1 -g3, log to logs/test_logs/sanitize_results.log
make lint       # grep audit for banned headers and libraries
```

Do not commit `build/`, `logs/`, or `*.log`. `.gitignore` already excludes them.

## Suites (`tests/test_runner.c`)

### Suite 1 — ISO C23

| Test | Checks |
|---|---|
| `test_c23_keywords` | `bool`, `alignas` / `alignof`, `nullptr` |
| `test_c23_constexpr_and_literals` | binary literals, digit separators, engine constexprs |
| `test_gnu_source_and_engine_banner` | `_GNU_SOURCE`, banner pointer |

### Suite 2 — Wayland + Ring A

| Test | Checks |
|---|---|
| `test_wayland_wire_encoding_decoding` | header / u32 / string roundtrip |
| `test_wayland_wire_fuzz_and_edge_cases` | truncated headers/strings, buffer overflow protection, 4-byte padding bounds, extreme int32/uint32 |
| `test_wayland_wire_deep_fuzz_and_security` | header size < 8 and unaligned rejection, non-NUL string rejection, integer overflow in length, 0-length nullable string, empty string roundtrip |
| `test_wayland_client_mock_roundtrip` | full roundtrip over local socketpair with mock compositor emitting 5 globals and callback.done over raw io_uring |
| `test_wayland_direct_socket_connect_and_roundtrip` | atomic pure io_uring direct descriptor socket creation + connect (`IOSQE_IO_LINK` + `IOSQE_FIXED_FILE`), zero POSIX fd allocation, fixed-file send/recv, and direct close |
| `test_wayland_pure_ring_native_socket_and_connect` | end-to-end direct descriptor socket lifecycle (socket + connect + accept + direct send/recv + direct close), failure resilience and slot cleanup |
| `test_wayland_raw_uring_roundtrip` | live compositor registry via uring; skips cleanly if no GUI socket |
| `test_xdg_shell_lifecycle` | bind, toplevel, configure, deferred ack, close |
| `test_window_hit_chrome_regions` | corners, edges, move bar, close square, cursor shapes, no chrome in fullscreen |
| `test_window_hit_list_first_match` | first-match rect table, chrome fill order (NE/E beat close), overflow reject |
| `test_window_buffer_size_from_xdg` | 0×0 → default, compositor size used verbatim, double-click jitter |
| `test_plot_demo_samples_in_range` | 256 demo floats in (0,1), not a flat line |
| `test_blob_self_relative_box` | packed `KHRB` box, relptrs, bad magic rejected |
| `test_blob_ingest_leaves_ui_reserve` | ingest lands in payload; high 64 KiB UI bytes unchanged |
| `test_blob_fit_centers_offset_mesh` | AABB fit translates an offset triangle into view |
| `test_shm_wire_pool_and_buffer` | shm pool + buffer create, page-aligned size, FD identity |

### Suite 3 — Wayplot topology

| Test | Checks |
|---|---|
| `test_ring_a_clock_and_no_iowait` | setup flags, registered ring, clock, 1 ms `TIMEOUT` → `-ETIME` |
| `test_msg_ring_64bit_payload` | 64-bit payload Ring B → Ring A |
| `test_pbuf_multishot_recvmsg` | socketpair + tier 0 + `recvmsg_out` |
| `test_pbuf_multishot_cycle_and_exhaustion` | continuous 8-cycle buffer recycling, out-of-bounds bid handling, malformed parse rejection |
| `test_pbuf_tier1_multishot_cycle_and_recycling` | 64 KiB buffer multishot recvmsg with 16 KiB payloads, bid bounds validation, view parsing, and tier 1 recycling |
| `test_sendmsg_skip_success` | skip-success SENDMSG, only NOP CQE, peer received bytes |
| `test_ring_sq_saturation_and_edge_cases` | SQ capacity saturation (returns `nullptr` when full), submission and drain recovery |
| `test_uring_timeout_precision_and_no_double_wait` | verifies kernel enter2 ETIME returns without double-waiting or CPU burning, plus real timeout arrival |
| `test_topology_bringup_and_ipc` | pins, hugepage, buffers, clock, PBUF sizes, MSG_RING BDA |
| `test_topology_ingest_read_fixed` | 4 KiB file through OPENAT2/READ_FIXED into hugepage |
| `test_topology_ingest_edge_cases` | nonexistent file rejection, 0-byte file handling, unaligned byte length (777 B) integrity |
| `test_topology_ingest_multiblock_large_file` | 64 KiB multi-page file ingest with full byte pattern verification and 255-character path boundary |
| `test_ingest_atomic_hardlink_failure_resilience` | 3-SQE atomic hardlinked ingest pipeline resilience against missing files (-ENOENT), 0-byte files, unaligned files, and slot recovery |
| `test_topology_cqe_staging_and_no_swallow` | stages unexpected eventfd and PBUF CQEs during wait_bda, verifies zero event loss and zero buffer leaks |
| `test_topology_cqe_deep_staging_saturation` | 24 datagram socket packets + eventfd arrival during BDA wait; verifies all completions harvested in FIFO order |
| `test_topology_stale_path_and_cmdq` | stack-allocated path safety with memory clobbering, path length boundary checking |
| `test_topology_bda_stress_and_cmdq_saturation` | 50 consecutive BDA IPC roundtrips, plus standalone SPSC CmdQ capacity saturation and FIFO drain |
| `test_eventfd_starvation_watch` | `POLL_ADD` on eventfd fires |
| `test_fixed_fd_install` | `REGISTER_FILES` + `FIXED_FD_INSTALL`, read from new fd |
| `test_sparse_files_registration_and_update` | `IORING_REGISTER_FILES2` sparse table initialization, openat2 directly into sparse slot, close, `IORING_REGISTER_FILES_UPDATE` install and clear (-EBADF validation), unregister |
| `test_direct_descriptor_ingest_pipeline` | direct descriptor slot 0 ingest into registered hugepage buffer, verifies zero POSIX fd allocation (`fcntl` validation) |
| `test_wayland_pbuf_inbound_outbound` | client arm inbound + send_skip over socketpair |

Topology tests that call `khr_topology_init` destroy the topology **before** `TEST_ASSERT` on derived flags, so a failed assert cannot leak the worker.

## Framework (`tests/test_framework.h`)

- `TEST_ASSERT(expr, msg)`, `TEST_ASSERT_EQ`, `TEST_ASSERT_NE`, `TEST_ASSERT_LT`, `TEST_ASSERT_LE`, `TEST_ASSERT_GT`, `TEST_ASSERT_GE`, `TEST_ASSERT_MEM_EQ`, `TEST_ASSERT_NOT_NULL`, `TEST_ASSERT_NULL` record file/line/expr, print a diagnostics box, append `logs/test_logs/failures.log`, and return `false`
- Tracks total assertions verified (`stats->assertions_checked`) for deep test validation metrics
- Per-test millisecond timer, UTF-8 pass bar, end-of-suite failure summary
- `g_khr_test_ctx` is the current test name / stats pointer

## Lint patterns

`make lint` fails on:

- `#include <stdbool.h>` or `<stdalign.h>`
- `liburing.h`, `<sys/epoll.h>`, `<poll.h>`, `epoll_create` / `epoll_ctl` / `epoll_wait`, **`poll(`**
- `wayland-client.h`, `<GL/`, `<GLES/`, `<EGL/`, `glBegin`, `glDraw`

The `poll(` pattern matches any identifier that ends in `poll(`. Do not name functions `khr_something_poll(`. Use `khr_uring_prep_eventfd_watch` / `khr_topology_arm_eventfd`.

CI (`.github/workflows/ci.yml`) repeats the same greps, then `make all`, `make run`, `make test`. GitHub `ubuntu-latest` is not kernel 7.2; Ring A flags may fail there even when they pass on CachyOS.

## Sanitizer notes

- ASan build skips `MAP_HUGETLB` (`__SANITIZE_ADDRESS__`) and uses the THP/anon fallback
- `-pthread` is required; the worker is a real thread
- 32/32 passed under ASan+UBSan (913 assertions verified, 50-run consecutive stress loop clean) on 2026-09-05
