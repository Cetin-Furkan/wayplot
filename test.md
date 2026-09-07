═══════════════════════════════════════════════════════════════════════════════════════════════════════════════
                              WAYPLOT ULTIMATE HARDWARE & KERNEL ENGINE TOPOLOGY
═══════════════════════════════════════════════════════════════════════════════════════════════════════════════

                                CPU CORE CLUSTER (Shared L3 Cache: ~32–96 MB)
   ┌───────────────────────────────────────────────────────────┬──────────────────────────────────────────────┐
   │  CPU CORE 0 (Direct PCIe & GPU Interrupt Affinity)        │  CPU CORE 1 (Adjacent Compute Core)          │
   ├───────────────────────────────────────────────────────────┼──────────────────────────────────────────────┤
   │  THREAD 1: REAL-TIME PRESENTATION & WAYLAND (Hot Loop)    │  THREAD 2: ASSET STREAMING & COMPUTE (Worker)│
   │  Memory Footprint: ~9 KiB Ring + 4 KiB Clean Standard     │  Memory Footprint: 2 MB Hugepages            │
   │  TLB Footprint: 2 Entries Total (Zero Page Faults)        │  (Vulkan Host-Visible Device Memory)         │
   └─────────────────────────────┬─────────────────────────────┴──────────────────────┬───────────────────────┘
                                 │                                                    │
                                 ▼                                                    ▼
   ┌───────────────────────────────────────────────────────────┐        ┌─────────────────────────────────────┐
   │                  RING A (The Real-Time Ring)              │        │      RING B (The Throughput Ring)   │
   │  • Flags: SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN    │        │  • Flags: SINGLE_ISSUER |           │
   │           NO_SQARRAY | NO_IOWAIT | CLAMP | CQSIZE         │        │           DEFER_TASKRUN |           │
   │  • Size:  64 SQEs (4 KiB) × 256 CQEs (4 KiB)              │        │           REGISTER_BUFFERS          │
   │  • Fast:  REGISTER_RING_FDS (enter_fd = 0)                │        │  • Size:  128 SQEs × 256 CQEs       │
   │  • Clock: IORING_REGISTER_CLOCK (CLOCK_MONOTONIC)         │        │  • Mem:   Pre-mapped VkDeviceMemory │
   └─────────────────────────────┬─────────────────────────────┘        └──────────────────┬──────────────────┘
                                 │                                                         │
         ┌───────────────────────┼───────────────────────┐                                 │
         ▼                       ▼                       ▼                                 │
   WAYLAND INBOUND        WAYLAND OUTBOUND         GPU FENCE SYNC                          │
 ─────────────────────  ────────────────────     ──────────────────                        │
 • Multishot RECVMSG    • Batched SENDMSG        • wp_linux_drm_                           │
 • Tiered PBUF_RING:      with                     syncobj_surface                         │
   - Tier 0: 128×256B     IOSQE_CQE_SKIP_SUCCESS • Zero-wait async                         │
     (4000Hz mouse/keys)  (Zero CQE churn)         acquire/release                         │
   - Tier 1: 16×64KiB   • SCM_RIGHTS fds         • POLL_ADD on DRM                         │
     (Registry bursts)    installed via            eventfd only if                         │
 • Kernel Zero-Copy       IORING_OP_FIXED_FD_      swapchain is                            │
   recvmsg_out parser     INSTALL                  starved                                 │
         │                       │                       │                                 │
         │                       │                       │                                 │
         └───────────────────────┼───────────────────────┘                                 │
                                 │                                                         │
                                 │      ◄── IORING_OP_MSG_RING (Sub-15ns IPC) ─────────────┘
                                 │          (Writes 64-bit Vulkan BDA pointer directly into
                                 │           Ring A's CQ ring via shared L3 CPU cache line)
                                 ▼
   ┌───────────────────────────────────────────────────────────┐        ┌─────────────────────────────────────┐
   │               VULKAN 1.4 BDA RENDER PASS                  │        │        STORAGE & INGESTION DMA      │
   │  • Push Constants: 64-bit GPU Buffer Device Addresses     │        │  • Cold Path (.obj / .dem):         │
   │  • Pure Push Descriptors (No Descriptor Set Allocations)  │        │    Parse on Core 1 ──► Write .wpbin │
   │  • Direct Dynamic Scissor Viewport Switching (Top/Front)  │        │  • Hot Path (.wpbin native):        │
   │  • Slang Fast Shaders (Studio Directional CAD Diffuse)    │        │    OPENAT2 (O_DIRECT) ──►           │
   └─────────────────────────────┬─────────────────────────────┘        │    READ_FIXED straight into Vulkan  │
                                 │                                      │    VkDeviceMemory (Zero CPU copies) │
                                 ▼                                      └──────────────────┬──────────────────┘
   ┌───────────────────────────────────────────────────────────┐                           │
   │                   DISPLAY HARDWARE (KMS)                  │                           ▼
   │  • Direct Scanout via DMA-BUF Import                      │                 NVMe STORAGE CONTROLLER
   │  • Hardware DRM Syncobj Timeline Point Signaling          │                 Direct PCIe Gen4/5 Bus DMA
   │  • 144 / 240 Hz VBlank Frame Cadence                      │                 (Zero-Copy into GPU RAM)
   └───────────────────────────────────────────────────────────┘



   ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════
                                        INTEL HARDWARE / LINUX KERNEL 7.x
═══════════════════════════════════════════════════════════════════════════════════════════════════════════════
                                                       │
                          ┌────────────────────────────┴────────────────────────────┐
                          ▼                                                         ▼
            ┌───────────────────────────┐                             ┌───────────────────────────┐
            │   CPU Core 0 (Thread 1)   │                             │   CPU Core 1 (Thread 2)   │
            │   Presentation & Render   │                             │    Storage DMA Worker     │
            ├───────────────────────────┤                             ├───────────────────────────┤
            │   UNIFIED RING A (64/256) │                             │   UNIFIED RING B (64/256) │
            │ • NO_SQARRAY (Linear)     │                             │ • NO_SQARRAY (Linear)     │
            │ • DEFER_TASKRUN (No IPI)  │                             │ • SINGLE_ISSUER           │
            │ • NO_IOWAIT (Silent Fans) │                             │ • Fixed File Table (16)   │
            │ • REGISTER_CLOCK (Mono)   │                             │ • SUBMIT_TIMEOUT Park     │
            └─────────────┬─────────────┘                             └─────────────┬─────────────┘
                          │                                                         │
                          │                     Shared L3 Cache                     │
                          │          Sub-15ns Cross-Core IORING_OP_MSG_RING         │
                          │   Core 0 dispatches LoadReq ───────────────────────►    │
                          │   ◄─────────────────────── Core 1 delivers 64-bit BDA   │
                          │                                                         │
     ┌────────────────────┴────────────────────┐               ┌────────────────────┴────────────────────┐
     ▼                                         ▼               ▼                                         ▼
┌──────────────────────────┐      ┌─────────────────────────┐ ┌──────────────────────────────────────────────┐
│ WAYLAND WIRE CLIENT      │      │ VULKAN 1.4 ENGINE       │ │ NVMe DMA STREAMING (SK hynix SSD)        │
├──────────────────────────┤      ├─────────────────────────┤ ├──────────────────────────────────────────────┤
│ • IORING_OP_SOCKET (dir) │      │ • Dynamic Rendering     │ │ Atomic 4-SQE Hardware Linked Chain:          │
│ • IORING_OP_CONNECT (dir)│      │ • 64-bit BDA Vertex Pull│ │  1. OPENAT_DIRECT into fixed slot 0          │
│ • Multishot RECVMSG      │      │ • Timeline Semaphores   │ │  2. READ into host-visible VkDeviceMemory    │
│ • Lockless PBUF_RING     │      │ • 3x DMA-BUF Swapchain  │ │  3. CLOSE_DIRECT fixed slot 0                │
│ • Skip-Success Send Batch│      │ • Zero WSI Drivers      │ │  4. MSG_RING delivers BDA pointer to Ring A  │
└────────────┬─────────────┘      └────────────┬────────────┘ └──────────────────────────────────────────────┘
             │                                 │
             │           Wayland Unix Domain Socket ($XDG_RUNTIME_DIR/wayland-0)
             │           • xdg_wm_base Window Management (xdg_surface + xdg_toplevel)
             │           • zwp_linux_dmabuf_v1 (Zero-Copy Framebuffer Import)
             │           • wl_pointer & wl_keyboard Input Events (PBUF_RING)
             ▼                                 ▼
┌────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                   GNOME COMPOSITOR (Mutter Wayland Display)                                │
│                     Direct GPU Scanout via Linux DRM/KMS Kernel Mode Setting (/dev/dri/card0)              │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────┘