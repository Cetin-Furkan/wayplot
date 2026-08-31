# View layers (which meshes a pane draws)

This is the step after `docs/CAPTION.md`. A view is still one scissor and
one camera. It also has a bitmask of document mesh slots. Overlay cards
stay on the whole window. Not a third `BeginRendering`. See `docs/HOST.md`.

---

## The hole

Two panes always drew every mesh. The plot ribbon showed up in the plan
view as a stripe on the map. Hiding it meant deleting the mesh from the
document. CAD/BIM views of one document are **filters**, not copies.

## Contract

- **`wp_view.layers`:** bit `i` = document mesh index `i`. Init is all
  bits on (`~0u`). Bit 32 and above are off.
- **`wp_view_mesh_on(v, i)`** is the draw test. The opaque loop skips a
  mesh when the bit is 0. Cards and captions are not masked.
- **`main`:** both panes draw every mesh (the plot is part of the
  document). The bitmask is still the filter: a test that sets `layers
  = 0` on one pane must show the clear.

## Tests

| Test | Lock |
|---|---|
| `test-engine-view` CPU | Init: mesh 0 is on. `layers = 0`: mesh 0 is off. |
| `test-engine-view` GPU | Two panes, both would show +Z. Right `layers = 0`: left **85 29 29** (+Z), right **18 41 56** (clear, not leftover cube). Heap `session` / `present`. |

## What we did not do

No UI toggles. No per-card layers. No third pass. No trackball.
