# Drape (image on a DEM)

This is the step after `docs/IMAGE.md`. A DEM already sits in `[-1,1]²` on
XZ. Planar UVs from that grid let `--image` color the heightfield instead
of adding a second, flat quad. Same pass, same views. See `docs/HOST.md`.

---

## The hole

`--image` always spawned an XZ quad. The DEM’s UVs were 0, so the
heightfield could not carry a heatmap or a photo. Two meshes, one
texture, no drape.

## Contract

- **DEM tessellate** writes `u = i/(cols-1)`, `v = j/(rows-1)` (XZ →
  `[0,1]²`). Plot writes `u` along the series and `v` across the ribbon.
  1×1 white albedo is unchanged (vertex color still shows).
- **Drape:** when a DEM mesh and a `wp_tex` both exist, bind the tex on
  that mesh. Vertex RGB is set to white so the image is the color.
  No extra quad.
- **`--image` alone** is a ground under the scene (`docs/GROUND.md`).
- **`--image` with `--dem`:** one heightfield, one albedo. Cube/OBJ are
  not draped (no planar XZ map). Plot keeps its own vertex color.
- Failure of the image still opens the window; the DEM stays.

A tint of the DEM by the image *center* (all UVs 0) is not this step.

## Tests

| Test | Lock |
|---|---|
| `test-renderer-dem` CPU | 2×2 tessellate: UV origin `(0,0)`, far corner `(1,1)`. |
| `test-renderer-image` GPU | 4×4 high DEM, white verts, split PPM: **6076 red / 6076 green**. Same mesh, no tex: DEM green-gray `190 190 52`. Cube still +Z. |

## What we did not do

No per-triangle UV atlas. No OBJ `vt`. No clipmaps. No third pass. No
`CULL_NONE`. No trackball.
