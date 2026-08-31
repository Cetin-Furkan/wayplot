# World XZ grid (a map scale)

This is the step after `docs/FIT.md`. A grid is a producer of a `wp_mesh`,
like a plot or a DEM. Thin quads in XZ, CCW from +Y, same opaque pass.
Not a third `BeginRendering`, not overlay pixel lines. See `docs/HOST.md`.

---

## The hole

Plan is orthographic and the camera frames the document. Nothing on the
ground tells you a unit. A map without a grid is a picture. Fit gave us
an AABB; the grid is that box turned into world lines.

## Contract

- **`wp_grid_from_aabb`:** origin-aligned step in `{1,2,5}×10^n` so
  about `WP_GRID_TARGET` (12) lines span the larger of X and Z. One extra
  step of padding. Plane at `aabb.min.y` minus a fraction of the radius
  (under the data, no z-fight with a DEM at 0). Degenerate box is
  `-EINVAL`.
- **`wp_grid_tessellate`:** each line is an XZ quad (not `LINE_LIST`;
  iGPU line width is 1.0). Same winding as the DEM (CCW from +Y). Minor /
  major (every 10) / axes (`n = 0`) by vertex color. Width is a fraction
  of `step`.
- **`main`:** after the content meshes, build the grid from their union
  AABB, add it as another doc mesh, then fit. Plan still hides the **plot
  slot**, not whatever mesh happens to be index 1. Grid stays on in both
  panes.

A gray fullscreen floor, or lines that do not move when the plan pans,
is not this step.

## Tests

| Test | Lock |
|---|---|
| `test-renderer-grid` CPU | Bad box rejected. Unit cube → origin-aligned step in 1/2/5×10ⁿ. First tri is a raster front from +Y. All verts share Y, `ny = 1`. 26 lines: **35.15 ns/line** (this i7-1165G7). |
| `test-renderer-grid` GPU | Close plan: origin **148 152 171**, **2928** grid / **13456** clear (lattice, not a floor). Diagonal pan half a cell: center **18 41 56**. Skip the draw: clear. Cube + grid, head-on: **85 29 29**. Heap `session` / `present`. |

## What we did not do

No screen-space line width. No keyboard. No colorbar. No trackball. No
third pass. No `CULL_NONE`.
