# Card mesh factory (one overlay rect)

This is the step after `docs/LIST.md`. It is **not** two cards as independent
`x,y,w,h` data you resize at runtime. That is the next row. See `docs/HOST.md`.

---

## What a card is

A **card** is a CPU-tessellated rectangle in **pixel space** (Y down), drawn
in the **overlay** pass. Same winding as glyph quads: TL, BL, BR, TR, triangles
`0,1,2` and `0,2,3`, `FRONT_FACE_COUNTER_CLOCKWISE` + `CULL_BACK`.

It is not:

- a fullscreen fragment shader (the old 320×460 / 580×340 panel)
- a lit 3D mesh (no depth, no camera, no albedo)
- two widgets with live width (next step)

The factory is `wp_card_cpu(x, y, w, h, &geom)` — four verts, six indices,
AABB. No heap. `w <= 0` or `h <= 0` is `-EINVAL`.

The GPU side is `wp_card`: overlay pipeline, solid color, blend, no depth,
per-draw UBO/VBO like text. `wp_draw_list_push_card` records it. Overlay
order: **cards then text** so a label can sit on a panel.

---

## Tests

| Test | Lock |
|---|---|
| `test-renderer-card` CPU | Unit quad. Pixel AABB. TL-BL-BR is front under `wp_mat4_ortho_pixel`. Clockwise TL-TR-BR is back. Zero/negative size rejected. List accepts a card item. |
| `test-renderer-card` GPU | Heap `session`/`present`. One card at `(16,24)` size `40×30` plus the cube, through the list. Card AABB filled with the factory color; cube center still +Z red (not a fullscreen panel); opposite corner empty. DMA-BUF frames. |

Two cards, or “change `w` on the CPU and the GPU follows,” is `docs/CARDS.md`.

---

## What we did not do

No second card. No live resize of a panel as the test. No instancing. No
document type. No `CULL_NONE`. No putting the rect size in Slang.
