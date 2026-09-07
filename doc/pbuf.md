# Provided Buffer Rings (PBUF)

Wayplot inbound I/O uses kernel-selected buffers (`IOSQE_BUFFER_SELECT`) instead of a userspace recv buffer per SQE. Two groups are registered on Ring A.

## Files

- `include/khoros/uring/pbuf.h`
- `src/uring/pbuf.c`

## Tiers

| | Tier 0 | Tier 1 |
|---|---|---|
| `bgid` | 0 | 1 |
| Count | 128 | 16 |
| Size | 256 B | 65'536 B (64 KiB) |
| Use | 4000 Hz pointer/key events | registry / fd-passing bursts |

Counts must be powers of two (`khr_pbuf_init` rejects anything else). Kernel maximum is 32768 entries.

## Memory layout

Two anonymous `mmap`s, page-aligned:

1. `struct io_uring_buf_ring` — `entries * sizeof(struct io_uring_buf)`, rounded up to a page
2. Contiguous payload — `entries * buf_size`, rounded up to a page

Registered with `IORING_REGISTER_PBUF_RING` (`nr_args = 1`):

```c
struct io_uring_buf_reg reg = {
    .ring_addr     = (uint64_t)br_ptr,
    .ring_entries  = entries,
    .bgid          = bgid,
};
```

`tail` lives at offset 14 of the ring and overlays `bufs[0].resv`. Never write `resv` when filling a buffer descriptor. Publish tail with a 16-bit release store.

## Recycle

A CQE with `IORING_CQE_F_BUFFER` carries the buffer id in `flags >> IORING_CQE_BUFFER_SHIFT` (`khr_cqe_buf_id`). After parsing, `khr_pbuf_recycle(pbuf, bid)` pushes that id back onto the ring. In topology-managed rings, `khr_topology_recycle_cqe_buffer(topo, &evt)` automatically maps the CQE event to tier 0 or tier 1 and recycles the buffer. During `khr_topology_destroy`, any remaining staged CQEs with buffers are cleanly recycled before ring destruction. Destroy unregisters the group then `munmap`s both mappings.

## `recvmsg_out` parser

Multishot `IORING_OP_RECVMSG` with buffer select writes this into the selected buffer:

```
[struct io_uring_recvmsg_out][name slot][control slot][payload]
```

Name and control **slots** are sized from the **request** `msghdr` (`msg_namelen`, `msg_controllen`), not from `out->namelen` / `out->controllen`. `khr_recvmsg_parse(buf, cap, msg_namelen, msg_controllen, &view)` walks that layout.

For the hot path we currently arm with `msg_namelen = 0` and `msg_controllen = 0`, so payload starts immediately after the 16-byte header. SCM_RIGHTS needs a non-zero `msg_controllen` and should use tier 1 (tier 0 is only 256 B).

`cqe->res` is the `recvmsg(2)` return (payload bytes), not the size of the header.

## Arming inbound

See [wayland.md](wayland.md).
- `khr_wl_client_arm_inbound` arms multishot `RECVMSG` on Tier 0 (`bgid = 0`, tag `KHR_TAG_WL_RECV`).
- `khr_wl_client_arm_inbound_tier1` arms multishot `RECVMSG` on Tier 1 (`bgid = 1`, tag `KHR_TAG_WL_RECV_T1`) for high-volume packets up to 64 KiB.
- Tag-aware buffer recycling (`khr_topology_recycle_cqe_buffer`) routes completions tagged `KHR_TAG_RECVMSG_T1` / `KHR_TAG_WL_RECV_T1` to `pbuf_tier1`, and generic/tier 0 completions to `pbuf_tier0`, preventing cross-tier buffer corruption.
