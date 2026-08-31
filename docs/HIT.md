# Host input + hit stack

This is the step after `docs/CARDS.md`. A card is `x,y,w,h` on the CPU. A
click on that rect is a host event, not a titlebar. See `docs/HOST.md`.

---

## The hole

Pointer messages died in `session.c`. Left click in the top 36 px was
`xdg_toplevel.move`. Left click on a 12 px edge was `resize`. A click on a
card at `y = 56` was discarded. `main` never saw an event. The two rects
were paint.

A hit stack that peeks `session->pointer_x` would keep the same wall
breach. An event struct that nothing consumes is ceremony.

## Contract

- **Session reports.** `wl_pointer` enter/leave/motion/button update a
  snapshot (`x,y`, `inside`, `buttons`, **press/release edges this pump**).
  Session does **not** call `move`/`resize`. Edges are cleared at the start
  of each `wp_session_pump`.
- **`wp_hit_stack`** is a CPU array of `{id, wp_rect}` in **logical**
  surface pixels (same space as the pointer). Last push is on top. `id` 0
  is invalid. Empty/`w<=0` is `-EINVAL`. Cap `WP_HIT_MAX` (64).
- **Pick** is half-open: `x >= r.x && x < r.x+w` (Y the same). No
  triangles. No callbacks.
- **`wp_input_feed`** is pure. On left press, in order:
  1. Window **edge** (12 px, not maximized/fullscreen) → `WP_CHROME_RESIZE`.
  2. Else **pick** → start drag (`drag_id` sticky until release or leave).
  3. Else **move band** (top 36 px, not fullscreen) → `WP_CHROME_MOVE`.
- Drag updates `drag_rect.x/y` from the grab; `w/h` stay. Motion over
  another rect does not re-pick. The release pump still writes the last
  sample, then clears `drag_id`. The caller applies `drag_rect` on that
  pump (keep a local id) so the last pixel is not dropped.
- **`wp_input_handle`** copies the session snapshot, feeds, then *asks*
  session to `move`/`resize` if chrome fired. Cursor: default over a hit or
  drag; edge shapes on a miss.
- `main` rebuilds the stack from the live rects every poll, even when
  `present_begin` is false (otherwise a press during vsync wait is lost).

Pointer serial for chrome is still the button serial session stored.

## Tests

| Test | Lock |
|---|---|
| `test-engine-hit` CPU | Push/copy/cap. Pick A, B, gap. Overlap last-wins. Card in the move band drags (not `MOVE`). 12 px edge wins over a card that covers the corner. Sticky drag when the pointer leaves the original rect. Release ends drag. Fullscreen suppresses chrome. Maximized suppresses resize, not move. |
| `test-engine-hit` GPU | Magenta A + green B. Feed a press-move on B; A unmoved; B's AABB follows; the strip B left is empty of green; cube center still +Z. A few DMA-BUF frames. |
| `test-engine-hit` measure | 64 stacked rects, 200k picks, ns/pick printed. This box: **59.75 ns/pick** (Iris Xe, i7-1165G7). Drag is not the cost. |

A window that “moved when I clicked the cube” with session still calling
`toplevel.move` from `pointer_button` is not this step.

## What we did not do

No views/scissor. No camera orbit. No drag-resize of `w/h`. No keyboard.
No ImGui / callbacks / widget base. No document type. No instancing. No
`CULL_NONE`. No hit testing in `session.c`. No fullscreen pick shader.
