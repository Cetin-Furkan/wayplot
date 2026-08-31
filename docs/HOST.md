# Host architecture: document, views, draws

This is the report of what we decided after the cube, text, resize, and cursor
steps. It is not a transcript. It is the why, so a later session does not
rebuild the old two-shader client under a new name.

If this file and `AGENTS.md` disagree, this file is the why; `AGENTS.md` is the
short must-follow list. Step logs (`PRESENT.md`, `CUBE.md`, `TEXT.md`,
`RESIZE.md`, `PASSES.md`, `LIST.md`, `CARD.md`, `CARDS.md`) are what landed in code.

**This file’s coding step is two cards as data** (`docs/CARDS.md`). Views,
instancing, a hit stack, and a document type wait until that step is green.

---

## 0. What we were actually talking about

The window looks like “a spinning cube and a version string.” That is a
**caller**, not the product. The questions on the table were:

1. Is the next object another hardcoded panel in Slang, or a host that can
   grow (two cards, a DEM, particles, a field from an AI model)?
2. Who owns a mesh — CPU or GPU? Does “UI on the GPU” mean a fullscreen
   fragment shader, or rasterizing triangles the CPU laid out?
3. Camera, light, and object transforms change every frame. Where does that
   live so we are not editing a shader to move a card?
4. Vulkan 1.4 has bindless, device-generated commands, indirect, descriptor
   buffers. Which of those is the v1 draw list, and which is fashion?
5. Can we skip to a CAD/BIM viewport, or is there a hole in the current
   frame that would make that list fake?

The short answers, expanded below:

- The product is a **document + views + GPU visualization**, like CAD/BIM,
  sized for a Mesa iGPU laptop, not Blender-in-a-year and not a nicer petri
  dish.
- A mesh is triangles in memory. **CPU owns which triangles exist and where
  they sit.** GPU shades them. That *is* “UI on the GPU.” A fullscreen
  fragment shader painting a panel is the thing we already refused.
- Transforms, particle positions, card `x,y,w,h` are **host-visible buffers**.
  On an iGPU that memory is system RAM. No PCIe upload as the default path.
- v1 draw list is **immediate recording** of a CPU array into two render
  scopes, using push descriptors we already have. Bindless / DGC / indirect
  are later or never for v1.
- **Pass folding comes first.** A list on top of “every widget begins
  rendering” is N engines. That is the old architecture with an array around
  it.

---

## 1. What the product is

Not “a nicer spinning cube.” Not “Blender in a year.” A **live scientific
viewport** on Mesa iGPUs (Intel ANV, AMD RADV):

- A **document** you can change (meshes, later particles, fields from an AI
  physics model, CAD/BIM elements, annotations).
- **Views** of that document (a rectangle on the window + a camera + which
  layers are on). Several views on one swapchain is how CAD/BIM feel usable
  on a laptop: plan + 3D + a plot, without extra OS windows.
- A **GPU visualization** of the current view. The GPU does not *own* the
  building.

Blender, BIM, and CAD are document programs with a viewport. The old wayplot
inverted that: one Slang file *was* the model. Panel size lived in C and again
in the shader (`320×460` vs `580×340`). Resize meant fighting both. Lighting
and “cards” were uniforms and `if`s, not objects you could add or move.

“More modern” here is not a fancier material graph. It is: the viewport is
live, the document is structured, an office iGPU can hold a working set in
**shared RAM**, and changing a width or a particle direction is a memory write
plus a draw — not a shader edit.

### What it is not (so we do not drift)

| Tempting next thing | Why it is the wrong product |
|---|---|
| Fullscreen Slang world / petri dish | The old client. Cannot add a second card, a mesh file, or a DEM. |
| Immediate-mode GUI toolkit (ImGui-style) | A widget library is not a scientific document. We need views of data. |
| Game engine / ECS / scene graph | Too much ceremony before mass. A CPU array of draw items is enough. |
| Bindless mega-shader indexing a giant SSBO | “One shader is the world” again. Same hole, newer API. |
| NVIDIA / discrete-GPU first | Author audience is office iGPUs. Do not special-case NVIDIA. |

---

## 2. Mesh, CPU, GPU (the confusion)

A **mesh** is triangles: positions (and usually normals, UVs, colors) plus
indices. The cube is a mesh. A card will be a mesh (a rectangle). Glyphs are
a mesh (a quad each). None of that means “UI runs on the CPU instead of the
GPU.”

Two jobs, two owners:

- **CPU owns layout:** which triangles exist, where they sit (`x,y,w,h` or a
  model matrix), which ones this frame is worth submitting. One source of
  truth for size. Never a second copy as a Slang constant.
- **GPU owns shading:** it reads those bytes from memory and rasterizes. On
  an iGPU that memory **is** system RAM. Host-visible buffers for data that
  change (transforms, text quads, particle positions). Relatively stable
  heaps for a static mesh or a font atlas (you already host-copied the atlas).

If the GPU “owns the UI” in the old sense — one fullscreen fragment shader
that paints a panel — you cannot freely change width, you cannot add a second
card, and every pixel of empty background still runs the shader. That is busy,
not efficient. Back-face cull already refuses to shade the inside of a cube;
a fullscreen UI shader has no equivalent of “this card is 200×80 in the
corner, skip the rest.”

So: **yes, the GPU draws the UI.** It draws **triangles the CPU placed.** That
is how every CAD viewport, every game, and FreeType-on-Vulkan text work. It
is not a downgrade from “GPU UI.” The downgrade was encoding the UI in Slang.

Camera and light are the same split. The camera is a CPU struct (`eye`,
`center`, `up`, `fovy`). The lit pass consumes it into an MVP UBO per draw.
Motion lives in the caller (`main` currently rotates the cube). When a view
exists, the view owns the camera. The pipeline does not.

---

## 3. What we already have (and what we do not)

Have:

- Present: Wayland + io_uring, DMA-BUF, two timeline roles, `present_poll` /
  `begin` / `end`. `begin` leaves `cmd` open. Present does not know cameras.
- Mesh + camera + lit pipeline. World CCW, Y-flip projection,
  `FRONT_FACE_COUNTER_CLOCKWISE` + `CULL_BACK` (Vulkan `a = -shoelace`). GPU
  pixel tests lock that. Never `CULL_NONE`.
- Text: CPU glyph layout + R8 atlas, overlay draws.
- Resize: `xdg_toplevel.configure` actually in the proto blob; buffer =
  logical × integer scale; GPU-idle retire.
- Pointer: `wp_cursor_shape_manager_v1` on enter (GNOME hides the cursor
  otherwise); 12 px edges resize; top 36 px move.

Have after pass folding + list + card factory + two cards:

- **`wp_pass`** owns `BeginRendering` / `EndRendering` and depth.
- **`wp_draw_list`**: CPU array, immediate `push_*` + `record`. Kinds: lit,
  text, card.
- **`wp_rect`**: source of truth for a card. List copies it; record
  tessellates. Two live rects in `main` are callers.

Do **not** have (and must not fake in the two-cards step):

- A **document** type.
- Views / scissor panes, hit-test drag, instancing.
- File meshes, particles, fields, CAD kernels.
- Bindless, indirect, DGC.

The cube and the version string are **callers**. They must not become the bones.

---

## 4. Ordered steps — do not skip

A draw list on top of “every widget begins rendering” is a fake list: you
cannot share a clear, you cannot share depth, you cannot instance two cards
in one pass, and an iGPU pays for N render scopes.

That is why pass folding is **row 1**, not “we will fold when we have cards.”
Cards inside two competing `BeginRendering` calls are still two engines.

| Order | Step | Why it is next | What it is not |
|---|---|---|---|
| **1 (done)** | **Pass folding** | The pass owns `BeginRendering` / `EndRendering`. Draws only bind and `DrawIndexed`. Depth lives here, not in the cube pipeline. | Not a scene graph. Not cards. Not a list. |
| **2 (done)** | **CPU draw list** | An array of items recorded in those two passes. Change an item on the CPU, the GPU follows. | Not bindless, not indirect, not ECS. |
| **3 (done)** | **Card mesh factory** | Pixel-space rect, same winding as text, overlay pass, one item on the list. | Not a fullscreen card shader. Not two live cards. |
| **4 (now)** | **Two cards as data** | `x,y,w,h` you can change at runtime; GPU follows. One source of truth. | Not hardcoded 320×460 in C and again in Slang. |
| Later | Instancing if both cards share the mesh | Optimization inside a pass. `instanceCount = N`. | Not a second architecture. |
| Later | Hit stack, file mesh, DEM, particles, fields | Producers of document data + draw items. | Not special renderers. |

Pass folding, the list, and the card factory are **done**. **This file’s
current coding step is row 4** (`docs/CARDS.md`). After that: instancing
if it is measured, then hit stack / file mesh / DEM — not a second
architecture.

Why not merge 1+2: a list without a pass still has every item opening a
render scope. You would write the list twice. Why not merge 2+3: a card
factory with nowhere to record it becomes another special-case draw in
`main`, which is how the cube almost became “the 3D object.”

---

## 5. Two passes (the only split we need for a long time)

1. **Opaque 3D** — one begin, clear color + depth, depth test on, back-face
   cull, all lit meshes.
2. **Overlay** — one begin, **load** color, no depth attachment (or test
   off), blend on, text and later 2D cards.

Why not one pass: text (and cards) must not write depth and must blend on
top of the cube. Putting blend+depth in one pipeline for everything is how
you get z-fighting overlays or translucent cubes. Two scopes is the CAD
default (solid, then annotations).

Why not three (or N): a “view” is a scissor **inside** a pass, not a third
`BeginRendering`. A card as a sub-rectangle of the window sets a smaller
scissor, draws, restores. Opening a render scope per widget is the hole we
are closing.

`present_begin` already transitioned the scanout image to
`COLOR_ATTACHMENT_OPTIMAL`. The pass does not talk to Wayland. Present still
does not know cameras.

Viewport/scissor for the full framebuffer are set at pass begin. A later view
may set a smaller scissor **inside** the pass.

Clear color lives on the **opaque pass**, not inside the cube pipeline. The
old bug was every object believing it owned the framebuffer. After folding,
`wp_lit_draw` does not clear. If you call it outside `opaque_begin`, you are
recording draws with no render scope (validation failure / nothing on
screen). That is the intended contract, not a missing convenience wrapper.

Depth images (one `D32_SFLOAT` per swapchain slot) belong to the pass.
Resize of the window resizes the pass’s depth, not a hidden field on the
cube. Lit no longer takes width/height at init.

---

## 6. Draw-list options (Vulkan 1.4) — why we pick one later

1.4 does not add a “draw list object.” It makes tools we **already call**
first-class: dynamic rendering, sync2, timelines, **push descriptors**,
maintenance5, host image copy. Bindless, descriptor buffers, and
device-generated commands are extra, not “the 1.4 way.” We refused unused
1.4 bits on the old client (`shaderDrawParameters` enabled, never used).
Do not “use 1.4” by enabling more unused bits.

When step 2 lands, the comparison is:

| Approach | What it is | When | v1 verdict |
|---|---|---|---|
| Immediate recording | Walk a CPU array; `BindPipeline` if changed; `vkCmdPushDescriptorSet`; `DrawIndexed` | Few dozen objects, iGPU, data changes every frame | **Yes — step 2** |
| Instancing | One mesh, mapped instance buffer, `instanceCount = N`, `gl_InstanceIndex` (core; not `shaderDrawParameters`) | Many objects, same pipeline+mesh (two cards, particles, BIM windows) | **Later, inside a pass** |
| Push constants for the 64-byte matrix | No UBO for that draw | Tiny per-draw blob | New pipeline layout; not a reason to delay |
| One SSBO of all objects | Shader indexes a giant array | Looks neat | Becomes “one shader is the world”; avoid |
| Indirect / multi-draw indirect / count | GPU or CPU writes draw structs | Compute culling fills the buffer | No cull compute yet; skip |
| Device-generated commands | GPU builds the command stream | Huge scenes | Extension, not core 1.4; skip |
| Bindless / descriptor indexing | Shader pulls images by index | Thousands of textures | Refused for v1; we have push descriptors |
| Secondary cmd buffers / replay | Record once, execute later | Static world | Fights per-frame UBOs; iGPU likes rerecord |
| Render graph / 64-bit sort keys | Sort thousands of draws | AAA-scale | Two buckets (3D, overlay) is enough |

**v1 list (when we get there):** CPU array + two passes + immediate `vkCmd*`
+ push descriptors. Instance the two cards when they share a quad.

Why immediate recording wins on an iGPU: the working set is small, the data
changes every frame (orbit, particles, live fields), and the CPU is already
sitting on the same RAM the GPU will read. Recording 20 `DrawIndexed` calls
is cheaper than a compute cull + indirect bootstrap you then have to debug.
When a DEM or a BIM floor has thousands of identical windows, instancing
is an optimization **inside** the same pass, not a new host.

Why not skip to instancing now: we have one cube and one string. Instancing
zero extra objects is ceremony.

---

## 7. iGPU, shared RAM, what not to render

This box is Iris Xe (ANV) / later RADV. There is no discrete framebuffer
over PCIe as the default mental model.

- **Changing state** (particle positions, instance transforms, a card’s
  `w,h`, glyph quads): host-visible, GPU reads in place. No discrete-GPU
  “upload over PCIe” as the default.
- **Stable** (font atlas, a static CAD mesh cache): host-copy once, or
  device-local which on an iGPU is still the same RAM.
- **Back-face cull** is “do not shade the inside of a cube.” Already locked
  by tests. That is free performance and correct lighting.
- **Do not draw what the camera cannot see:** CPU skips the draw (frustum /
  “this tile is off-screen”). A fullscreen shader cannot skip; it runs for
  every pixel.
- **`damage_buffer`** tells the compositor which pixels changed. It does
  **not** replace a draw list. Both.
- **Do not** two-sided raster, `CULL_NONE`, or encode the scene in Slang to
  “fix” winding.

Memory types: prefer `DEVICE_LOCAL`, fall back to any bit in
`memoryTypeBits`. That is already the device helper. Do not introduce VMA
for v1.

---

## 8. Particles, AI fields, CAD/BIM (later, same host)

These are why the host exists. They are **not** this step. They all plug in
as **producers of document data** that become draw items in the two passes.

- **Particles:** a buffer `{position, velocity, …}` plus a stepper. CPU
  first; compute when measured. Draw as instances. Not a fragment
  integrator and not a fullscreen trail shader.
- **AI physics output:** a field on particles, a grid, or mesh nodes. Run
  the model **outside** the rasterizer; upload a texture or per-vertex
  attribute; viewport samples it. Huge fields stream as **tiles** the
  camera can see (same idea as a DEM). The NPU is not a Vulkan queue; do
  not put it in `wp_device_open`.
- **CAD:** parameters + topology on the CPU; tessellate for the viewport;
  retessellate on zoom. Edges are another list. Do not start a NURBS
  kernel in this step.
- **BIM:** a graph of typed elements; GPU sees instances (“mesh 17,
  transform T”). Isolate floor = filter the list, not `if` in the shader.

Simulation and AI are **producers of document fields**, not special
renderers. If we skip pass folding and the list, every one of those
becomes another `wp_*_draw` that begins rendering. That is how you get
five engines.

---

## 9. What this step (passes) must prove

After pass folding:

- The cube still looks like a cube from the outside (existing raster GPU
  test).
- Glyphs still land in their AABB (existing text GPU test).
- **One** opaque `BeginRendering` then **one** overlay `BeginRendering` per
  frame in `main` / tests, not one per widget.
- `wp_lit_draw` / `wp_text_draw` do not call `BeginRendering` /
  `EndRendering`.
- Depth images belong to the **pass**, not to the cube pipeline. Resize of
  the window resizes the pass’s depth, not a hidden cube object.
- Isolated text still has a **clear**: the opaque pass clears even if no
  lit mesh is drawn, then overlay loads. Text must not grow a private
  `load_clear` flag — that was a second engine.

What this step must **not** do: cards, a draw-item struct, instancing, a
document type, extra Slang, `CULL_NONE`.

---

## 10. Tests (every step)

Pass folding is locked by `test-renderer-pass` (GPU: opaque cube + overlay
`H` in one command buffer with two render scopes) plus the existing
cube/text/raster tests updated to call the pass. Heap-allocate
`session`/`present` in GPU tests that open a live compositor (the raster
stack smash was `sizeof(wp_session)` vs a stale `.o` after session grew).

If someone puts `BeginRendering` back inside `wp_lit_draw`, overlay text or
the cube test breaks, or the new test’s contract (draws between begin/end
only) fails: nested `BeginRendering` is illegal.

The list is locked by `test-engine-draw`: CPU array mechanics, then GPU —
two overlay `"H"` items (white + yellow) so a shared UBO cannot fake two
draws; relayout one glyph on the CPU; old AABB empty, new AABB has ink;
empty list still clears; dropping overlay items drops glyphs. Do not
write that test against `main`’s cube. Cards get their own test when the
card factory exists.

---

## 11. Frame shape after this step

```text
present_poll
present_begin                 /* cmd open, color = COLOR_ATTACHMENT_OPTIMAL */
  wp_draw_list_clear
  wp_draw_list_push_lit(...)
  wp_draw_list_push_text(...)
  wp_draw_list_record(...)    /* opaque begin/walk/end, overlay begin/walk/end */
present_end                   /* submit, attach, commit */
```

`BeginRendering` still lives on `wp_pass`. `main` still *fills* the list.
The next step is a card mesh factory, then two cards as `x,y,w,h` data
pushed as items.

---

## 12. Refusals that stay true

- No libwayland, liburing, scanner, GLFW, `poll` as the wait loop.
- No `CULL_NONE` / two-sided raster to hide winding.
- No fullscreen fragment shader as the world or the UI.
- No unused 1.4 feature bits as decoration.
- No NVIDIA special case. No NPU in device init.
- No nested `wayplot/` directory. No leaf folders for one function.
- `main.c` stays next to the Makefile.
- Do not skip row 1 to “get to cards.”
