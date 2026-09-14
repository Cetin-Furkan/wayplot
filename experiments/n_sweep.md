# Khoros N-Sweep Empirical Benchmark Receipt

- **Engine Version**: 0.3.0
- **Standard**: Pure ISO C23 (`__STDC_VERSION__ = 202311L`)
- **Hardware**: Intel Iris Xe Graphics (Tiger Lake-LP GT2)
- **OS / Kernel**: Linux 7.2.11-arch1-1 (x86_64, GNOME Mutter Wayland)
- **Vulkan API**: 1.4.303 (`VK_KHR_dynamic_rendering`, BDA, Multi-Draw Indirect Count)

## Empirical Results

| N | tick µs | LBVH µs | cull+draw GPU ns | draw_count | energy drift |
|---|---|---|---|---|---|
|   16 |    5.27 |    3.79 |           520772 |         15 |       0.7654 |
|  128 |   25.20 |   13.60 |           613333 |         73 |       0.8622 |
|  512 |  171.03 |   97.46 |           665315 |        292 |       0.9298 |
| 1024 |  470.85 |  302.56 |           460508 |        556 |       0.9332 |

*Recorded automatically via `make bench` / `build/bench_n`.*
