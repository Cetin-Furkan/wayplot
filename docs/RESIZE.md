# Resize (integer scale + xdg states)

This is the step after `docs/TEXT.md`. The host consumes compositor size, scale, and toplevel states, then rebuilds DMA-BUF images. Not a demo “half-screen button.” Tiling, maximize, fullscreen, and interactive resize are **compositor gestures**; we ack the configure and draw at the new buffer size.

The hole this step closes: `xdg_toplevel.configure` was **not in the proto blob**, so `size_dirty` never fired. `present_begin` already knew how to retire a swapchain. It was never told.

---

## Contract

**Logical size** is `xdg_toplevel.configure` width/height (surface coordinates). `0×0` still means “client default” → 1280×720.

**Integer scale** is `max(1, preferred_buffer_scale, max wl_output.scale of outputs the surface is on)`. Buffer size:

```text
buf = logical × scale   (clamped to [1, 8192])
```

`wl_surface.set_buffer_scale(scale)` goes out on every commit with the attach. `damage_buffer` is in **buffer** pixels.

**States** (bitmask on the session, not a widget): maximized, fullscreen, resizing, activated, tiled (any tiled_*). The app can read them. The host does not invent snap-to-half; GNOME/Hyprland send the size.

**Ack:** every new `xdg_surface.configure` serial is acked on the next present, even if size did not change (activated-only configures).

**Retire:** GPU `vkDeviceWaitIdle`, drop release eventfds, retire the three images, **then** free, then create. Do not free in-flight DMA-BUFs. Wayland destroy/create of `wl_buffer` still goes through `IORING_OP_SENDMSG`.

**Pointer / cursor.** GNOME hides the pointer if the client never sets one after `wl_pointer.enter` (the old client had the same hole). We bind `wp_cursor_shape_manager_v1` and `set_shape` with the **enter** serial (button serial is ignored by the spec). Edge of the surface (12 px) changes to n/s/e/w/diagonal resize cursors and a left-click there is `xdg_toplevel.resize`. The top 36 px (not on an edge) is `xdg_toplevel.move`. That is hit-testing, not a drawn titlebar. `set_shape` is the compositor’s cursor, not a Vulkan overlay.

Keyboard is **not** bound, so keys stay with the compositor. Tablet/touch are not bound. io_uring still carries the Wayland socket; pointer events are ordinary messages on that socket.

---

## Walls

| File | Job |
|---|---|
| `src/wayland/proto.c` | `xdg_toplevel.configure`, `set_buffer_scale`, surface enter/leave/`preferred_buffer_scale`, `wl_output`, `wl_seat`/`wl_pointer`, toplevel move/resize/minmax/max/fullscreen |
| `src/wayland/session.c` | Parse configure+states, outputs, scale, pointer serials, `wp_session_buffer_size` |
| `src/engine/present.c` | Recreate at buffer size, wait GPU, ack, fill `frame.scale` |
| `main.c` | Relayout the label when scale or extent changes. Not a resize UI. |

---

## Tests

| Test | What it locks |
|---|---|
| `test-wayland-proto` | configure opcode 0, `set_buffer_scale` 8, `preferred_buffer_scale` 2, `wl_output.scale` 3 |
| `test-wayland-session` | `scale >= 1`, `buf == logical × scale` |
| `test-engine-present` | Force `size_dirty` to a new logical size; next begin has a matching swapchain |

---

## What we did not do

No cards. No fractional-scale protocol (integer only). No CSD titlebar. No `poll()`. No `CULL_NONE`. No second io_uring.
