# Wayplot

** The reality of this code and project **

Everything you see on this code is a ai slop as of right now, i must said this from the very beginning, i will re write most of the part myself again in the future but for now, there is no human written code anymore. Some of the old versions or closed beta versions were having almost half human half ai slop code but this version is created to push the limits so i can learn new things and get new ideas by looking at it.

This project will have no real life application now, its main purpose to give the real human of this project to ideas and ways to build an actual engine.
Right now, the project has 2 rings, ring A and ring B, each of them is pinned to specific threads except the main thread, rings are small io_uring submission life cycle for the io_uring syscal, ring A is the small one with 4kb sqe and always running, the ring B is the one with big pages 2mb and only actiave when something is triggered it, the rings are working async, they submit their data as the same ring so the main submit is always running

---

```bash
make
./build/engine                                # interactive window (PBR Damascus Suzanne + lights)
./build/engine --deck=experiments/spheres_n8.deck # 8 colliding spheres in live 3D window
./build/engine assets/shapes/sphere.kblob     # procedural PBR sphere skin
make test                                     # 194 / 194 tests passing (100.0%) across 18 suites
make bench                                    # N-sweep empirical benchmarks (Tiger Lake Xe)
```

Build, test, export, design rules: [DEVELOPER.md](DEVELOPER.md).

---

## Now (Version 0.3.0)

> **Caption**: *The banner is measured, the integrator is an IVP you can fail, the run is an experiment.*

A bare-metal scientific instrument, not just a visual demo. Live Wayland window with zero-copy BDA GPU vertex pulling, Cook-Torrance GGX PBR, Two-Pass Hi-Z occlusion culling, 30-bit Morton LBVH broadphase on Core 3, and physical harmonic audio synthesis.

* **Experiment Decks (`.deck`)**: Scriptable `# khoros-run v1` physics runs with SI units (restitution $e$, friction $\mu$, gravity $g$, masses, and initial velocities).
* **Live 3D & Headless Modes**: Run `--deck=<path>` interactively in 3D or `--headless --csv=<path>` for high-frequency 18-column mechanical telemetry.
* **Deterministic Verification**: Bit-exact state hashes (`--hash-only`) proving numerical reproducibility across runs.
* **Xe N-Sweep Benchmarks**: Measured latency from $N=1$ to $N=1024$ bodies on Intel Iris Xe.

ISO C23. Vulkan 1.4. Zero third-party middleware (no `libwayland`, `liburing`, `libcglm`, `SDL`, or `GLFW`). Native Linux 7.2+ `io_uring` + raw syscalls only.

## Next

Multi-draw indirect batching per distinct mesh, volume rendering passes, and extended rigid-body constraint solvers. The same machine, aimed at a field. Time you can scrub. A working set: GPU has 2 GB, file is 8 GB, only the slice on screen (and the time you are about to open) is hot.

## Far

Slides, a desktop, eventually an OS that does not treat a syscall as a cache bomb. That is later. This repo has to run on Linux *now*.

---

## Why it is built like this

A faster `imshow` still copies. A GLFW window still redraws. A library swapchain still hides when the compositor is done.

So the engine talks to the kernel on two `io_uring` rings, presents with DMA-BUF and explicit sync, and maps one hugepage the GPU can see. Chrome and payload share that map so a file cannot smash the title bar. The present thread never waits on disk.

It looks over-engineered if you think the problem is drawing a cube. The problem is an 8 GB series that must stay a series.

## Why this kernel

**Linux 7.2+** (CachyOS is the box we build on). Registered rings, `NO_IOWAIT`, direct descriptors, linked ingest, hugepages — that ABI is the product. GitHub’s 6.x runner is CI, not the target.

## Not only speed

| Usual stack | Here |
|---|---|
| Copy into objects, raster, upload, repeat | Data stays. GPU reads it. |
| Window wakes forever | Present sleeps until dirty |
| File parsed every tool | Native blob, ingest off the present thread |
| Fast on 1e5 points | Designed for the file that does not fit in VRAM |

Tests (`make test`) cover rings, protocol, ingest, format, hit testing. They do not grade the picture. Look at the window.

---

[DEVELOPER.md](DEVELOPER.md) · [doc/](doc/README.md)
