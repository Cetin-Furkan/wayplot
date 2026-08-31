# CPU draw list (immediate recording)

This is the step after `docs/PASSES.md`. It is **not** cards, not a
document type, not instancing, not bindless. See `docs/HOST.md` for why
the list comes before a card factory.

---

## The hole

`main` still said “spin this cube, stamp this string.” Two call sites, two
engines’ worth of knowledge even after pass folding. Adding a second overlay
run meant editing `main` and hoping `wp_text_draw` did not overwrite the
first run’s host-visible UBO/VBO (it would: one mapped buffer per swapchain
slot, memcpy from offset 0).

A list that can only hold one text item is a fake list.

---

## Contract

- **`wp_draw_list`** is a CPU array of items. Host (`src/engine/draw.c`).
  Renderer still does not know about a list. Present still does not know
  about cameras.
- **Immediate recording.** Each frame: `clear` (or rebuild), `push_*`,
  `record`. No bindless, no indirect, no DGC, no secondary cmd buffers.
- **Two kinds in this step:** `WP_DRAW_LIT` (opaque bucket) and
  `WP_DRAW_TEXT` (overlay bucket). Kind implies bucket. A card later is a
  new kind or a mesh, not a third `BeginRendering`.
- **Pointers, not copies of GPU objects.** Mesh, camera, font, geom stay
  owned by the caller. The list copies the 16-float model and the rgba.
  Pointers must remain valid until after `record`. Changing a geom’s
  origin (relayout) and recording again is how a rect moves.
- **`record` always opens both passes.** Opaque clears even if there is
  no lit item (same rule as pass folding). Overlay loads even if there is
  no text item.
- **`record_opaque` / `record_overlay`** walk one bucket inside an
  already-begun pass. They call `wp_lit_reset` / `wp_text_reset` then
  `DrawIndexed` per item. They do not call `BeginRendering`.
- Cap: `WP_DRAW_MAX` (64). Push returns `-ENOSPC`. Same number is the
  per-slot draw cap on lit/text UBOs.

### Per-draw ranges (why lit/text changed)

One `wp_text_draw` per slot used to `memcpy` the VBO/IBO/UBO from offset 0.
Two overlay items in one command buffer would both execute with the
**second** geom and the **second** color: the GPU has not run yet.

Now each pipeline keeps a per-slot write head:

- `wp_lit_reset` / `wp_text_reset` at the start of that bucket.
- UBO entries are `256`-byte aligned (Vulkan max
  `minUniformBufferOffsetAlignment`). Push descriptor `offset` selects
  the entry. Two colors stay two colors.
- Text vertices/indices append. `vkCmdDrawIndexed(..., firstIndex,
  vertexOffset, ...)` draws that run. Glyphs do not clobber the previous
  run.

Direct callers that draw once per slot still work: used starts at 0.
A long-lived frame loop that records the list resets every pass.

---

## Frame shape

```text
present_begin
  wp_draw_list_clear
  wp_draw_list_push_lit(...)
  wp_draw_list_push_text(...)
  wp_draw_list_record(...)     /* opaque begin/walk/end, overlay begin/walk/end */
present_end
```

`BeginRendering` still lives on `wp_pass`. The list walks.

---

## Tests

| Test | Lock |
|---|---|
| `test-engine-draw` CPU | init, copy of model/rgba, count by kind, clear keeps cap, grow to 40, `-EINVAL` on null, `-ENOSPC` at `WP_DRAW_MAX`. |
| `test-engine-draw` GPU | Heap `session`/`present`. Empty list still clears. Two `"H"` items (white + yellow) in one overlay: both AABBs, **different** colors (UBO not overwritten). Relayout the yellow `H` on the CPU; old AABB empty, new AABB has ink; cube center still +Z red. Clear the list, push only the cube: glyphs gone. A few DMA-BUF frames through the list. |

A window that “presented N frames” with `main` still calling `wp_lit_draw`
directly is not this step.

---

## What we did not do

No card mesh factory. No `x,y,w,h` widget. No instancing. No document
type. No extra Slang. No `CULL_NONE`. No bindless / indirect / DGC.
No moving `BeginRendering` off `wp_pass`.
