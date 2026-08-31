# Two views (one pass)

This is the step after `docs/PAN.md`. A view is still a scissor inside the
opaque pass. Two of them is two scissors, two cameras, one
`BeginRendering`. Same mesh. See `docs/HOST.md`.

---

## Contract

- Two `wp_view`s split the pane below the cards (left / right).
- Same uploaded mesh, identity model, **two** `wp_lit_draw`s after
  `wp_view_bind` for each. UBO write heads from `wp_lit_reset` then two
  draws so the cameras stay distinct.
- Pointer picks the view that contains it (`hover_i` / sticky `view_i`).
  Orbit, pan, and dolly apply to that view only.
- Both cameras started as orbits (right yawed ~72°). A plan preset on
  the right pane is `docs/PLAN.md`.

## Tests

| Test | Lock |
|---|---|
| `test-engine-view` CPU | Two rects: press on the right pane sets `view_i == 1`. |
| `test-engine-view` GPU | One left-half view: right is clear. Then two panes, both head-on: **both** pane centers are +Z red. |

## What we did not do

No document type. No DEM. No third `BeginRendering`. No instancing.
