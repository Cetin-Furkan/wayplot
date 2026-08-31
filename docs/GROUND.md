# Image as ground (not a slab through the cube)

This is the fix after `docs/GRID.md`. `--image` without a DEM is still one
textured mesh in the opaque pass. It is a **mat under the document**, not
a second world at `y = 0`. Drape on a DEM is unchanged. See `docs/IMAGE.md`
and `docs/DRAPE.md`.

---

## The hole

`--image split.ppm` with the factory cube planted a `[-1,1]²` quad at
`y = 0`. That plane cuts the cube in half. Plan of that document was the
split photo; the cube was a caption. A photo that replaces the object is
a demo.

## Contract

- **`wp_image_ground(box)`:** XZ from the box, padded ~12% so it reads as
  a mat. `y = box.min.y` minus a fraction of the radius (under the data,
  no z-fight with the cube bottom). UVs still `[0,1]²`, white verts,
  CCW from +Y, plus the reverse winding (`CULL_BACK`, not `CULL_NONE`).
- **`wp_image_quad`:** unchanged unit factory at `y = 0` for UV tests.
- **`main`:** `--image` + DEM still drapes. `--image` without DEM, after
  the content AABB exists, uses `wp_image_ground`. No content: keep the
  unit quad.

A window whose plan of cube + split is the split at the center, with no
cube +Y, is not this fix.

## Tests

| Test | Lock |
|---|---|
| `test-renderer-image` CPU | Ground under a unit cube: `y` below `min.y`. Covers the cube XZ. Both windings. |
| `test-renderer-image` GPU | Cube + ground (split PPM), plan: **67 200 200** (cube +Y), not red/green. Head-on: **85 29 29** (+Z). Isolated unit quad still shows both halves. Heap `session` / `present`. |

## What we did not do

No PNG/JPEG. No overlay blit. No `CULL_NONE`. No trackball.
