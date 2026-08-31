# Plan camera (view preset)

This is the step after `docs/DOC.md`. A plan is a `wp_view` camera looking
down −Y at the XZ plane. Same mesh, same opaque pass, same document. Not
a third `BeginRendering`. See `docs/HOST.md`.

---

## The hole

Two panes were the same kind of orbit. The right one was yawed ~72° so
it was not a copied screenshot. A CAD/BIM laptop wants **3D + plan**:
world +Y is height, so a DEM’s XZ grid is a map.

`sync_cam` always set `up = (0,1,0)`. Eye on +Y looking at the origin
makes look-at degenerate (`cross(forward, up) = 0`). Pan used the same
world-up and would no-op. A function that set `eye = (0, dist, 0)` and
then called today’s `capture()` would snap back to a turntable.

## Contract

- **`wp_view_plan`**: `plan = 1`, `yaw = 0`, eye = `center + (0, dist, 0)`,
  `up = (sin(yaw), 0, cos(yaw))` so +Z is the top of the screen at yaw 0.
  Dist is kept (clamped if it was unset). Still perspective.
- **Yaw in plan** rotates the map; the eye stays above the center.
- **Pitch (orbit `dy`)** is ignored in plan. The map stays a map. Tilt
  lives on the 3D pane.
- **Pan** uses `cam.up`, not hardcoded world +Y, so a plan pan moves in
  XZ (height stays). Dolly still changes `dist`.
- **`main`**: left pane stays the default corner view. Right pane is
  `wp_view_plan`. Same doc, same mesh.

A head-on +Z cube is **not** a plan. The plan of the factory cube shows
the **+Y** (cyan) face, not the +Z red face, and not the teal clear.

## Tests

| Test | Lock |
|---|---|
| `test-engine-view` CPU | After `wp_view_plan`, eye is above `center`, `up` is not world +Y and not parallel to the view. Yaw stays in plan. Pan from plan moves XZ, not Y. Pitch orbit **stays in plan** (still ortho, still looking −Y). |
| `test-engine-view` GPU | Split 128×128: left head-on +Z (`85 29 29`), right plan (`67 200 200`, cube +Y). Right is not the clear and not +Z red. Heap `session` / `present`. |

## What we did not do

No ortho projection. No second mesh. No `CULL_NONE`. No trackball. No
third pass. No DEM tiles.
