# Wayland Wire Client (no libwayland)

`libwayland` is banned. The engine speaks the Wayland wire protocol in pure C23 and submits `SEND` / `RECV` / `RECVMSG` / `SENDMSG` on the compositor socket through raw `io_uring`.

## Files

| File | Role |
|---|---|
| `include/khoros/wayland/wire.h`, `src/wayland/wire.c` | Encode / decode headers, u32, i32, padded strings |
| `include/khoros/wayland/client.h`, `src/wayland/client.c` | UNIX socket connect, registry roundtrip, PBUF inbound, skip-success outbound |

## Wire format

Every message is 8-byte header plus payload, 32-bit little-endian:

```
uint32 object_id
uint16 opcode | uint16 size     /* size includes the 8-byte header */
```

Strings: `uint32` length including the NUL, then bytes, padded to 4. `khr_wl_pad4` is `(len + 3) & ~3U`.

Fixed object ids used during the first roundtrip:

| Id | Object |
|---|---|
| `KHR_WL_DISPLAY_ID` (1) | `wl_display` |
| `KHR_WL_REGISTRY_ID` (2) | `wl_registry` (created by `get_registry`) |
| `KHR_WL_CALLBACK_ID` (3) | `wl_callback` (created by `sync`) |

## Connect

`khr_wl_client_connect(client, ring, override_path)`:

- **100% Pure io_uring Direct Descriptors**: When `ring` supports registered files (default on Ring A via `khr_uring_config_ring_a`), socket creation and connection are executed entirely through `io_uring` in a single atomic submission without calling libc `socket()` or `connect()`.
- **Linked SQE Submission**:
  1. `khr_uring_prep_socket(ring, AF_UNIX, SOCK_STREAM, 0, KHR_DIRECT_SLOT_WAYLAND, true, KHR_TAG_WL_SOCKET)` linked with `IOSQE_IO_LINK`. The kernel allocates the socket directly into sparse slot `KHR_DIRECT_SLOT_WAYLAND` (slot 1), stripping `SOCK_CLOEXEC` to satisfy Linux 7.2 requirements for ring-private fixed files.
  2. `khr_uring_prep_connect(ring, KHR_DIRECT_SLOT_WAYLAND, &sun, sizeof(sun), true, KHR_TAG_WL_CONNECT)` with `IOSQE_FIXED_FILE`.
- **Zero POSIX FD Table Allocation**: The Wayland socket never enters the process's file descriptor table, eliminating `fget`/`fput` atomic refcounting and POSIX file table locks.
- **Direct Disconnect**: `khr_wl_client_disconnect` submits `khr_uring_prep_close(client->ring, client->sock_fd, true, KHR_TAG_CLOSE)` directly to the ring, closing the direct slot without libc `close(2)`.

## Roundtrip

`khr_wl_client_roundtrip` builds two requests in one buffer:

1. `wl_display.get_registry(new_id = 2)`
2. `wl_display.sync(new_id = 3)`

Submits `khr_uring_prep_send`, then loops `khr_uring_prep_recv` into `client->in_buf[16384]` until `wl_callback.done`. Both operations automatically apply `IOSQE_FIXED_FILE` when `client->is_direct` is true. Registry `global` events fill `client->globals[]` and cache shortcuts:

- `wl_compositor`
- `xdg_wm_base`
- `wl_shm`
- `wl_seat`
- `wp_linux_drm_syncobj_manager_v1`
- `zwp_linux_dmabuf_v1`

`khr_wl_client_roundtrip` executes with a bounded 2,000 ms timeout per io_uring stage to prevent hangs on disconnect. The protocol is verified via `test_wayland_client_mock_roundtrip` and `test_wayland_direct_socket_connect_and_roundtrip` over a local mock server, guaranteeing complete test coverage of both POSIX and pure direct-descriptor socket pipelines even when headless. Live compositor tests skip cleanly if the desktop socket cannot be opened.

Wire decoding (`khr_wl_decode_string`) enforces mandatory terminating NUL bytes, rejects integer overflows in length fields, and decodes 0-length nullable strings as empty strings (`""`), protecting downstream callers from invalid string memory access.

## PBUF inbound / skip-success outbound

After `khr_wl_client_attach_pbufs(client, tier0, tier1)`:

- `khr_wl_client_arm_inbound` — one multishot `RECVMSG` with `IOSQE_BUFFER_SELECT` on tier 0's `bgid`. `recv_hdr` stays in the client struct for the life of the request.
- `khr_wl_client_send_skip` — `SENDMSG` + `IOSQE_CQE_SKIP_SUCCESS` linked to a NOP barrier, then wait for the NOP.
- `khr_wl_client_process_cqe(client, user_data, res, flags)` — **the steady-state consumer (non-blocking)**. The engine pumps Ring A (`khr_topology_pump` classifies into `evt_q`), then feeds each event here: `WL_RECV`/`WL_RECV_T1` packets resolve to their provided buffer, parse via `recvmsg_out` + the shared wire parser, and return a message count. Foreign tags and `-ENOBUFS` re-arm signals return 0. Buffer ownership stays with the caller (recycle with `khr_topology_recycle_cqe_buffer`). `khr_wl_client_roundtrip` is bootstrap-only (one registry discovery at connect).

Do not reuse `send_hdr` / `send_iov` / the caller's payload until that NOP CQE arrives.

## Not yet

- `xdg_wm_base` / `xdg_surface` / `xdg_toplevel` bind and configure
- `zwp_linux_dmabuf_v1` feedback and buffer params
- `wp_linux_drm_syncobj_surface` acquire/release
- SCM_RIGHTS receive (needs `msg_controllen` + tier 1) and `IORING_OP_FIXED_FD_INSTALL` on the send path
- Replacing the roundtrip `IORING_OP_RECV` loop with the armed multishot path
