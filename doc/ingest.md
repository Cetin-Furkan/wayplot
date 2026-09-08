# Storage Ingest (OPENAT2 + READ_FIXED)

Hot path for native `.wpbin` (and any other aligned blob): the compute thread opens the file with `O_DIRECT` and DMA-reads it into a buffer previously registered on Ring B.

## Files

- `include/khoros/uring/ingest.h`
- `src/uring/ingest.c`

Must be issued from Ring B's `SINGLE_ISSUER` thread. Topology wraps it as `khr_topology_ingest`.

## Path Safety & Lifetime

`khr_topology_ingest` safely copies caller-provided path strings into an inlined fixed-size buffer (`char path[KHR_PATH_MAX]`, 256 bytes) inside the SPSC command queue item before enqueuing. Callers may pass short-lived stack strings without risking stale pointers or memory corruption on Core 1. Paths exceeding 255 bytes or null pointers are rejected with `false` before enqueue.

## Sequence

Atomic direct descriptor linked pipeline submitted and executed on Ring B in a single `io_uring_enter` submission without process file descriptor table allocation:

1. `IORING_OP_OPENAT2` directly into direct descriptor slot `KHR_DIRECT_SLOT_INGEST` (slot 0) via `khr_uring_prep_openat2` with `struct open_how { .flags = O_RDONLY | (try_odirect ? O_DIRECT : 0) }`, linked with `IOSQE_IO_LINK`.
2. `IORING_OP_READ_FIXED` directly from fixed file slot 0 (`IOSQE_FIXED_FILE`) into registered destination buffer (`buf_index = 0`, hugepage on Ring B) with `len = cap`, `off = 0`, hardlinked with `IOSQE_IO_HARDLINK`.
3. `IORING_OP_CLOSE` releases direct slot 0 directly inside the kernel via `khr_uring_prep_close(ring, KHR_DIRECT_SLOT_INGEST, true, KHR_TAG_INGEST_CLOSE)`.
4. Harvest all 3 completions (`OPENAT2`, `READ_FIXED`, `CLOSE`) in a single wait loop without intermediate userspace context switches. Because `READ_FIXED` uses `IOSQE_IO_HARDLINK`, `CLOSE` is unconditionally executed whenever `OPENAT2` succeeds, ensuring direct descriptor slot 0 is cleanly freed even if `O_DIRECT` returns `-EINVAL` before fallback retry.

This design eliminates the prior 4-step user-kernel roundtrip wait loop (`statx` -> wait -> `openat2` -> wait -> `read_fixed` -> wait -> `close`), completely removes `statx`, and ensures zero POSIX file descriptors are allocated or leaked during storage ingest.

`khr_ingest_read_fixed` returns `0` on success and writes the byte count to `out_bytes`. Negative values are kernel `-errno` from a CQE, or `-1` on local failure (SQ full, wait timeout, tag mismatch).

`struct open_how` lives on the issuer's stack and remains valid until the matching CQE is harvested.

## Alignment

`O_DIRECT` requires 512-byte (often 4 KiB) aligned buffers and sizes. The 2 MiB arena is aligned. Tests write a 4096-byte pattern.

## Cold path (not implemented)

The topology diagram's cold path is: parse `.obj` / `.dem` on core 1, write `.wpbin`, then the hot path above. No parser exists yet.

## Destination today

`dst` is the topology 2 MiB hugepage (THP fallback when `MAP_HUGETLB` is unavailable), registered as Ring B buffer 0. The product window wraps that same mapping as the Vulkan BDA arena.

Memory split (not a screen split): ingest writes `[0, total - 64 KiB)` — the payload. Chrome cards live at the high 64 KiB so DMA cannot smash the title bar. `khr_window_run` submits `khr_topology_ingest_submit` for `./engine file.khrb` and never waits on disk on the present thread.

The packed mesh blob (`khr_blob_t`, magic `KHRB`) uses self-relative pointers. CPU validates once and fits the camera to the AABB; the mesh shader reads verts/indices through BDA. Default with no file is a unit box written into the payload once.

```bash
./build/engine --write-box /tmp/box.khrb
./build/engine /tmp/box.khrb
blender --background --python tools/export_khrb.py -- /tmp/mesh.khrb
blender --background --python tools/export_khrb.py -- --monkey /tmp/suzanne.khrb
./build/engine /tmp/suzanne.khrb
```

The engine does not parse `.blend`. The Python script is the cold path.
