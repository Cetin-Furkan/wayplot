# File mesh (Wavefront OBJ)

This is the step after `docs/HIT.md`. The cube factory is a caller. A mesh
is triangles in memory, uploaded through `wp_mesh_upload`. A file is a
producer of those triangles. See `docs/HOST.md`.

---

## The hole

`wp_mesh_cube` was the only 3D producer. Adding a second shape meant
another factory, which is how the cube almost became “the 3D object.”
`wp_mesh_upload` already takes any `wp_vn_vertex` + indices. A file
closes that.

## Contract

- **Format:** Wavefront OBJ, UTF-8. `v` / `vn` / `f`. `vt`, groups,
  materials, lines: ignored. Optional `v x y z r g b` (0–1, or 0–255 if
  any channel > 1).
- **Faces:** 1-based and negative indices. `v`, `v/vt`, `v//vn`,
  `v/vt/vn`. n-gons fan-triangulated (`0,i,i+1`). Degenerate triangles
  skipped. n-gon with more than 32 corners is `-E2BIG`.
- **Normals:** if `vn` is indexed, use it (normalized). Else the triangle’s
  own cross product. Do **not** reverse indices.
- **Caps:** file ≤ 32 MiB (`-EFBIG`). Vertices ≤ `WP_MESH_MAX_V` (65536,
  uint16 indices). Indices ≤ `WP_MESH_MAX_I` (1<<20). Line ≤ 4096.
- **Errors:** missing path `-ENOENT`, empty / no faces `-EINVAL`. Library
  returns a negative errno. Process lives. `main --mesh bad.obj` prints
  the error and still opens the window **without** substituting the cube.
- **`wp_obj_parse`** from memory. **`wp_obj_load`** from a regular file
  (`open`/`read`). Both fill a heap `wp_mesh_cpu`. Caller `wp_mesh_upload`
  then `wp_mesh_cpu_free`.
- No `--mesh`: cube factory, as before.

Not glTF, not PBR, not mtllib textures, not uint32 indices.

## Tests

| Test | Lock |
|---|---|
| `test-renderer-obj` CPU | Missing file lives. Empty / no faces / bad index rejected. Triangle 3 verts. Quad fans to 2 tris. Relative `f -4 -3 -2 -1`. `v//vn` used. Cube OBJ: 12 tris, first +Z triangle is raster-front under a head-on camera, `-Z` is back. Parse 10k quads: **~270 ns/tri** (this i7-1165G7, 20k tris, 684 KiB). |
| `test-renderer-obj` GPU | CCW red +Z quad from OBJ: head-on center is +Z red, not clear. Clockwise of the same corners: center is the clear (culled). Cube file uploaded and presented a few DMA-BUF frames. |

A window that still only knows `wp_mesh_cube` is not this step.

## What we did not do

No views. No camera orbit. No DEM. No glTF. No `CULL_NONE`. No second
lit pipeline. No document type. No instancing.
