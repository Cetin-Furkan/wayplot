# Wayplot

** The reality of this code and project **

Everything you see on this code is a ai slop as of right now, i must said this from the very beginning, i will re write most of the part myself again in the future but for now, there is no human written code anymore. Some of the old versions or closed beta versions were having almost half human half ai slop code but this version is created to push the limits so i can learn new things and get new ideas by looking at it.

This project will have no real life application now, its main purpose to give the real human of this project to ideas and ways to build an actual engine.
Right now, the project has 2 rings, ring A and ring B, each of them is pinned to specific threads except the main thread, rings are small io_uring submission life cycle for the io_uring syscal, ring A is the small one with 4kb sqe and always running, the ring B is the one with big pages 2mb and only actiave when something is triggered it, the rings are working async, they submit their data as the same ring so the main submit is always running

---

## Quickstart

```bash
# Build binary and compile Slang shaders to SPIR-V
make -j$(nproc)

# Interactive 3D presentation window (Suzanne + Spheres + Boxes + Cylinders + Tori)
./build/engine

# Run multi-mesh physics experiment deck in live 3D window
./build/engine --deck=experiments/mixed_n64.deck

# Stress test with 512 physics bodies across 5 procedural meshes with live telemetry
./build/engine --stress-n=512

# Run without audio hardware access (null sink)
./build/engine --no-audio --deck=experiments/mixed_n64.deck

# Headless experiment fast-forward with 18-column mechanical state CSV logging
./build/engine --headless --deck=experiments/mixed_n64.deck --ticks=600 --csv=logs/run.csv

# Run full test suite (194 tests, 8567 assertions across 18 suites)
make test

# Memory safety & UB verification (AddressSanitizer + UndefinedBehaviorSanitizer)
make sanitize

# Hardware N-sweep benchmarks on Intel Iris Xe
make bench
```

Build details, coding standards, and architectural contracts: [DEVELOPER.md](DEVELOPER.md).

---

## Release: Version 0.3.1

> **Caption**: *Multi-mesh 3D PBR, DAC-synced modal audio, and real-time CPU/GPU telemetry.*

Version 0.3.1 elevates Khoros from single-mesh visualization into a true multi-mesh 3D simulation engine with hardware-synchronized acoustics and sub-millisecond telemetry monitoring.

### Key Capabilities in 0.3.1

1. **True Multi-Mesh 3D Scene Graph**:
   * Direct 1:1 GPU culling and multi-draw indirect passes for heterogeneous shapes in a single frame.
   * Procedural parametric mesh generation: **Suzanne (Monkey)**, **UV Spheres**, **Cylinders**, **Chamfer Boxes**, and **Tori**.
   * Decks containing mixed bodies (`shape=sphere`, `shape=aabb`, `shape=capsule`) render their true geometries simultaneously with Cook-Torrance GGX PBR materials.

2. **DAC-Synchronized Audio Engine**:
   * Replaced unsynchronized software `timerfd` timers with hardware DAC pacing via `poll(POLLOUT)` directly on the ALSA PCM file descriptor.
   * Completely eliminates crystal drift, buffer under-runs (`EPIPE`), and parasitic crackle clicks.
   * Added CLI controls: `--no-audio` for silent/null-sink operation, `--audio-card=<N>`, and `--audio-device=<N>` to prevent unexpected HDMI audio hijacking.

3. **High-Precision CPU & GPU Telemetry**:
   * Continuous real-time measurement of overall process CPU utilization via `CLOCK_PROCESS_CPUTIME_ID`.
   * Frame CPU rendering latency (scene encoding + DMA-BUF presentation commit) and GPU synchronization wait time.
   * Real-time console reporting every 1000 ms:
     ```text
     [TELEMETRY]  59.2 FPS (16.90 ms) | CPU Process:  3.2% | Render CPU: 0.29 ms | GPU Wait: 0.00 ms | Bodies: 64 (2 meshes)
     ```

4. **Dynamic Stress Testing**:
   * `--stress-n=<N>` and `--stress-gpu` CLI flags to spawn hundreds or thousands of colliding bodies across all 5 meshes, evaluating LBVH broadphase scaling and GPU instance throughput.

---

## Architectural Blueprint

Khoros is engineered with zero third-party middleware: no `libwayland`, `liburing`, `libcglm`, `SDL`, or `GLFW`. All operations interface directly with Linux kernel syscalls and the Vulkan 1.4 API under strict ISO C23 (`__STDC_VERSION__ = 202311L`).

```text
                                  [ Hardware Topology ]
                                            │
        ┌───────────────────────────────────┴───────────────────────────────────┐
        ▼                                                                       ▼
 [ Core 2: Frame-Paced Present ]                                   [ Core 3: Fixed-Tick Compute ]
  ├── Wayland Client (Raw Syscalls)                                 ├── 60 Hz Symplectic Euler Physics
  │   ├── zwp_linux_dmabuf_v1 (Zero-copy)                          │   ├── 30-bit Morton Code Radix Sort
  │   └── wp_presentation_time (Hardware VSync)                     │   └── LBVH Tree Collision Broadphase
  ├── Vulkan 1.4 GPU Command Submissions                           ├── Double-Buffered BDA Transform Flip
  │   ├── Slang Compute: cull_hiz.slang (Occlusion & Frustum)       ├── Modal Harmonic Audio Synthesis
  │   ├── Multi-Mesh Indirect Draws (SV_InstanceID direct BDA)     └── Ring B (io_uring) Async Ingest
  └── Real-time Telemetry (CPU & GPU frame timers)                              │
        │                                                                       │
        └───────────────────────────┬───────────────────────────────────────────┘
                                    ▼
                 [ 2 MiB BDA Hugepage Arena (mmap / MADV_HUGEPAGE) ]
                 ├── Slot A / Slot B Instance Transform Buffers
                 ├── Procedural Vertex & Index Buffers (Spheres/Boxes/Tori)
                 └── Draw Indirect Command & Count Buffers
```

### Decoupled Ring Architecture
* **Ring A (Core 2)**: 64 SQ / 256 CQ entries. Non-blocking `enter_fd`, `CLOCK_MONOTONIC`, `NO_IOWAIT`. Dedicated to immediate IPC, presentation sync, and low-latency Wayland wire protocol exchanges.
* **Ring B (Core 3)**: 128 SQ / 256 CQ entries with registered hugepage memory buffers (2048 KiB THP/anon). Dedicated to streaming binary blobs (`.kblob`), deck scripts, audio PCM streaming, and background file ingest.

---

## Command-Line Reference

```text
Usage: ./build/engine [OPTIONS] [blob_path | deck_path]

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
  --no-audio              Disable ALSA audio engine (null sink)
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

The engine enforces test contracts where every physical quantity and rendering promise must have a test that can fail.

```bash
# Run 194 unit and integration tests across 18 suites
make test
```

### Verified Test Suites

| Suite | Description | Status |
|---|---|---|
| **Suite 01–03** | Core Topology, Ring A/B `io_uring`, IPC Messaging | ✔ PASS (100%) |
| **Suite 04–06** | Hugepage BDA Arena, Slang Compute Pipelines, Camera Math | ✔ PASS (100%) |
| **Suite 07–09** | Wayland Wire Protocol, DMA-BUF Export, MSAA/D32 Render Targets | ✔ PASS (100%) |
| **Suite 10–12** | Multi-Mesh Indirect Dispatch, Two-Pass Hi-Z Occlusion, Audio Mixer | ✔ PASS (100%) |
| **Suite 13–15** | Lock-free Input Ring, Bindless Textures, Morton LBVH Spatial Physics | ✔ PASS (100%) |
| **Suite 16–18** | Ground Grid, Symplectic Euler IVP Convergence, Deck Determinism | ✔ PASS (100%) |

All 194 tests pass cleanly under both native execution and AddressSanitizer + UndefinedBehaviorSanitizer (`make sanitize`).

---

## Comparison

| Conventional Stack | Khoros Architecture |
|---|---|
| Middleware layers (SDL, GLFW, cglm, libwayland) | Pure ISO C23 + Raw Linux Syscalls + Vulkan 1.4 |
| CPU-to-GPU mesh uploads every frame | Single BDA hugepage arena; GPU pulls directly |
| CPU-bound frustum & occlusion loops | GPU compute shaders (`cull_hiz.slang`) with wave compaction |
| Unsynchronized audio timers with crackle | Hardware DAC `poll(POLLOUT)` ALSA sync |
| Indeterminate simulation timesteps | Decoupled 60 Hz symplectic integrator with sub-frame interpolation |

---

[DEVELOPER.md](DEVELOPER.md) · [doc/](doc/README.md) · [experiments/](experiments/)
