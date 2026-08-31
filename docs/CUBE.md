# Mesh + camera + lit pass (the cube step)

This is the step after `docs/PRESENT.md`. First `vkCmdDrawIndexed` of an **uploaded** mesh. Not a fullscreen fragment shader, not `SV_VertexID`, not a spinning-cube object wired into the renderer.

The window that looked like a **dart**, then an **inside-out cube** (faces widening toward the silhouette), was this step with `FRONT_FACE_CLOCKWISE`. Vulkan's facing area is `a = -shoelace` in Y-down framebuffer, so world-CCW + Y-flip is **COUNTER_CLOCKWISE** on screen. CLOCKWISE draws the insides. The old client then “fixed” the same class of bug with `CULL_NONE`.

Stay here until a cube is a cube. Do not start text.

---

## Walls (so the next mesh is not another special case)

| File | Job |
|---|---|
| `src/helper/math3d.c` | Column-major mat4. `wp_mat4_mul_vec4` is `c0*x+c1*y+c2*z+c3*w`. NDC area + `wp_triangle_front_facing`. |
| `src/renderer/camera.c` | Look-at + Vulkan Y-flip projection. Owns front-face: `wp_camera_front_clockwise()`. |
| `src/renderer/mesh.c` | CPU cube factory (`wp_cube_cpu`) + GPU upload. Winding is CCW from the outside. Not a spinning object. |
| `src/renderer/lit.c` | Lit-mesh pipeline. Any `wp_mesh` with `wp_vn_vertex`. Cull BACK. Front face from the camera. Bind + `DrawIndexed` inside `wp_pass_opaque_begin` / `end`. Depth is on the pass, not here. |
| `shaders/lit.slang` | Vertex + fragment. UBO is **four float4 columns**, not `float4x4`. |
| `main.c` / `test/renderer/cube_test.c` | Callers. Rotation lives here, not in mesh/lit. |

Present still does not know cameras. Lit does not include Wayland headers. `present_begin` leaves `cmd` open; the pass begins rendering; lit records draws; `present_end` submits.

---

## Why it was a dart, and the lock

1. **Wrong front face.** `wp_mat4_perspective_vk` sets `m[5] = -f` so world +Y is up on screen. World-CCW stays **CCW on screen**. Vulkan's facing area is `a = -½ ∑(x_i y_{i+1} - x_{i+1} y_i)` in Y-down framebuffer — the minus makes positive `a` mean COUNTER_CLOCKWISE on screen. Pipeline is `VK_FRONT_FACE_COUNTER_CLOCKWISE` + `VK_CULL_MODE_BACK_BIT`. Mapping the raw (no-minus) shoelace onto `CLOCKWISE` draws the **insides**: first a dart, then a cube whose faces widen toward the silhouette.

   A triangle is drawn only when the camera is on the outward side of that face — `wp_triangle_front_facing`, and `test-renderer-raster` reads GPU pixels of a head-on +Z cube and requires the center to be the red outward face, not the green inner face.

   Do **not**:
   - `CULL_NONE` / two-sided (hides the bug, lights and later meshes will be wrong)
   - Reverse mesh indices to chase the screenshot
   - Draw front and back always

2. **Transposed clip matrix.** Slang `float4x4` in a std140 UBO has come out **RowMajor** even with `-matrix-layout-column-major` and `column_major`. Uploading CPU column-major (or transposing “to be safe”) then disagrees with `mul(M, v)` and the projection shears a cube into a needle. The UBO is four columns; the shader does `c0*x + c1*y + c2*z + c3*w`, identical to `wp_mat4_mul_vec4`. No matrix decoration to get wrong.

`WP_FRONT_NDC_AREA_SIGN` in `math3d.h` is the single switch. `wp_camera_front_clockwise()` and the pipeline frontFace both read it. Changing Y-flip without that sign, or the sign without Y-flip, is how this becomes a dart again.

---

## Tests that must fail if it regresses

These run on `make test` **before** the GPU cube window. A dart used to pass because `test-renderer-cube` only counted presented frames.

| Test | What it locks |
|---|---|
| `test-math3d` | Column expansion; `m[5]<0`; origin at NDC center; world-CCW faces; +Z NDC area **< 0**; cube NDC AABB is a box, not a needle. |
| `test-renderer-mesh` | For default / head-on / orbit cameras: raster front **equals** “outward normal points at eye”. From outside, 1–3 faces drawn, at least one culled. |
| `test-renderer-spv` | Embedded `lit.vert.spv` has **no** `RowMajor` and **no** `OpTypeMatrix`. |
| `test-renderer-raster` | GPU readback: head-on center is +Z (red), not -Z (green). 3/4 view keeps +Z, culls -Z. |
| `test-renderer-cube` | Same 1–3 front-face CPU check, then 8 DMA-BUF frames. Motion is in the test, not the mesh. |

If you change projection, winding, or the shader UBO layout, these fail on the CPU. Do not skip them.

---

## GPU path (unchanged present contract)

- 24 vertices (unique normals per face), 36 `uint16` indices, 6 face colors
- `vkGetDeviceBufferMemoryRequirements` (maintenance5) before `vkCreateBuffer`
- Dynamic rendering, color + **D32_SFLOAT** depth, one depth image per swapchain slot
- `vkCmdPushDescriptorSet`: binding 0 UBO (8× float4 columns + light), binding 1 combined sampler
- 1×1 white albedo via `vkCopyMemoryToImage`
- SPIR-V `#embed` with `--embed-dir=$(OUT)`, arrays `aligned(4)`, entry name `"main"`

Default camera is a corner view `(1.6, 1.2, 2.0)` so the first frame shows three faces, not a head-on square.

---

## What we did not do

No text. No cards. No fullscreen “cube in a fragment shader.” No descriptor pool. No `shaderDrawParameters`. No two-sided raster. No `float4x4` in the UBO. No spinning state inside `wp_mesh` / `wp_lit`.
