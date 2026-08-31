# Pass folding (the step before a draw list)

This is the step after `docs/RESIZE.md` / cursor. It is **not** cards and
**not** a draw-item array. See `docs/HOST.md` for why this comes first.

---

## The hole

`wp_lit_draw` began **and** ended dynamic rendering (clear color+depth).
`wp_text_draw` began and ended **again** (load color, with a private
`load_clear` flag for the isolated glyph test). Two widgets, two render
scopes. A future list of N objects would open N scopes. That is the old
“each panel is an engine” mistake.

Depth lived on the cube pipeline (`wp_lit` images, `wp_lit_resize`). A
window resize looked like a cube problem.

---

## Contract

- **`wp_pass`** owns depth (one `D32_SFLOAT` image per swapchain slot),
  clear color, and `vkCmdBeginRendering` / `EndRendering`.
- **Opaque:** barrier depth → `DEPTH_ATTACHMENT_OPTIMAL`, begin with color
  **clear** + depth **clear**, full viewport/scissor. Pipelines keep their
  own depth-test / cull state.
- **Overlay:** memory barrier after the opaque scope, begin with color
  **load**, **no** depth attachment, full viewport/scissor.
- **`wp_lit_draw` / `wp_text_draw`** only push descriptors and
  `vkCmdDrawIndexed`. They must run **inside** the matching begin/end.
  They do not begin rendering. They do not take an `VkImageView`.
- Isolated overlay (glyph-only tests) still goes through **opaque begin/end
  with no lit draw** so the clear lives in one place. Text has no
  `load_clear`.
- Present still does not know cameras. The pass does not include Wayland
  headers. `wp_pass_resize` waits GPU idle before replacing depth.

`main`: `present_begin` → `opaque_begin` → lit draws → `opaque_end` →
`overlay_begin` → text draws → `overlay_end` → `present_end`.

---

## Tests

| Test | Lock |
|---|---|
| `test-renderer-pass` | Heap `session`/`present`. Resize depth. One cmd: opaque cube (head-on +Z red at center) then overlay `"H"` with coverage in the glyph AABB, empty opposite corner. A few DMA-BUF frames with both scopes. |
| existing raster / cube / text | Updated to use the pass; still PASS. Raster still locks winding. Text still locks glyph AABB. |

A window that “presented N frames” with nested `BeginRendering` inside
`wp_lit_draw` is not this step.

---

## What we did not do

No draw list struct. No cards. No instancing. No second Slang. No
`CULL_NONE`. No moving present into the renderer. No document type.
