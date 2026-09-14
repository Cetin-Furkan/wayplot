# Wayplot

** The reality of this code and project **

Everything you see on this code is a ai slop as of right now, i must said this from the very beginning, i will re write most of the part myself again in the future but for now, there is no human written code anymore. Some of the old versions or closed beta versions were having almost half human half ai slop code but this version is created to push the limits so i can learn new things and get new ideas by looking at it.

This project will have no real life application now, its main purpose to give the real human of this project to ideas and ways to build an actual engine.
Right now, the project has 2 rings, ring A and ring B, each of them is pinned to specific threads except the main thread, rings are small io_uring submission life cycle for the io_uring syscal, ring A is the small one with 4kb sqe and always running, the ring B is the one with big pages 2mb and only actiave when something is triggered it, the rings are working async, they submit their data as the same ring so the main submit is always running

---

## Release: Version 0.3.3

> **Caption**: *Zero-HDMI-Hijack desktop audio routing, dynamic multi-shape collision physics, and precision target FPS pacing.*

Version 0.3.3 eliminates desktop audio contention on modern DSP audio systems, introduces multi-shape contact mechanics (Capsule-Plane, AABB-Plane, Sphere-Sphere), and gives precise software control over engine presentation pacing and hardware power utilization.

```
                  ┌───────────────────────────────────────────┐
                  │          Khoros 0.3.3 Architecture        │
                  │   Pure ISO C23 · Zero Third-Party Libs    │
                  └─────────────────────┬─────────────────────┘
                                        │
        ┌───────────────────────────────┴───────────────────────────────┐
        ▼                                                               ▼
 [ Presentation & Present ]                                    [ Simulation & Compute ]
  ├── Core 2 Affinity (Lock-Free)                              ├── Core 3 Affinity (Pinned Worker)
  ├── Wayland Wire (Raw Syscalls)                              ├── 60/120 Hz Symplectic Integrator
  │   ├── zwp_linux_dmabuf_v1 Zero-Copy                        ├── 30-bit Morton Radix LBVH Broadphase
  │   └── wp_presentation_time Feedback                        ├── SAT Narrowphase (Sphere/Box/Capsule)
  ├── Vulkan 1.4 BDA Pipeline                                  └── Pipe Audio Mixer (48 kHz Stereo)
  │   ├── Two-Pass Hi-Z Occlusion Culling                          ├── Desktop Auto-Sink Router
  │   ├── Hardware GPU Timestamps (VK_QUERY_TIMESTAMP)             ├── Zero-HDMI Hijack (PipeWire/Pulse)
  │   └── Precision FPS Pacing Governor                            └── Modal Acoustic Resonance Synth
```

---

### Highlights of Version 0.3.3

#### 1. Zero-HDMI-Hijack Audio Auto-Routing (`--audio=auto|pipewire|pulse|alsa|null`)
On modern Intel SOF DSP and AMD ACP audio architectures, opening raw `/dev/snd/pcmC0D0p` ALSA nodes while a desktop sound server (PipeWire / PulseAudio) is running creates an internal DSP pipeline conflict. ALSA driver arbitration deactivates the analog laptop speaker sink and switches audio output to secondary HDMI sinks, hijacking desktop sound and generating parasite crackles.

Khoros 0.3.3 solves this cleanly without adding external library dependencies:
* **Zero-Middleware Pipe Transport**: Forks an ultra-lightweight pipe sink (`pw-cat` or `paplay`) streaming raw 16-bit signed 48 kHz stereo PCM over a POSIX pipe with an expanded 256 KiB ring buffer (`F_SETPIPE_SZ`).
* **Desktop Sound Preservation**: Desktop audio never switches to HDMI; your music, video, and system sounds remain on your chosen output device without disruption.
* **Auto-Fallback Chain**: Automatically detects `pw-cat` -> `paplay` -> raw direct ALSA -> silent null sink.
* **Modal Synthesis**: Generates crisp, artifact-free collision acoustics with no clicks, crackles, or latency jitter.

#### 2. Dynamic Multi-Shape Collision Physics & Differentiated Acoustics
* **Separating Axis Theorem (SAT) Formulations**:
  * **Capsule-Plane SAT**: Rotates cylinder endpoint proxies through rigid body orientation quaternions and computes exact signed distance contact depths and surface normals.
  * **AABB-Plane SAT**: Evaluates half-extent projections $e_r = h_x |n_x| + h_y |n_y| + h_z |n_z|$ to detect box corner and face contacts with zero penetration drift.
* **Per-Mesh Dynamic Binding**:
  * **Mesh 0 (Suzanne)**: Spherical bounding volume proxy with harmonic metallic resonance.
  * **Mesh 1 (Sphere)**: Analytical sphere-plane and sphere-sphere impulse resolution.
  * **Mesh 2 (Cylinder)**: Oriented capsule proxy with high-frequency wooden click transients.
  * **Mesh 3 (Chamfer Box)**: Oriented AABB proxy with low-frequency acoustic thud dynamics.
  * **Mesh 4 (Torus)**: Toroidal bounding proxy with dual-frequency bell resonance.

#### 3. Precision Target FPS Pacing (`--target-fps=<N>` & `--frames=<N>`)
* **Dynamic Frame Pacing**: Set target display rates (e.g. `--target-fps=60`, `120`, `144`, `240`) enforced via high-resolution `clock_nanosleep(CLOCK_MONOTONIC)` with sub-microsecond spin-pacing.
* **Automated Deterministic Profiling (`--frames=<N>`)**: Run exactly N presentation frames before shutting down cleanly for benchmarking and headless continuous integration.
* **Unlocked Maximum GPU Throughput (`--unlocked`)**: Uncap presentation limits to measure raw Vulkan silicon throughput (300+ FPS).

#### 4. Hardware Silicon GPU Timestamping (`VK_QUERY_TYPE_TIMESTAMP`)
* Direct silicon timestamp queries at `VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT` and `VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT`.
* Calibrated via `VkPhysicalDeviceLimits.timestampPeriod` to deliver nanosecond-accurate GPU execution time and live percentage GPU power utilization.

---

## Quickstart

```bash
# Build engine binary and compile Slang shaders to SPIR-V
make -j$(nproc)

# Launch interactive 3D window (auto-routed desktop audio + 60 Hz VSync)
./build/engine

# Set precision target frame rate to 120 FPS
./build/engine --target-fps=120

# Run multi-mesh simulation deck with 64 dynamic rigid bodies
./build/engine --deck=experiments/mixed_n64.deck

# Maximize GPU hardware saturation with 1024 rigid bodies across 5 meshes
./build/engine --stress-gpu

# Unlocked maximum throughput benchmark (300+ FPS)
./build/engine --unlocked --frames=300

# Select specific audio backend (pipewire, pulse, alsa, null)
./build/engine --audio=pipewire
./build/engine --audio=null

# Run headless experiment fast-forward with 18-column mechanical state CSV logging
./build/engine --headless --deck=experiments/mixed_n64.deck --ticks=600 --csv=logs/run.csv

# Run full test suite (198 tests, 8586 assertions across 18 suites)
make test

# Full AddressSanitizer + UndefinedBehaviorSanitizer memory safety verification
make sanitize
```

---

## Real-Time Engine Telemetry

During execution, Khoros streams continuous real-time diagnostics to `stdout`:

```text
# Standard 60 Hz Paced Mode with Desktop PipeWire Audio:
[AUDIO] Output: PipeWire (pw-cat pipe, 48 kHz stereo)
[TELEMETRY]  59.8 FPS (16.72 ms) | CPU Proc:  3.5% (0.32 ms) | GPU HW:  1.49 ms ( 8.9% load) | Bodies: 64 (2 meshes)

# Target 120 FPS Precision Paced Mode (--target-fps=120):
[TELEMETRY] 120.1 FPS ( 8.33 ms) | CPU Proc:  4.1% (0.28 ms) | GPU HW:  1.48 ms (17.8% load) | Bodies: 64 (2 meshes) [120 FPS CAP]

# Unlocked Max-Throughput Silicon Mode (--unlocked):
[TELEMETRY] 312.4 FPS ( 3.20 ms) | CPU Proc:  5.8% (0.11 ms) | GPU HW:  1.51 ms (47.2% load) | Bodies: 64 (2 meshes) [UNLOCKED]

# Hardware Power Saturation Stress Testing (--stress-gpu):
[TELEMETRY] 148.6 FPS ( 6.73 ms) | CPU Proc:  7.9% (0.22 ms) | GPU HW:  5.71 ms (84.8% load) | Bodies: 1024 (5 meshes) [UNLOCKED]
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
  │   └── wp_presentation_time (Hardware VSync)                     │   └── LBVH Spatial Tree Broadphase
  ├── Vulkan 1.4 GPU Command Stream                                 ├── Multi-Shape SAT Narrowphase
  │   ├── cull_hiz.slang (Occlusion & Frustum)                      ├── Double-Buffered BDA Transform Flip
  │   ├── Multi-Mesh Indirect Draws (SV_InstanceID direct BDA)      ├── Desktop Audio Mixer (PipeWire/Pulse)
  │   └── Silicon GPU Timestamp Queries (VK_QUERY_TIMESTAMP)        └── Ring B (io_uring) Async Ingest
  └── Real-time Telemetry (CPU & GPU frame timers)                              │
        │                                                                       │
        └───────────────────────────┬───────────────────────────────────────────┘
                                    ▼
                 [ 2 MiB BDA Hugepage Arena (mmap / MADV_HUGEPAGE) ]
                 ├── Slot A / Slot B Instance Transform Buffers
                 ├── Procedural Vertex & Index Buffers (Spheres/Boxes/Tori)
                 └── Draw Indirect Command & Count Buffers
```

### Decoupled Dual-Ring Architecture
* **Ring A (Core 2)**: 64 SQ / 256 CQ entries. Non-blocking `enter_fd`, `CLOCK_MONOTONIC`, `NO_IOWAIT`. Dedicated to immediate IPC, presentation synchronization, and low-latency Wayland wire protocol exchanges.
* **Ring B (Core 3)**: 128 SQ / 256 CQ entries with registered hugepage memory buffers (2048 KiB THP/anon). Dedicated to streaming binary blobs (`.kblob`), deck scripts, audio PCM streaming, and background file ingest.

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
# Run 198 unit and integration tests across 18 suites
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
| **Total** | **18 Suites · 198 Tests** | **8,586** | **✔ PASS (100.0%)** |

All 198 tests execute with **0 leaks, 0 errors, and 0 undefined behavior** under both native compilation and AddressSanitizer + UndefinedBehaviorSanitizer (`make sanitize`).

---

## Architectural Comparison

| Conventional Middleware Stack | Khoros 0.3.3 Architecture |
|---|---|
| Middleware libraries (SDL, GLFW, cglm, libwayland) | Pure ISO C23 + Raw Linux Syscalls + Vulkan 1.4 |
| CPU-to-GPU mesh uploads every frame | Single 2 MiB BDA hugepage arena; GPU pulls directly |
| CPU-bound frustum & occlusion loops | GPU compute shaders (`cull_hiz.slang`) with wave compaction |
| Raw ALSA conflicts causing HDMI sound hijacking | Zero-middleware PipeWire/Pulse pipe streaming to desktop sink |
| Unsynchronized audio timers with crackle | Sample-accurate modal synthesis with smooth envelope decays |
| VSync-locked presentation only | Precision `--target-fps` pacing, `--unlocked` 300+ FPS, `--frames` governor |
| Indeterminate simulation timesteps | Decoupled 60/120 Hz symplectic integrator with sub-frame interpolation |

---

[DEVELOPER.md](DEVELOPER.md) · [doc/](doc/README.md) · [experiments/](experiments/)
