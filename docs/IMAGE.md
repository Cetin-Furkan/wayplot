# Image (albedo that is not 1×1 white)

This is the step after `docs/PLOT.md`. An image is CPU pixels host-copied
to a sampled `VkImage`, then bound per lit draw. UVs on the mesh pick
texels. The 1×1 white albedo stays the default. See `docs/HOST.md`.

---

## The hole

Lit already had a combined sampler. It always sampled `float2(0.5, 0.5)`
of a 1×1 white texel, so vertex color * 1 was the whole story. A photo
or a split heatmap could not show two colors on one quad. Replacing the
1×1 with a bigger image and still sampling the center is a tint, not
this step.

## Contract

- **Format:** NetPBM **P6** (binary RGB). ASCII header, `#` comments.
  8-bit if `maxval < 256`. Side 1..2048. Loaded as tightly packed RGBA8
  (A=255). Missing file `-ENOENT`. Bad magic/size `-EINVAL` / `-E2BIG`.
- **GPU:** `vkCopyMemoryToImage` (host image copy), push descriptor
  binding 1. Default lit albedo remains 1×1 white. `wp_lit_draw` is
  unchanged (uses the default). `wp_lit_draw_tex` binds a `wp_tex`, or
  the default when the pointer is NULL.
- **Mesh:** `wp_vn_vertex` has `u,v`. Existing producers leave them 0
  (1×1 white still looks the same). A textured quad in XZ, CCW from
  +Y, maps `u,v` across `[0,1]`. Vertex color is white so the image
  shows.
- **Document:** optional albedo pointer per mesh slot. Cube/plot stay
  NULL.
- **`main`:** `--image FILE` uploads the PPM. With a DEM it drapes
  (`docs/DRAPE.md`). Without a DEM it is a ground under the scene
  (`docs/GROUND.md`), not a `y = 0` slab through the cube. Failure
  prints; the window still opens.

A GPU test that tints the cube by the image center, with both halves
the same mix, is not this step.

## Tests

| Test | Lock |
|---|---|
| `test-renderer-image` CPU | Missing file. P5 is not P6. 2×1 P6 → RGBA. Quad is 4 verts / 6 indices, raster-front from +Y, UVs span 0..1. |
| `test-renderer-image` GPU | 32×2 split (left red, right green), camera from +Y. **3937 red / 3937 green** pixels. Halves `238 0 0` and `0 238 0`. Cube with default 1×1 still +Z `85 29 29`. Host-copy 256²: **0.05 ms**. Heap `session` / `present`. |

## What we did not do

No PNG/JPEG. No third `BeginRendering`. No bindless. No overlay blit
pipeline. No `CULL_NONE`. No trackball.
