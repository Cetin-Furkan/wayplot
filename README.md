# Wayplot

**The reinvention of the wheel.**

This is not a plotting library with a faster inner loop. It is an engine for work that is *big in time and space*: long series, fields, complex-valued problems, the things a notebook turns into a RAM spike and a frozen window. The wheel we are replacing is the one that copies the world in order to show it.

The name is the brief. **Plot way more** — not “a plot on Wayland.”

The program in this tree is the near future of that idea: a Linux engine you can sit other work on. The far future is a different machine (window system, desktop, kernel). This repository does not wait for that machine. It has to run *now*, on Linux, and be something you can point at.

---

## What you can open today

A real window. A mesh in the client. Orbit, pan, zoom, frame. A gimbal that does not fight the close button. A native file (`.khrb`) that the GPU reads in place. When nothing changes, the present loop sleeps.

That is v0.1. It is small and it is true. It is not yet the 8 GB field on a 2 GB GPU. Do not read the next section as a feature list of the binary you just built.

Drop live captures here when you have them — the cube, Suzanne, the gimbal. Until then these diagrams are the claim, not a fake screenshot:

![Only the visible slice should be hot](doc/images/working-set.svg)

![Idle when the picture did not change](doc/images/idle.svg)

---

## Not just faster

Speed is the wrong comparison. Matplotlib, and most “scientific” viewers built like it, *move* the data: into objects, into a CPU image, onto the GPU, again next frame. A two-gigabyte series becomes twelve gigabytes of copies and still looks like a JPEG of a plot.

The difference we care about is **residency**.

If the GPU has 2 GB and the file is 8 GB, the engine should not try to become 8 GB of VRAM. It should keep the file, and only the part you are looking at — and the strip of time you are about to scrub to — should be hot. The rest stays cold. When you drag the bar, the next slice is already there so it does not hitch. When you stop, the machine goes quiet.

v0.1 is a 2 MiB payload and one mesh. The *shape* is already that story: one hugepage, GPU bindless reads, ingest that does not block the present thread, frames only when dirty. The next work is to aim that shape at a field that would actually murder a notebook.

---

## Why it looks over-engineered

It looks like we reinvented sockets, buffers, and a window because we are stubborn. We are stubborn, but that is not the reason.

A plot that copies cannot idle. A window that polls cannot sit still. A swapchain you do not own cannot tell you when the compositor is done. An engine that is “just GLFW plus a shader” is fine until the dataset is larger than memory and the window is still redrawing a world that did not change.

So this tree talks to `io_uring` without `liburing`, talks Wayland without `libwayland`, presents with DMA-BUF and explicit sync instead of Vulkan WSI, and keeps chrome and payload in one mapping so ingest cannot smash the title bar. That is not a flex. It is so **the bytes stay put, the GPU can see them, and the CPU can sleep.**

We build from zero on those pieces because they are the ones we intend to keep when this engine is under other programs — and because doing them the usual way would teach the usual lesson: hide the cost in a library until the file is 8 GB.

---

## Why this kernel

The target is a **current Linux** (we develop on CachyOS 7.2). Not because new is moral. Because the I/O we need is in that kernel: registered rings, cooperative task run, `NO_IOWAIT`, direct descriptors (files that never enter the process table), linked ingest, hugepage-backed buffers.

GitHub’s runner is usually an older 6.x. That is a CI host, not the product. If a flag does not exist there, the engine is not wrong; the runner is not 7.2.

---

## Ambition

**Near:** an engine other projects can use. A function of space and time you can *see* without melting the machine. Complex numbers as something the GPU already knows how to multiply. A time bar. A cache of the past and the near future. Then enough UI to name what you are looking at.

**Far:** slides that are not PowerPoint, a desktop that is not a socket farm, eventually an OS that does not treat a syscall as a cache bomb. That is a different repository’s life. This one is the learning path that still has to ship.

---

## Tests

`make test` is green when the **plumbing** is green: rings, wire, ingest into the hugepage, hit lists, blob parse, Vulkan modules.

It does not know if the monkey faces you. It does not know if the light is mud. A hundred passing tests and a wrong picture can both be true. That gap is real; we are not going to advertise 100% as proof of the window.

---

## Run it

Linux, a 7.2-class kernel, GCC with C23, Vulkan 1.4, `slangc`.

```bash
make
./build/engine
./build/engine /path/to/mesh.khrb
```

Default mesh is a unit box. `--write-box` writes a `.khrb` and exits (no window). Blender export is a cold Python path; the engine does not parse `.blend`.

How to build, test, export, and the design rules: **[DEVELOPER.md](DEVELOPER.md)**. Subsystem manuals: **[doc/](doc/README.md)**.
