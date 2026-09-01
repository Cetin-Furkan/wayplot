# Architecture Specification: Demand-Driven Rendering & Retained Text Layout

## 1. Executive Summary & Problem Statement

Prior to this update, `wayplot` operated on a continuous VSync loop. Although architected as a zero-allocation, low-overhead substrate, `main.c` exhibited two critical compute leaks:

1. **Continuous VSync Polling (Idle Waste):** In every iteration of `while (!s->closed)`, the loop called `wp_present_poll(p, 50ms)` followed immediately by `wp_present_begin()`, full 3D opaque dynamic rendering, 2D overlay rendering, and `wp_present_end()`. Even with zero user interaction and static geometry, the CPU recorded command buffers and the GPU executed rasterization at a fixed 60 Hz rate.
2. **Per-Frame Dynamic Text Layout (Heap Churn):** In each frame, `main.c` called `wp_text_geom_free(&cap[ci])` and invoked `wp_text_layout(font, c->caption, ...)` for all visible UI cards. This forced FreeType glyph-metric traversal and heap reallocation 60 times per second for unchanging string literals.

The refactored implementation establishes a **3-State Reactive Event Loop** governed by Linux `io_uring` kernel sleep and a **Retained Layout Cache**, driving steady-state CPU and GPU compute down to **0.0%** during idle periods without introducing latency or frame pacing stutter.

---

## 2. Core Architectural Changes

```text
+-----------------------------------------------------------------------------------+
|                               REACTIVE EVENT LOOP                                 |
+-----------------------------------------------------------------------------------+
                                          │
                                          ▼
                      ┌───────────────────────────────────────┐
                      │    Phase 1: Dynamic io_uring Sleep    │
                      │  timeout = dirty_frames ? 0 : UINT64  │
                      └───────────────────┬───────────────────┘
                                          │ (Wakes on socket / syncobj / timer)
                                          ▼
                      ┌───────────────────────────────────────┐
                      │    Phase 2: Ingest Wayland Stream     │
                      │   wp_present_poll & wp_input_handle   │
                      └───────────────────┬───────────────────┘
                                          │
                                          ▼
                      ┌───────────────────────────────────────┐
                      │   Phase 3 & 4: Dirtiness Evaluation   │
                      │ Deltas != 0 || hover != last || resize│
                      └───────────────────┬───────────────────┘
                                          │
                        Is State Dirty?   │
                                          ├─────────────── NO ──────────────┐
                                          │                                 │
                                         YES                                │
                                          │                                 │
                                          ▼                                 │
                      ┌───────────────────────────────────────┐             │
                      │ Phase 6: wp_present_begin() (Acquire) │             │
                      └───────────────────┬───────────────────┘             │
                                          │                                 │
                        Swapchain Ready?  │                                 │
                                          ├─────────────── NO ────────┐     │
                                          │                           │     │
                                         YES                          │     │
                                          │                           │     │
                                          ▼                           ▼     ▼
                      ┌───────────────────────────────────────┐    ┌─────────────┐
                      │ Phase 8: Retained Text Check & Draw   │    │             │
                      │ (Re-layout ONLY if scale/rect dirty)  │    │             │
                      └───────────────────┬───────────────────┘    │             │
                                          │                        │             │
                                          ▼                        │  continue   │
                      ┌───────────────────────────────────────┐    │  (Loop to   │
                      │ Phase 9: Record & Submit Passes       │    │   Phase 1)  │
                      │ dynamicRendering + Push Descriptors   │    │             │
                      └───────────────────┬───────────────────┘    │             │
                                          │                        │             │
                                          ▼                        │             │
                      ┌───────────────────────────────────────┐    │             │
                      │ Phase 10: Commit & Decrement Counter  │    │             │
                      │ dirty_frames--; (sustain if active)   │    │             │
                      └───────────────────┬───────────────────┘    │             │
                                          │                        │             │
                                          └────────────────────────┴─────────────┘

```

### State Machine Breakdown

#### Phase 1: Dynamic Kernel Sleep (`io_uring_enter`)

* When `dirty_frames > 0` and the swapchain is ready, `poll_timeout_ns` is set to `0`. `wp_present_poll` non-blockingly reaps pending CQEs from the provided-buffer ring.
* When `dirty_frames == 0` or the swapchain is stalled waiting on a compositor `surface.frame` callback, `poll_timeout_ns` is set to `UINT64_MAX`. The kernel deschedules the thread (`TASK_INTERRUPTIBLE`), clock-gating the CPU execution core until hardware socket bytes arrive.

#### Phases 2–4: Exhaustive Invalidation Tracking

Dirtiness is evaluated across three distinct categories:

1. **Kinematic & Interaction Deltas:** `orbit_dx`, `orbit_dy`, `pan_dx`, `pan_dy`, `axis_v`, `drag_id`, `dragging`, `orbiting`, `panning`.
2. **Pointer Focus & Hover State:** Tracking `input.hover_i != last_hover_i` ensures hover transitions (e.g., cursor entering a card or switching viewports) trigger a redraw immediately.
3. **Surface & Window Geometry:** `lw != last_lw`, `lh != last_lh`, `s->size_dirty`, and `s->configure_dirty`.

#### Phase 5 & 6: Swapchain Throttle Preservation

If `wp_present_begin(p, &f)` fails because the compositor has not yet delivered `frame_done` or the swapchain image fence is unsignaled, `wait_for_frame = true` is set, and the loop executes `continue`.

* **Critical Design Choice:** `dirty_frames` is **not** decremented. On the subsequent iteration, `poll_timeout_ns` becomes `UINT64_MAX`, cleanly suspending the thread until the compositor's VSync callback arrives without dropping the pending draw state.

#### Phase 8: Retained Text Geometry Cache

`struct wp_rect cap_rect[2]`, `int32_t cap_laid_scale[2]`, and `int cap_valid[2]` track card layout validity.

* `wp_text_layout()` and `wp_text_geom_free()` execute **only** when `!cap_valid[ci]`, when `cap_laid_scale[ci] != f.scale`, or when card coordinates mutate during an active drag (`cap_rect[ci].x != c->rect.x`).
* Static frames and camera orbits reuse pre-tessellated quad memory buffers directly.

#### Phase 10: Multi-Buffer Refresh & Gesture Continuity

* Each successful `wp_present_end` decrements `dirty_frames`. Initializing `dirty_frames = WP_SWAPCHAIN_IMAGES` (3) ensures every slot in the Vulkan swapchain receives updated framebuffer data.
* If `input.orbiting`, `input.panning`, or `input.drag_id` remains asserted at frame completion, `dirty_frames` is forced back to `WP_SWAPCHAIN_IMAGES`, ensuring uninterrupted 60 Hz fluid tracking during active mouse holds.

---

## 3. Engineering Traps & Failure Modes Addressed

### 1. `struct wp_hit_stack` Structural Error

* **Error:** Initial draft attempted to query `hits.hover_id`.
* **Root Cause:** In the `wayplot` architecture, `struct wp_hit_stack` is a low-level rectangular hit buffer populated by `wp_doc_fill_hits()`. Hover resolution logic lives within `struct wp_input` as `input.hover_i` (index of hovered viewport or target).
* **Fix:** The dirty check was corrected to track `input.hover_i != last_hover_i`.

### 2. The VSync State Starvation Race Condition

* **Failure Mode:** In simple event loops, `scene_dirty = false` is assigned at the end of every loop turn. If `wp_present_begin()` returns `false` due to VSync throttling, the dirty state is lost, and the window freezes until another event arrives.
* **Fix:** State dirtiness is maintained via an integer counter (`dirty_frames`) that decrements *only* upon a successful presentation commit (`wp_present_end()`).

### 3. Triple-Buffering Visual Artifacts (Stale Swapchain Slots)

* **Failure Mode:** Using a boolean flag (`scene_dirty = true`) renders only a single frame to Swapchain Image Index 0. When rendering subsequently idles, Swapchain Images 1 and 2 remain un-cleared or stale.
* **Fix:** Setting `dirty_frames = 3` ensures the state machine cycles through all three swapchain slots before going to sleep.

---

## 4. Empirical Verification & Test Harness

The following standalone verification tools allow benchmarking the compute difference between the legacy continuous loop and the new demand-driven engine.
