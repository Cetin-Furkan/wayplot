# Wayplot

** The reality of this code and project **

Everything you see on this code is a ai slop as of right now, i must said this from the very beginning, i will re write most of the part myself again in the future but for now, there is no human written code anymore. Some of the old versions or closed beta versions were having almost half human half ai slop code but this version is created to push the limits so i can learn new things and get new ideas by looking at it.

This project will have no real life application now, its main purpose to give the real human of this project to ideas and ways to build an actual engine.
Right now, the project has 2 rings, ring A and ring B, each of them is pinned to specific threads except the main thread, rings are small io_uring submission life cycle for the io_uring syscal, ring A is the small one with 4kb sqe and always running, the ring B is the one with big pages 2mb and only actiave when something is triggered it, the rings are working async, they submit their data as the same ring so the main submit is always running

---

```bash
make
./build/engine                 # window, default box
./build/engine mesh.khrb       # ingest a native mesh
```

Build, test, export, design rules: [DEVELOPER.md](DEVELOPER.md).

---

## Now

A real window. GPU mesh in the client. Orbit, pan, zoom, frame. Axis gimbal that does not steal the close button. Native `.khrb` — CPU checks once, GPU reads in place. Dirty present: no change, no frame.

ISO C23. Vulkan 1.4. No `libwayland`. No `liburing`. No OpenGL. No Vulkan WSI.

## Next

The same machine, aimed at a field. Time you can scrub. A working set: GPU has 2 GB, file is 8 GB, only the slice on screen (and the time you are about to open) is hot. Then other programs sit on this instead of standing up their own viewer.

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
