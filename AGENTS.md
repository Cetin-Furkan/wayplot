# Agent rules for this repo

Read `docs/HOST.md` and `docs/CONTEXT.md` before changing architecture. The code does not contain the decisions. Step contracts live in `docs/` (`DOC.md`, `PLAN.md`, `DEM.md`, `VIEW.md`, …).

## Audience

Mesa iGPU: Intel ANV, AMD RADV. Turnip later. **Do not** special-case NVIDIA. Android is a second present backend later, not `#ifdef` in wayland. NPU is not Vulkan.

## Walls

- `src/wayland` + `src/uring`: compositor client. No cameras, fonts, cards.
- `src/vulkan`: device, memory, upload, pipelines. No Wayland object ids.
- `src/engine`: loop, present_begin/end, input events, host (views/resources/**draws**/document). `wp_draw_list` and `wp_doc` live here. Neither calls `vkCmdBeginRendering`. The list walks into `wp_pass`. The doc holds card rects and `wp_mesh *`; it does not own GPU memory.
- `src/renderer`: cameras, meshes, **pass** (`BeginRendering` lives here), terrain, plots, images, UI tessellation, text. Draws (`wp_lit_draw`, `wp_text_draw`) only bind + `DrawIndexed` inside a pass. Per-draw UBO/VBO ranges so N items in one scope stay distinct. Do not begin rendering from a widget. Plot, DEM, image, and grid are mesh/texture producers, not new renderers. Default lit albedo is 1×1 white; a real image is a `wp_tex` bound per draw.
- `src/helper`: arena, relative pointers, log, fd, time. No present/gpu includes.
- `main.c` stays next to the Makefile.

## Hard refusals (author)

No libwayland, liburing, wayland-scanner, GLFW, poll/epoll as the event loop. io_uring for Wayland **recv and send**. Internal protocol/caps/assets may use relative pointers. **The Unix socket still speaks Wayland wire.** Do not invent a display protocol.

## Vulkan

Require 1.4 and **use** it: push descriptors, host image copy or a transfer queue, maintenance5 image queries. Do not enable unused 1.4 bits as decoration.

Explicit sync: **two** timeline semaphores (GPU acquire vs compositor release), not one counter for both.

Device after dmabuf `main_device`. Pick GPU whose DRM node matches the **tranche you allocate from**.

## Do not

- Nested folders named `wayplot` under this repo.
- Leaf directories for one function (`resize/`, `begin/`, `pick/`).
- Fullscreen fragment shaders as the world or the UI.
- `exit(1)` on bad content (mesh/font). Platform death can be fatal.
- Port the old dish/UI slang as architecture. Cube, then text, then two cards, then a DEM.
- Fullscreen UI shader for letters. Text is glyph quads + atlas. No HarfBuzz in v1.
- `CULL_NONE` / two-sided raster to hide winding. World CCW + camera Y-flip ⇒ `FRONT_FACE_COUNTER_CLOCKWISE` + `CULL_BACK` (Vulkan `a = -shoelace`). `float4x4` in a UBO (use four columns). Spinning-cube state in the renderer; motion stays in the caller.
- Two cards as `wp_rect` data landed (`docs/CARDS.md`) before views and the document. `wp_doc` is `docs/DOC.md`. Do not put `BeginRendering` in the document.

## Make / version

- Default `make` is quiet (`BUILD=normal`). Debug prints exist only under `make debug` / `make asan` (`-DDEBUG`).
- `VERSION` is `major.minor.patch`. `make release` bumps **minor** if `main.c` or `src/**/*.c` changed since `.version-stamp`. `make release update` bumps **major** and zeros minor/patch. `test/` does not count.
- Do not tag or push GitHub from the Makefile unless the author asks.

## Reference

Old working client: parent directory `../` (`vulkan.c`, `wayland.c`, `dmabuf_negotiate.c`, `engine.c`). Copy present math. Do not rewrite DMA-BUF from memory.
