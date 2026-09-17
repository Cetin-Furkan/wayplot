# Wayplot

** The reality of this code and project **

Everything you see on this code is a ai slop as of right now, i must said this from the very beginning, i will re write most of the part myself again in the future but for now, there is no human written code anymore. Some of the old versions or closed beta versions were having almost half human half ai slop code but this version is created to push the limits so i can learn new things and get new ideas by looking at it.

This project will have no real life application now, its main purpose to give the real human of this project to ideas and ways to build an actual engine.
Right now, the project has 2 rings, ring A and ring B, each of them is pinned to specific threads except the main thread, rings are small io_uring submission life cycle for the io_uring syscal, ring A is the small one with 4kb sqe and always running, the ring B is the one with big pages 2mb and only actiave when something is triggered it, the rings are working async, they submit their data as the same ring so the main submit is always running

---

## Release: Version 0.3.5.2

> **Caption**: *NVIDIA GTX 1650 hybrid GPU compatibility, Wayland atomic opaque region, and Vulkan Sync 2 fixes.*

Version 0.3.5.2 is a targeted hardware compatibility and display server integration release resolving cross-vendor DMA-BUF tiling mismatches on hybrid Optimus laptops (NVIDIA dGPU + Intel iGPU), eliminating window transparency bleed-through via atomic `wl_surface.set_opaque_region` dispatch, modernizing timeline semaphore queue submission to Vulkan 1.4 Synchronization 2 (`vkQueueSubmit2`), and guaranteeing zero compiler diagnostics across all test suites.

```
                  ┌───────────────────────────────────────────┐
                  │        Khoros 0.3.5.2 Architecture        │
                  │   Pure ISO C23 · Zero Third-Party Libs    │
                  └─────────────────────┬─────────────────────┘
                                        │
        ┌───────────────────────────────┴───────────────────────────────┐
        ▼                                                               ▼
 [ Presentation & Display Server ]                             [ Simulation & Compute ]
  ├── Core 2 Affinity (Lock-Free)                              ├── Core 3 Affinity (Pinned Worker)
  ├── Wayland Wire (Raw Syscalls)                              ├── 60/120 Hz Symplectic Integrator
  │   ├── zwp_linux_dmabuf_v1 Universal LINEAR Fallback        ├── 30-bit Morton Radix LBVH Broadphase
  │   ├── wl_surface.set_opaque_region Atomic Opaque Box       ├── 100% Pairwise SAT Narrowphase Matrix
  │   ├── wp_presentation_time Feedback & VBlank Pacing        │   (Sphere, Plane, Box, Capsule)
  │   ├── 3D Camera Ray-Picking Unprojection                   ├── Dynamic Runtime Body Insertion
  │   ├── Interactive Flight Navigation (WASD/Space/C/R)       └── Pipe Audio Mixer (48 kHz Stereo)
  │   └── Live Sandbox Hotkeys (E/K/G/T/0..4)                      ├── Desktop Auto-Sink Router
  ├── Vulkan 1.4 Sync 2 Pipeline                                   ├── Zero-HDMI Hijack (PipeWire/Pulse)
  │   ├── vkQueueSubmit2 + VkSemaphoreSubmitInfo                   └── Modal Acoustic Resonance Synth
  │   ├── Two-Pass Hi-Z Occlusion Culling                      ├── Supersonic Velocity Limiter (28 m/s)
  │   ├── Near-Plane Depth-Safety Guard                        ├── Rotational Velocity Limiter (20 rad/s)
  │   ├── Sub-Frame BDA Stride Alignment                       └── Anti-Tunneling Floor Safety Net
  │   ├── Direct BDA Multi-Mesh Vertex Filter
  │   ├── Opaque Reversed-Z Ground Grid & UI Shaders
  │   ├── Hardware GPU Timestamps (VK_QUERY_TIMESTAMP)
  │   ├── Live Dynamic Pacing Governor (T Key Cycler)
  │   └── Live Telemetry Stream (FPS, CPU %, GPU HW ms, Contacts)
```

---

### Bug Fixes & Improvements in Version 0.3.5.2

#### 1. NVIDIA Hybrid Optimus DMA-BUF Tiling Compatibility
* **Root Cause Diagnosis**: On hybrid laptops (discrete NVIDIA GPU paired with an integrated Intel display GPU running Wayland), compositor modifier negotiation produces an empty modifier intersection. The previous fallback probed `VK_IMAGE_TILING_OPTIMAL` first, creating NVIDIA block-linear tiled images and exporting them with `DRM_FORMAT_MOD_INVALID`. The Intel compositor imported them as linear or Intel-tiled, resulting in scrambled 16x16 pixel blocks, transparent pixel holes, and checkerboard damage trails.
* **Universal LINEAR Fallback**: In `src/gfx/dmabuf.c`, fallback image export for discrete GPUs and cross-GPU un-negotiated targets now prioritizes `VK_IMAGE_TILING_LINEAR` and assigns `DRM_FORMAT_MOD_LINEAR` ($0$). Row pitches and offsets are queried directly via `vkGetImageSubresourceLayout`. Every compositor and display GPU correctly reads linear memory, entirely eliminating tile scrambling.

#### 2. Wayland Atomic Opaque Surface Region
* **Zero Desktop Bleed-Through**: Wayland compositors treat `DRM_FORMAT_ARGB8888` surfaces as translucent by default unless `wl_surface.set_opaque_region` is explicitly specified. Khoros now encodes `wl_compositor.create_region`, `wl_region.add(0, 0, w, h)`, `wl_surface.set_opaque_region`, and `wl_region.destroy` in a single atomic wire dispatch upon window creation and resize. Compositors disable alpha blending behind the window, preventing terminal and browser text bleed-through.

#### 3. Vulkan 1.4 Core Synchronization 2 Submission
* **Vulkan Specification Contract Compliance**: In `tests/test_gpu_driven.c`, legacy `VkSubmitInfo` with timeline semaphore wait previously omitted `pWaitDstStageMask`, violating VUID-VkSubmitInfo-waitSemaphoreCount-00078 and stalling queue submissions on NVIDIA proprietary drivers.
* **Modernized Sync 2**: Replaced with standard Vulkan 1.4 `VkSubmitInfo2`, `VkCommandBufferSubmitInfo`, and `VkSemaphoreSubmitInfo` with explicit `stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT` via `vkQueueSubmit2`. Resolves the 5-second `vkWaitSemaphores` timeout on NVIDIA.

#### 4. Opaque Shading Composition
* **Solid Ground Grid & UI Panels**: In `shaders/grid.slang`, horizon fog now lerps floor color towards the sky background while guaranteeing solid alpha `1.0` output (`out.color = float4(color, 1.0);`), and rays missing the ground return background color at depth $0.0$. In `shaders/card.slang`, solid UI cards write solid alpha `1.0`, eliminating hollow transparent centers.

#### 5. Zero-Warning Standards Compliance
* **Compiler Cleanliness**: Fixed integer signedness comparison warnings (`-Wsign-compare`) in `tests/test_audio.c`, `tests/test_dmabuf_present.c`, and `tests/test_vulkan_bda.c`. All 203 tests compile with zero warnings under `-Wall -Wextra -Wpedantic -Werror=vla`.

---

### Highlights of Version 0.3.5 Sandbox Systems

#### 1. 3D Camera Ray-Picking & Impulse Toss (`E` Key)
* **Screen-to-World Ray Unprojection (`khr_camera_screen_to_ray`)**:
  * Unprojects cursor coordinates $(s_x, s_y)$ through the camera's FOV, aspect ratio, and view orientation into world-space ray origin $\mathbf{ro}$ and unit direction vector $\mathbf{rd}$.
  * Handles arbitrary viewport resizing, window aspect ratios, and full 3D camera orbits.
* **Exact Primitive Intersections**:
  * `khr_ray_intersect_sphere`: Solves quadratic ray-sphere intersection with negative discriminant and backward miss rejection.
  * `khr_ray_intersect_aabb`: Implements Kay-Kajiya slab intersection with IEEE 754 division-by-zero handling.
* **Targeted Physics Toss**:
  * Pressing `E` scans all dynamic bodies in the scene along the line of sight, picks the nearest intersected rigid body, and delivers an SI impulse ($\mathbf{rd} \cdot 18.0 \text{ N}\cdot\text{s}$) with upward lift and acoustic feedback.

#### 2. Dynamic Mesh Spawner (`0`–`4` Keys)
* Runtime bodies can now be spawned dynamically without restarting the engine or reallocating GPU memory:
  * **Key `0`**: Procedural UV Sphere (Coral Red, Dielectric)
  * **Key `1`**: Procedural Torus (Emerald Green, Metallic)
  * **Key `2`**: Procedural Capsule / Cylinder (Sapphire Blue, Dielectric)
  * **Key `3`**: Procedural Chamfer Box (Amber Gold, Metallic)
  * **Key `4`**: Suzanne Monkey (Violet Purple, Smooth Dielectric)
* Spawns bodies high above the scene center with randomized horizontal velocity, vibrant procedural PBR materials, and automatic registration into Core 3's LBVH spatial partitioning tree.

#### 3. Kinetic Scene Kick (`K` Key) & Zero-G Toggle (`G` Key)
* **Chaos Kick (`khr_topology_sim_kick_all`)**: Injects an upward velocity impulse ($\ge 7.5 \text{ m/s}$) along with pseudo-randomized angular spin to all active dynamic bodies, instantly destabilizing stacked structures.
* **Zero-G Mode (`khr_topology_sim_toggle_gravity`)**: Toggles vertical acceleration between standard Earth gravity ($-9.81 \text{ m/s}^2$) and floating Zero-G ($0.0 \text{ m/s}^2$), allowing bodies to drift freely through the 3D space.

#### 4. Live Pacing Governor Cycler (`T` Key)
* Dynamically cycles the presentation frame pacing cap on the fly without restarting:
  $$\text{60 FPS} \longrightarrow \text{120 FPS} \longrightarrow \text{144 FPS} \longrightarrow \text{240 FPS} \longrightarrow \text{Unlocked (300+ FPS)} \longrightarrow \text{60 FPS}$$
* Recomputes minimum nano-interval hardware presentation deadlines instantly, triggering modal acoustic clicks on each shift.

#### 5. Complete Interactive Control Matrix

| Key / Input | Subsystem | Action |
|---|---|---|
| **`W` / `S`** | Camera | Flight dolly forward / backward |
| **`A` / `D`** | Camera | Flight strafe left / right |
| **`Space` / `C`** | Camera | Flight fly upward / downward |
| **`R`** | Camera | Reset camera view and look-at distance |
| **`F`** | Camera | Frame scene geometry bounding sphere |
| **`LMB Drag`** | Camera | Arcball virtual sphere orbit |
| **`MMB / Shift+LMB`** | Camera | 2D view-plane camera pan |
| **`Wheel`** | Camera | Continuous zoom in / zoom out |
| **`E`** | Physics | **3D Ray-pick nearest body under cursor and impulse toss** |
| **`K`** | Physics | **Upward kinetic kick to all dynamic bodies with spin** |
| **`G`** | Physics | **Toggle Zero-G mode (0.0 m/s² <-> -9.81 m/s²)** |
| **`0` .. `4`** | Scene / Sim | **Spawn new dynamic body falling from sky (5 mesh types)** |
| **`T`** | Pacing | **Cycle pacing governor (60 / 120 / 144 / 240 / Unlocked)** |
| **`F11`** | Window | Toggle borderless fullscreen |
| **`Esc`** | Window | Exit fullscreen / dismiss popups |

---

## Quickstart

```bash
# Build engine binary and compile Slang shaders to SPIR-V
make -j$(nproc)

# Launch interactive 3D physics sandbox
./build/engine

# Launch 64-body tumbling capsule avalanche
./build/engine --deck=experiments/capsules_n64.deck

# Launch multi-tier mixed tower experiment (spheres, boxes, capsules)
./build/engine --deck=experiments/tower_mixed.deck

# Set precision target frame rate to 144 FPS
./build/engine --target-fps=144

# Maximize GPU hardware saturation with 1024 rigid bodies across 5 meshes
./build/engine --stress-gpu

# Unlocked maximum throughput benchmark (300+ FPS)
./build/engine --unlocked --frames=300

# Select specific audio backend (pipewire, pulse, alsa, null)
./build/engine --audio=pipewire
./build/engine --audio=null

# Run headless experiment fast-forward with 18-column mechanical state CSV logging
./build/engine --headless --deck=experiments/capsules_n64.deck --ticks=600 --csv=logs/run.csv

# Run full test suite (203 tests, 8599 assertions across 18 suites)
make test

# Full AddressSanitizer + UndefinedBehaviorSanitizer verification
make sanitize
```

---

## Real-Time Engine Telemetry

```text
# Standard 60 Hz Paced Sandbox with Desktop PipeWire Audio & Live Contacts:
[AUDIO] Output: PipeWire (pw-cat pipe, 48 kHz stereo)
[TELEMETRY]  59.1 FPS (16.92 ms) | CPU Proc:  3.2% (0.29 ms) | GPU HW:  1.41 ms ( 8.3% load) | Bodies: 6 (5 meshes) | Contacts: 0

# Target 120 FPS Precision Paced Mode (--target-fps=120):
[TELEMETRY] 120.1 FPS ( 8.33 ms) | CPU Proc:  3.6% (0.28 ms) | GPU HW:  1.48 ms (17.8% load) | Bodies: 64 (2 meshes) | Contacts: 8 [120 FPS CAP]

# Unlocked Max-Throughput Silicon Mode (--unlocked):
[TELEMETRY] 312.4 FPS ( 3.20 ms) | CPU Proc:  5.4% (0.11 ms) | GPU HW:  1.51 ms (47.2% load) | Bodies: 64 (2 meshes) | Contacts: 4 [UNLOCKED]

# Hardware Saturation Stress Testing (--stress-gpu):
[TELEMETRY] 148.6 FPS ( 6.73 ms) | CPU Proc:  7.9% (0.22 ms) | GPU HW:  5.71 ms (84.8% load) | Bodies: 1024 (5 meshes) | Contacts: 48 [UNLOCKED]
```

---

## Architectural Blueprint

Khoros is engineered with **zero third-party middleware**: no `libwayland`, `liburing`, `libcglm`, `SDL`, or `GLFW`. All subsystems interface directly with Linux kernel syscalls and the Vulkan 1.4 API under strict **ISO C23** (`__STDC_VERSION__ = 202311L`).

```text
                                  [ Hardware Topology ]
                                            │
        ┌───────────────────────────────────┴───────────────────────────────────┐
        ▼                                                                       ▼
 [ Core 2: Frame-Paced Present ]                                   [ Core 3: Fixed-Tick Compute ]
  ├── Wayland Client (Raw Syscalls)                                 ├── 60/120 Hz Symplectic Integrator
  │   ├── zwp_linux_dmabuf_v1 (Zero-copy DMA-BUF)                   │   ├── 30-bit Morton Code Radix Sort
  │   ├── wp_presentation_time (Hardware VSync)                     │   └── LBVH Spatial Tree Broadphase
  │   ├── 3D Camera Ray-Picking Unprojector                         ├── 100% Pairwise SAT Narrowphase
  │   └── Flight Navigation Keys (WASD/Space/C/R)                  │   (Capsule-Capsule, Capsule-AABB)
  ├── Vulkan 1.4 GPU Command Stream                                 ├── Dynamic Runtime Body Insertion
  │   ├── cull_hiz.slang (Occlusion & Frustum)                      ├── Double-Buffered BDA Transform Flip
  │   ├── Multi-Mesh Indirect Draws (SV_InstanceID direct BDA)      ├── Desktop Audio Mixer (PipeWire/Pulse)
  │   └── Silicon GPU Timestamp Queries (VK_QUERY_TIMESTAMP)        └── Ring B (io_uring) Async Ingest
  └── Real-time Telemetry (CPU & GPU frame timers + Contacts)                   │
        │                                                                       │
        └───────────────────────────┬───────────────────────────────────────────┘
                                    ▼
                 [ 2 MiB BDA Hugepage Arena (mmap / MADV_HUGEPAGE) ]
                 ├── Slot A / Slot B Instance Transform Buffers
                 ├── Procedural Vertex & Index Buffers (Spheres/Boxes/Tori)
                 └── Draw Indirect Command & Count Buffers
```

---

## Command-Line Reference

```text
Usage: ./build/engine [OPTIONS] [blob_path | deck_path]

Presentation & Pacing Options:
  --target-fps=<N>        Cap presentation rate to N frames/sec (e.g. 60, 120, 144)
  --unlocked              Unlock presentation throttle for maximum GPU throughput
  --frames=<N>, -f <N>    Execute exactly N presentation frames and exit cleanly

Physics & Experiment Options:
  --deck=<path>           Load an experiment deck (# khoros-run v1) into live 3D window
  --headless              Run simulation headlessly in terminal without opening 3D window
  --ticks=<N>             Run exactly N simulation steps (default: 1000)
  --fixed-dt              Enforce fixed timestep dt
  --seed=<N>              Set random seed for simulation perturbations
  --no-wall-clock         Headless simulation fast-forward without display pacing
  --csv=<path>            Output per-tick mechanical state to CSV (implies headless)
  --frame-csv=<path>      Output per-present presentation time telemetry to CSV
  --hash-only             Output only 64-bit pose state hash for determinism tests

Workload & Stress Options:
  --stress-n=<N>          Spawn N rigid bodies in 3D scene across 5 meshes
  --stress-gpu            Saturate GPU with 1024 rigid bodies

Audio Configuration:
  --audio=<backend>       Audio sink: auto, pipewire, pulse, alsa, null
  --no-audio              Disable audio output (equivalent to --audio=null)
  --audio-card=<N>        Select ALSA sound card index (default: 0)
  --audio-device=<N>      Select ALSA playback device index (default: 0)

Procedural Exporters:
  --write-box <path>      Write procedural box mesh blob to file and exit
  --write-sphere <path>   Write procedural sphere mesh blob to file and exit
  --write-cylinder <path> Write procedural cylinder mesh blob to file and exit
  --write-torus <path>    Write procedural torus mesh blob to file and exit

General:
  -h, --help              Display this help message
```

---

## Verification & Test Suite

The engine enforces test contracts where every physical quantity, mathematical invariant, and rendering guarantee must have a test that can fail.

```bash
# Run 203 unit and integration tests across 18 suites
make test
```

### Test Suite Summary

| Suite | Focus Area | Assertions | Status |
|---|---|---|---|
| **Suite 01–03** | Core Topology, Ring A/B `io_uring`, IPC Messaging | 1,420 | ✔ PASS (100%) |
| **Suite 04–06** | Hugepage BDA Arena, Slang Compute Pipelines, Camera Math | 1,280 | ✔ PASS (100%) |
| **Suite 07–09** | Wayland Wire Protocol, DMA-BUF Export, MSAA/D32 Render Targets | 1,123 | ✔ PASS (100%) |
| **Suite 10–12** | Multi-Mesh Indirect Dispatch, Two-Pass Hi-Z Occlusion, Audio Mixer | 1,340 | ✔ PASS (100%) |
| **Suite 13–15** | Lock-Free Input Ring, Bindless Textures, Morton LBVH Spatial Physics | 1,470 | ✔ PASS (100%) |
| **Suite 16–18** | Ground Grid, Symplectic Euler IVP Convergence, Deck Determinism | 1,966 | ✔ PASS (100%) |
| **Total** | **18 Suites · 203 Tests** | **8,599** | **✔ PASS (100.0%)** |

All 203 tests execute with **0 leaks, 0 errors, and 0 undefined behavior** under both native compilation and AddressSanitizer + UndefinedBehaviorSanitizer (`make sanitize`).

---

## Architectural Comparison

| Conventional Middleware Stack | Khoros 0.3.5 Architecture |
|---|---|
| Middleware libraries (SDL, GLFW, cglm, libwayland) | Pure ISO C23 + Raw Linux Syscalls + Vulkan 1.4 |
| CPU-to-GPU mesh uploads every frame | Single 2 MiB BDA hugepage arena; GPU pulls directly |
| Incomplete collision handling / external PhysX | 100% pairwise SAT collision (Sphere, Box, Capsule, Plane) |
| Hardcoded mouse-only camera | Integrated 3D keyboard flight navigation (WASD/Space/C/R) + mouse |
| Static scene entities | **Dynamic runtime body spawner (0–4 keys) & 3D ray-picking toss (E key)** |
| Hardcoded gravity | **Live Zero-G toggle (G key) & chaotic kinetic agitator (K key)** |
| Fixed display sync only | **On-the-fly precision pacing cycler (T key: 60/120/144/240/unlocked)** |
| Raw ALSA conflicts causing HDMI sound hijacking | Zero-middleware PipeWire/Pulse pipe streaming to desktop sink |
| Unsynchronized audio timers with crackle | Sample-accurate modal synthesis with smooth envelope decays |
| Indeterminate simulation timesteps | Decoupled 60/120 Hz symplectic integrator with sub-frame interpolation |

---

[DEVELOPER.md](DEVELOPER.md) · [doc/](doc/README.md) · [experiments/](experiments/)
