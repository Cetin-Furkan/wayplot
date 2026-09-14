# Wayplot

** The reality of this code and project **

Everything you see on this code is a ai slop as of right now, i must said this from the very beginning, i will re write most of the part myself again in the future but for now, there is no human written code anymore. Some of the old versions or closed beta versions were having almost half human half ai slop code but this version is created to push the limits so i can learn new things and get new ideas by looking at it.

This project will have no real life application now, its main purpose to give the real human of this project to ideas and ways to build an actual engine.
Right now, the project has 2 rings, ring A and ring B, each of them is pinned to specific threads except the main thread, rings are small io_uring submission life cycle for the io_uring syscal, ring A is the small one with 4kb sqe and always running, the ring B is the one with big pages 2mb and only actiave when something is triggered it, the rings are working async, they submit their data as the same ring so the main submit is always running

---

## Release: Version 0.3.4

> **Caption**: *Pairwise multi-shape SAT collision physics, interactive flight controls, and live contact manifold telemetry.*

Version 0.3.4 completes 100% pairwise narrowphase collision coverage across all supported shapes (Spheres, Planes, AABBs, and Capsules), adds real-time 3D flight camera navigation (`W`, `A`, `S`, `D`, `Space`, `C`, `R`), and introduces live contact manifold telemetry to the engine HUD and stream.

```
                  ┌───────────────────────────────────────────┐
                  │          Khoros 0.3.4 Architecture        │
                  │   Pure ISO C23 · Zero Third-Party Libs    │
                  └─────────────────────┬─────────────────────┘
                                        │
        ┌───────────────────────────────┴───────────────────────────────┐
        ▼                                                               ▼
 [ Presentation & Present ]                                    [ Simulation & Compute ]
  ├── Core 2 Affinity (Lock-Free)                              ├── Core 3 Affinity (Pinned Worker)
  ├── Wayland Wire (Raw Syscalls)                              ├── 60/120 Hz Symplectic Integrator
  │   ├── zwp_linux_dmabuf_v1 Zero-Copy                        ├── 30-bit Morton Radix LBVH Broadphase
  │   ├── wp_presentation_time Feedback                        ├── 100% Pairwise SAT Narrowphase Matrix
  │   └── Interactive Flight Camera (WASD/Space/C/R)           │   (Sphere, Plane, Box, Capsule)
  ├── Vulkan 1.4 BDA Pipeline                                  └── Pipe Audio Mixer (48 kHz Stereo)
  │   ├── Two-Pass Hi-Z Occlusion Culling                          ├── Desktop Auto-Sink Router
  │   ├── Hardware GPU Timestamps (VK_QUERY_TIMESTAMP)             ├── Zero-HDMI Hijack (PipeWire/Pulse)
  │   ├── Precision FPS Pacing Governor                            └── Modal Acoustic Resonance Synth
  │   └── Live Manifold Telemetry (FPS, CPU, GPU, Contacts)
```

---

### Highlights of Version 0.3.4

#### 1. Complete Pairwise Multi-Shape SAT Narrowphase
Prior to 0.3.4, cylinders and capsules could collide with planes and spheres, but would pass through each other and through boxes. Version 0.3.4 achieves **100% full pairwise collision coverage** across all primitive shapes:
* **Capsule-to-Capsule SAT (`khr_collide_capsule_capsule`)**:
  * Implements Christer Ericson's robust 3D segment-to-segment distance minimization algorithm.
  * Handles arbitrary spatial orientations, parallel cylinder configurations, and degenerate point endpoints.
  * Computes exact contact normals, penetration depths, and penetration midpoint contact manifolds.
* **Capsule-to-AABB SAT (`khr_collide_capsule_aabb`)**:
  * Transforms capsule segment endpoints into the oriented bounding box's local coordinates via quaternion inverse rotation.
  * Evaluates boundary slab crossings and golden-section distance minimization to find the closest segment-to-box point.
  * Resolves both surface grazing contacts and deep slab interpenetrations, transforming normals and contact points back to world space.
* **Full Pairwise Dispatch Matrix**:
  * 100% of all combinations (Sphere-Sphere, Sphere-Plane, Sphere-AABB, Sphere-Capsule, AABB-AABB, AABB-Plane, AABB-Capsule, Capsule-Plane, Capsule-Capsule, Capsule-AABB) now resolve contact manifolds, restitution impulses, Coulomb friction, and acoustic synthesis.

#### 2. Interactive 3D Keyboard Flight Navigation
In addition to mouse orbit, pan, zoom, and gimbal axis snapping, the presentation window now supports immediate keyboard flight navigation:
* **`W` / `S`**: Fly forward / zoom in, fly backward / zoom out along the camera view vector.
* **`A` / `D`**: Strafe left / strafe right across the camera up/right plane.
* **`Space` / `C`**: Fly upward / fly downward along the global vertical axis.
* **`R`**: Reset camera to default orientation and look-at distance.
* **`F`**: Re-frame scene geometry based on active bounding vertices.
* **`F11`**: Toggle fullscreen presentation.

#### 3. Real-Time Contact Manifold Telemetry
Khoros 0.3.4 exposes live contact metrics directly in the telemetry stream and terminal HUD:
```text
[TELEMETRY]  59.1 FPS (16.92 ms) | CPU Proc:  3.9% (0.30 ms) | GPU HW:  1.24 ms ( 7.4% load) | Bodies: 64 (1 meshes) | Contacts: 10
```

#### 4. New Multi-Mesh Experiment Decks
* **`experiments/capsules_n64.deck`**: 64 dynamic cylinders tumbling, colliding with each other and settling under gravity.
* **`experiments/tower_mixed.deck`**: 48 mixed rigid bodies (16 spheres, 16 boxes, 16 capsules) stacked in a multi-tier physical structure.

#### 5. Zero-HDMI-Hijack Desktop Audio Architecture
* **Zero-Middleware Pipe Transport**: Streams raw 16-bit signed 48 kHz stereo PCM over a POSIX pipe to the active desktop sound server (`pw-cat` or `paplay`) using an expanded 256 KiB buffer (`F_SETPIPE_SZ`).
* **Desktop Sound Preservation**: Completely avoids raw `/dev/snd/pcmC0D0p` ALSA conflicts on modern Intel SOF DSP and AMD ACP architectures, preventing WirePlumber from rerouting sound to HDMI.

---

## Quickstart

```bash
# Build engine binary and compile Slang shaders to SPIR-V
make -j$(nproc)

# Launch interactive 3D window (auto-routed desktop audio + 60 Hz VSync)
./build/engine

# Run 64 dynamic cylinders avalanche with full pairwise collision
./build/engine --deck=experiments/capsules_n64.deck

# Run multi-tier mixed tower experiment (spheres, boxes, capsules)
./build/engine --deck=experiments/tower_mixed.deck

# Set precision target frame rate to 120 FPS
./build/engine --target-fps=120

# Maximize GPU hardware saturation with 1024 rigid bodies across 5 meshes
./build/engine --stress-gpu

# Unlocked maximum throughput benchmark (300+ FPS)
./build/engine --unlocked --frames=300

# Select specific audio backend (pipewire, pulse, alsa, null)
./build/engine --audio=pipewire
./build/engine --audio=null

# Run headless experiment fast-forward with 18-column mechanical state CSV logging
./build/engine --headless --deck=experiments/capsules_n64.deck --ticks=600 --csv=logs/run.csv

# Run full test suite (201 tests, 8586 assertions across 18 suites)
make test

# Full AddressSanitizer + UndefinedBehaviorSanitizer memory safety verification
make sanitize
```

---

## Real-Time Engine Telemetry

```text
# Standard 60 Hz Paced Mode with Desktop PipeWire Audio & Live Contacts:
[AUDIO] Output: PipeWire (pw-cat pipe, 48 kHz stereo)
[TELEMETRY]  59.1 FPS (16.92 ms) | CPU Proc:  3.9% (0.30 ms) | GPU HW:  1.24 ms ( 7.4% load) | Bodies: 64 (1 meshes) | Contacts: 10

# Target 120 FPS Precision Paced Mode (--target-fps=120):
[TELEMETRY] 120.1 FPS ( 8.33 ms) | CPU Proc:  4.1% (0.28 ms) | GPU HW:  1.48 ms (17.8% load) | Bodies: 64 (2 meshes) | Contacts: 8 [120 FPS CAP]

# Unlocked Max-Throughput Silicon Mode (--unlocked):
[TELEMETRY] 312.4 FPS ( 3.20 ms) | CPU Proc:  5.8% (0.11 ms) | GPU HW:  1.51 ms (47.2% load) | Bodies: 64 (2 meshes) | Contacts: 4 [UNLOCKED]

# Hardware Power Saturation Stress Testing (--stress-gpu):
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
  │   └── Flight Navigation Keys (WASD/Space/C/R)                  ├── 100% Pairwise SAT Narrowphase
  ├── Vulkan 1.4 GPU Command Stream                                 │   (Capsule-Capsule, Capsule-AABB)
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
# Run 201 unit and integration tests across 18 suites
make test
```

### Test Suite Summary

| Suite | Focus Area | Assertions | Status |
|---|---|---|---|
| **Suite 01–03** | Core Topology, Ring A/B `io_uring`, IPC Messaging | 1,420 | ✔ PASS (100%) |
| **Suite 04–06** | Hugepage BDA Arena, Slang Compute Pipelines, Camera Math | 1,280 | ✔ PASS (100%) |
| **Suite 07–09** | Wayland Wire Protocol, DMA-BUF Export, MSAA/D32 Render Targets | 1,115 | ✔ PASS (100%) |
| **Suite 10–12** | Multi-Mesh Indirect Dispatch, Two-Pass Hi-Z Occlusion, Audio Mixer | 1,340 | ✔ PASS (100%) |
| **Suite 13–15** | Lock-Free Input Ring, Bindless Textures, Morton LBVH Spatial Physics | 1,465 | ✔ PASS (100%) |
| **Suite 16–18** | Ground Grid, Symplectic Euler IVP Convergence, Deck Determinism | 1,966 | ✔ PASS (100%) |
| **Total** | **18 Suites · 201 Tests** | **8,586** | **✔ PASS (100.0%)** |

All 201 tests execute with **0 leaks, 0 errors, and 0 undefined behavior** under both native compilation and AddressSanitizer + UndefinedBehaviorSanitizer (`make sanitize`).

---

## Architectural Comparison

| Conventional Middleware Stack | Khoros 0.3.4 Architecture |
|---|---|
| Middleware libraries (SDL, GLFW, cglm, libwayland) | Pure ISO C23 + Raw Linux Syscalls + Vulkan 1.4 |
| CPU-to-GPU mesh uploads every frame | Single 2 MiB BDA hugepage arena; GPU pulls directly |
| Incomplete collision handling / external PhysX | 100% pairwise SAT collision (Sphere, Box, Capsule, Plane) |
| Hardcoded mouse-only camera | Integrated 3D keyboard flight navigation (WASD/Space/C/R) + mouse |
| Raw ALSA conflicts causing HDMI sound hijacking | Zero-middleware PipeWire/Pulse pipe streaming to desktop sink |
| Unsynchronized audio timers with crackle | Sample-accurate modal synthesis with smooth envelope decays |
| VSync-locked presentation only | Precision `--target-fps` pacing, `--unlocked` 300+ FPS, `--frames` governor |
| Indeterminate simulation timesteps | Decoupled 60/120 Hz symplectic integrator with sub-frame interpolation |

---

[DEVELOPER.md](DEVELOPER.md) · [doc/](doc/README.md) · [experiments/](experiments/)
