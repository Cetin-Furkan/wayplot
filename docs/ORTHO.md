# Orthographic plan (a map, not a 3D view from above)

This is the step after `docs/LAYERS.md`. A plan is still one scissor and
one camera looking −Y. Its projection is now a parallel box, not a 60°
frustum. Same mesh, same opaque pass. Not a third `BeginRendering`.
See `docs/HOST.md` and `docs/PLAN.md`.

---

## The hole

`wp_view_plan` set the eye above the center and hid the plot, but
`wp_camera_proj` was still `wp_mat4_perspective_vk`. Parallel world
edges met. A wall had screen area. That is a 3D view from the ceiling,
not a CAD/BIM plan.

## Contract

- **`wp_mat4_ortho_vk(half_h, aspect, znear, zfar)`:** view-space box,
  Y-flipped (`m[5] < 0`) like the perspective matrix, Vulkan 0–1 depth
  (near → `ndc.z = 0`). `w = 1` (no perspective divide from z).
- **`wp_camera.ortho`:** when set, `wp_camera_proj` uses that matrix.
  Half-height is `distance(eye, center) * tan(fovy/2)` so a plan **dolly**
  is a map scale change, and leaving plan keeps the look-at size.
- **`wp_view_plan` / `sync_cam`:** `plan` ⇒ `cam.ortho = 1`. Yaw stays
  ortho. Pitch does not leave plan.
- **3D pane** stays perspective. Overlay pixel ortho is unchanged.

A plan that still magnifies the near face of the cube (more +Y pixels
than the same camera with `ortho` forced off) is not this step.

## Tests

| Test | Lock |
|---|---|
| `test-math3d` CPU | Ortho Y-flips. Same view-x at two depths share NDC x. Near `ndc.z = 0`, far `= 1`. |
| `test-engine-view` CPU | Plan sets `cam.ortho`. Same world XZ, different Y → same NDC xy. Pitch leave clears ortho. `wp_camera_proj` ortho: **31.51 ns/call** (this i7-1165G7). |
| `test-engine-view` GPU | Plan of the cube is still +Y (`67 200 200`). Full-pane +Y pixels: ortho **1600**, same eye perspective **2304**. Heap `session` / `present`. |

## What we did not do

No grid. No fit-to-mesh. No infinite-far. No trackball. No third pass.
