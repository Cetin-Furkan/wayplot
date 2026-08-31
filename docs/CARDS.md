# Two cards as data

This is the step after `docs/CARD.md`. A card is `x,y,w,h` on the CPU. The
GPU tessellation is derived. Changing a width is a memory write plus a
record — not a shader edit. See `docs/HOST.md`.

---

## The hole

The factory could draw **one** rect. `main` still said “make a 220×96
panel.” Size lived in the caller as tessellation arguments that vanished
after `wp_card_cpu`. Two panels would have been two hardcoded geoms, same
class of bug as `320×460` vs `580×340`.

## Contract

- **`struct wp_rect`** is the source of truth (`x, y, w, h`, pixel space).
  `wp_rect_ok` is `w > 0 && h > 0`. `wp_rect_scaled` applies integer
  buffer scale.
- **`wp_draw_list_push_card`** copies the rect (and rgba). It does not
  store a geom pointer. Invalid rect is `-EINVAL`.
- **Record tessellates** with `wp_card_cpu` from the copied rect. The
  caller does not keep triangles around.
- Two cards in one overlay pass keep their own color (per-draw UBO from
  the list step).
- Overlay order is still cards then text.

`main` holds two logical rects and scales them at push time. They are
callers, not a document type.

## Tests

| Test | Lock |
|---|---|
| `test-renderer-card` CPU | List copies the rect; caller can change `w` after push. Two items. Zero width rejected. |
| `test-renderer-card` GPU | Magenta A and green B, non-overlapping. Each AABB is its color, not the other. Shrink B’s `w`; A unmoved; the strip B no longer owns has no green. Cube center still +Z red. |

## What we did not do

No views/scissor. No hit-test / drag-resize of a card. No instancing. No
document type. No extra Slang. No `CULL_NONE`. No putting width in the
shader.
