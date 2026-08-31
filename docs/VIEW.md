# One view (scissor + camera)

This is the step after `docs/MESH.md`. A view is a rectangle on the window
plus a camera. It is a scissor **inside** the opaque pass, not a third
`BeginRendering`. The view owns the camera. Pointer miss on cards (and not
window chrome) orbits that camera. The mesh no longer spins because a timer
said so. See `docs/HOST.md`.

---

## The hole

`main` rotated a model matrix every frame. The camera was a default corner
view that nothing owned. Clicking the cube area did nothing. A second pane
would have been another `BeginRendering`. Cards were objects; the 3D was
still a screensaver.

## Contract

- **`wp_view`**: logical `wp_rect` (Y down) + orbit (`yaw`, `pitch`, `dist`,
  `center`) that writes `wp_camera`. Default camera is captured into orbit
  params so the first frame still shows three faces.
- **Bind:** after `opaque_begin` (full-FB clear), `wp_view_bind` sets
  **viewport and scissor** to the pane in buffer pixels (`logical × scale`).
  Overlay begin restores the full window. Aspect for lit draws is the pane,
  not the swapchain.
- **Input order** on left press (unchanged walls, one new miss):
  1. 12 px window edge → resize
  2. hit stack (cards) → drag
  3. top 36 px → `xdg_toplevel.move`
  4. else inside the view rect → orbit (sticky until release/leave)
- Orbit: `yaw += dx * 0.005`, `pitch -= dy * 0.005` (Y-down: drag up looks
  up). Pitch clamped to ~±89°. Center and distance stay. Model matrix is
  identity.
- The view is **not** a hit-stack item. Putting a full-window rect on the
  stack would steal the move band.

`main` places the pane below the two cards (`y = 152`). Resize updates `w,h`
from the logical size.

## Tests

| Test | Lock |
|---|---|
| `test-engine-view` CPU | Init matches the default camera at the origin. Orbit dx changes yaw, not center/dist. Pitch clamp. Pixels: scale 2 of `{10,20,30,40}` is `{20,40,60,80}`. Empty rect rejected. Feed: press in the pane orbits; press in the 36 px band still `MOVE`; press on a card still drags; orbit is sticky across another rect. |
| `test-engine-view` GPU | View is the left half of 128×128. Head-on cube: left-pane center is +Z red; right-half center is the clear. Not a fullscreen projection cropped. A few DMA-BUF frames. |
| measure | 100k `wp_view_orbit(1,0)`: **~15–35 ns/call** (this i7-1165G7). |

A window that still `wp_mat4_rotate`s from `clock_gettime` is not this step.

## What we did not do

No second view. No pan/dolly/scroll. No document type. No instancing. No
DEM. No `CULL_NONE`. No third `BeginRendering`. No putting the view on the
hit stack.
