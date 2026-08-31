# Heightmap / DEM (one grid)

This is the step after `docs/VIEWS.md`. A DEM is a producer of a `wp_mesh`,
not a new renderer. Grid in XZ, height on +Y, CCW from above. See
`docs/HOST.md`.

---

## Contract

- **Format:** NetPBM **P5** (binary gray). ASCII header, then samples.
  8-bit if `maxval < 256`, 16-bit big-endian otherwise. Comments `#`
  allowed. Side 2..256. Samples mapped to height `∈ [0,1]`.
- **Tessellate:** shared verts (`cols*rows`), two tris per cell. Spacing
  so the grid sits in `[-1,1]²` on XZ. Amplitude default `0.5`. Vertex
  normals from finite difference `n = normalize(-dh/dx, 1, -dh/dz)`.
  Vertex color follows height (green-gray, not cube red).
- **Errors:** missing file `-ENOENT`, bad magic/size `-EINVAL` / `-E2BIG`.
  Process lives. `--dem FILE` like `--mesh`: on failure, print and open
  the window without substituting the cube.
- Not a clipmap, not a compute shader, not a fullscreen height field.

`--dem` wins over `--mesh` over the cube factory (one lit mesh).

## Tests

| Test | Lock |
|---|---|
| `test-renderer-dem` CPU | Missing file. Bad magic. 2×2 P5 → 4 verts / 6 indices. First tri is raster-front from +Y. 128² tessellate: **22.96 ns/vert** (this i7-1165G7). |
| `test-renderer-dem` GPU | Ramp DEM, default-ish camera: center is not the clear and not cube +Z red. A few DMA-BUF frames. |

## What we did not do

No tiles / clipmaps. No GEOTiff. No second mesh in the same view. No
document type. No `CULL_NONE`.
