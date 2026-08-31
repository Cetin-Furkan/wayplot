# Frame the document (AABB + fit)

This is the step after `docs/ORTHO.md`. A view still has a camera and a
layer mask. After load it looks at the **document**, not at the origin
with `zfar = 20`. Same pass, same meshes. See `docs/HOST.md`.

---

## The hole

Every producer sat in about `[-1,1]`. The camera was built for a unit
cube: look-at origin, `dist` ~3, `zfar` 20, dolly capped at 18. A real
`--mesh` off the origin, or bigger than a few units, is the clear. CAD
opens a file and **frames it**. We opened a file and stared at (0,0,0).

## Contract

- **`wp_aabb`:** world min/max. `wp_aabb_from_vn` walks CPU verts.
  Upload copies that box onto `wp_mesh.aabb`. Union is a CPU fold.
- **`wp_view_fit(v, box)`:** keeps yaw, pitch, and `plan`. Sets `center`
  to the box center, `dist` so a bounding sphere of the box (plus pad
  `WP_VIEW_FIT_PAD`) fits the pane aspect, `znear`/`zfar` from that
  sphere, and `dist_min`/`dist_max` so a later dolly cannot slam back
  to the unit-cube cap of 18.
- **`main`:** after the doc meshes are up, union their AABBs and fit
  both panes (3D keeps the corner orbit, plan stays plan+ortho).

A window that still looks at the origin after loading a cube sitting at
`x = 4` is not this step.

## Tests

| Test | Lock |
|---|---|
| `test-renderer-mesh` CPU | Unit cube AABB is `[-0.5,0.5]³`. Empty verts rejected. 128²: **3.44 ns/vert** (this i7-1165G7). |
| `test-engine-view` CPU | Fit of an offset box moves `center`. Plan+fit stays plan and ortho. A 100-unit box sets `dist` and `zfar` **above 18**. |
| `test-engine-view` GPU | Head-on, cube translated +X 4: center **18 41 56** (clear). After fit (yaw 0): **85 29 29** (+Z). Heap `session` / `present`. |

## What we did not do

No grid. No keyboard Home. No frustum cull. No trackball. No third pass.
