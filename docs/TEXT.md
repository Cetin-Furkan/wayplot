# Text (glyph quads + atlas)

This is the step after `docs/CUBE.md`. Real letters: CPU-rasterized glyphs, an atlas, indexed quads. Not a fullscreen UI shader, not HarfBuzz, not a “hello world” object wired into the renderer.

Resize / drag / tile / maximize stays the **next** present step. `present_begin` already rebuilds DMA-BUF images when xdg size changes. What resize still needs (integer scale, io_uring on the new buffers, layout of cards) is compositor work. Doing it now would only stretch a cube. Text first so later layout has something to place.

---

## Walls

| File | Job |
|---|---|
| `src/renderer/font.c` | Load a TTF (FreeType). Rasterize a coverage set. Pack an R8 atlas. Metrics. Host-copy the atlas when a device exists. |
| `src/renderer/text.c` | CPU layout (UTF-8 → quads). Overlay pass: blend, no depth, `LOAD` after the 3D pass. |
| `shaders/text.slang` | Pixel-space vertex → Vulkan NDC. Atlas red = coverage. No `float4x4`. |
| `helper/math3d.c` | `wp_mat4_ortho_pixel` for tests (same map the shader uses). |
| `main.c` | Caller. Lays out a version string. The pass does not know the string. |

Renderer does not include Wayland headers. Bad font path → negative errno, process lives.

---

## Contract

**Pixel space, Y down, (0,0) = top-left of the framebuffer.** The shader is

```text
clip.xy = 2 * pixel / viewport - 1
```

No extra Y-flip (the 3D projection already flipped world Y). A quad `TL, BL, BR, TR` with triangles `0,1,2` and `0,2,3` is a front face under `FRONT_FACE_COUNTER_CLOCKWISE` + `CULL_BACK` (same pairing as the cube; Vulkan `a = -shoelace`). Do not `CULL_NONE`.

**Font** is a resource: atlas + per-codepoint metrics. Not a label.

**Layout** is CPU: UTF-8, origin = top-left of the first line, `\n` starts a new line. Space/tab advance without a quad. Missing codepoints use `?`. Simple FreeType kerning when the face has a kern table. No HarfBuzz, no bidi.

**Draw** is bind + push descriptors + `vkCmdDrawIndexed` **inside** `wp_pass_overlay_begin` / `end` (color **load**, no depth). Text does not call `BeginRendering`. Isolated glyph tests still open an empty opaque pass so the clear lives on `wp_pass`, not a `load_clear` flag.

Host-visible vertex/index buffers, one pair per swapchain slot. Atlas is static (`vkCopyMemoryToImage`).

Coverage for v1: U+0020..U+007E. Enough for labels, axes, file names. More planes later, same atlas builder.

Default face search (first that opens):

- `/usr/share/fonts/TTF/DejaVuSans.ttf`
- `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`
- `/usr/share/fonts/noto/NotoSans-Regular.ttf`
- `/usr/share/fonts/liberation/LiberationSans-Regular.ttf`

---

## Tests

| Test | What it locks |
|---|---|
| `test-renderer-font` | Open default TTF. `'W'` advances more than `'i'`. `"Hello"` is 5 quads. `""` is 0. Newline grows height. Missing glyph does not abort. Quad is `wp_triangle_front_facing` under `wp_mat4_ortho_pixel`. |
| `test-renderer-text` | GPU: overlay `"H"` on a known clear. Readback: coverage inside the layout AABB, empty far from it. Then a few DMA-BUF frames with cube+text. |

A window that “presented 8 frames” with no letters still fails the readback.

---

## What we did not do

No cards. No HarfBuzz. No fontconfig. No resize/scale/drag. No `CULL_NONE`. No SDF in a fullscreen fragment shader. No spinning “text object” inside the pass.
