# Plot (1D series → mesh)

This is the step after `docs/PLAN.md`. A plot is a producer of a `wp_mesh`,
like a DEM. CPU floats on the document; GPU shades the ribbon. Same
opaque pass, same two views. See `docs/HOST.md`.

---

## The hole

Plan + 3D existed. The cards were empty color. There was no series the
views could sample. Changing a value meant editing `main` or waiting on
a texture. A plot is not a photo and not a third `BeginRendering`.

## Contract

- **`wp_plot`:** heap `float y[n]`, `n` in 2..4096. ASCII load: one
  number per token, `#` comments. Missing file `-ENOENT`. Bad text
  `-EINVAL`. Process lives.
- **Tessellate:** 2-row ribbon in XZ, height on +Y (a 1D DEM). `x ∈
  [-1,1]`, `z ∈ ±0.5`, `y = sample * amp` (default `0.5`). Shared verts,
  two tris per cell CCW from above, plus the reverse winding so the
  underside is a front from −Y (`CULL_BACK`, not `CULL_NONE`). Normals
  `n = normalize(-dh/dx, 1, 0)`. Vertex color is blue-cyan from the
  sample, not cube red, not DEM green-gray.
- **Document:** `wp_doc` holds `wp_plot *` the same way it holds
  `wp_mesh *`. It does not free the series or the GPU mesh. Change a
  sample on the plot, retessellate, re-upload; pixels follow.
- **`main`:** a sine series is a second mesh on the doc (cube / OBJ /
  DEM stay). `--plot FILE` replaces that series. Load failure prints;
  the window still opens.

Not a line-list pipeline, not an overlay sparkline, not a texture.

## Tests

| Test | Lock |
|---|---|
| `test-renderer-plot` CPU | Missing file. `n=2` → 4 verts / 12 indices (both sides). Top tri front from +Y; underside front from −Y. |
| `test-renderer-plot` GPU | From +Y: low `43 111 209`, high `43 202 209`. From −Y: **43 202 209** (same ribbon, not the clear). Heap `session` / `present`. |

## What we did not do

No trackball. No albedo that isn’t 1×1 white. No GEOTiff / tiles. No
third pass. No `CULL_NONE`.
