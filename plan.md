# ai-z: Implementation Plan (Linux C++ TUI)

## Goal
Build a Linux C++ command-line/TUI app that:
- Shows **configurable performance timelines**:
  - CPU usage
  - GPU usage (NVIDIA / AMD / Intel)
  - Disk bandwidth usage
  - PCIe bandwidth usage
- Provides bottom-of-screen options:
  1. benchmarks
  2. config display
  3. exit
- Runs benchmarks:
  - PCIe bandwidth test (NVIDIA / AMD / Intel)
  - Floating-point operations per second (FLOPS)
  - “PyTorch matrix multiplication test” implemented **in C++ only** (no Python). Closest match is **LibTorch** (PyTorch C++ API); alternatives are vendor BLAS backends.

## Non-Goals (initial implementation)
- No Python tooling, scripts, or runtime dependencies.
- No extra UX beyond the three bottom options and the timelines view.
- No network features.

## UX / Interaction

### Main Screen (Timelines)
- Displays a set of timeline panels for the enabled metrics.
- Refresh interval: default 250ms–1000ms (configurable).
- Timeline window: last N samples (e.g., 120 samples).

### Bottom Options
- `F1` / `1`: **benchmarks**
- `F2` / `2`: **config display**
- `F3` / `3` or `q`: **exit**

### Benchmarks Screen
- Lists the three benchmarks.
- Runs selected benchmark and shows results.
- Behavior when backend not available: show a clear message (e.g., “Built without CUDA backend”).

### Config Display Screen
- Shows which timelines are enabled.
- Allows toggling each metric on/off.
- Saves config.

## Architecture

### Modules
- `tui/`: ncurses rendering, input, screen routing (timelines / benchmarks / config).
- `metrics/`: common interfaces and metric collectors.
- `bench/`: benchmark runner interfaces and per-backend implementations.
- `config/`: load/save config (simple key=value format).
- `platform/`: small utilities (time, file reading, etc.).

### Interfaces
- `IMetricCollector`: produces a `double` sample and a unit/label.
- `TimelineBuffer`: fixed-size ring buffer for samples.
- `IBenchmark`: exposes `name()`, `isAvailable()`, `run()`.

## Metrics Collection Plan

### CPU Usage
- Source: `/proc/stat`.
- Compute % busy from deltas of idle vs total.

### Disk Bandwidth
- Source: `/proc/diskstats`.
- Compute bytes/s from sectors read/written deltas * sector_size (assume 512 unless detected).

### GPU Usage (Vendor Backends)
- Common API: `GpuBackend` with `probe()` and `sample()`.
- NVIDIA:
  - Preferred: NVML (optional, runtime dynamic load).
- AMD:
  - Preferred: sysfs / ROCm SMI if present (optional).
- Intel:
  - Preferred: Level Zero / sysfs where available (optional).
- Initial milestone: implement **stub** that reports “unavailable” if no backend.

### PCIe Bandwidth Usage
- Two concepts:
  - **Capability** (link width/speed): sysfs (`/sys/bus/pci/devices/.../current_link_width`, `current_link_speed`).
  - **Throughput** (bytes/s): hardware counters (often requires perf events / privileged access / vendor APIs).
- Initial milestone: show capability and a placeholder for throughput; later implement throughput via perf/vendor where feasible.

## Benchmarks Plan (C++ Only)

### PCIe Bandwidth Test (NVIDIA/AMD/Intel)
- Approach: device↔host transfers of a large buffer, timed with vendor timing primitives.
  - NVIDIA: CUDA `cudaMemcpyAsync` + CUDA events.
  - AMD: HIP `hipMemcpyAsync` + HIP events.
  - Intel: oneAPI/SYCL USM copies + event timing (if chosen).
- Output: GB/s (H→D, D→H).

### FLOPS Test
- CPU: tight compute kernel (FMA-heavy loop) with `std::chrono` and pinned affinity optional.
- GPU variants: use CUDA/HIP/SYCL kernels where applicable.

### “PyTorch Matrix Multiplication” (No Python)
- Preferred: **LibTorch (PyTorch C++ API)**:
  - CPU build for universal support.
  - CUDA build for NVIDIA.
  - ROCm build for AMD.
- Intel GPU path may be better served by SYCL/oneMKL GEMM instead of assuming a universally-available LibTorch Intel GPU build.

## Build & Packaging

### Build System
- CMake.
- Dependencies (initial): `ncurses`.
- Optional dependencies behind flags:
  - `AI_Z_ENABLE_NVML`
  - `AI_Z_ENABLE_CUDA`
  - `AI_Z_ENABLE_HIP`
  - `AI_Z_ENABLE_LIBTORCH`
  - `AI_Z_ENABLE_LEVEL_ZERO`
  - `AI_Z_ENABLE_SYCL`

### apt install ai-z
- Provide Debian packaging skeleton (debian/).
- Strategy:
  - Base package: ncurses-only, CPU/Disk metrics, benchmark stubs.
  - Optional packages or build variants for GPU stacks (CUDA/ROCm/oneAPI) and LibTorch to avoid huge mandatory deps.

## Milestones

### Milestone 1: Running TUI + CPU/Disk timelines
- App builds and runs on Linux.
- Timelines show real CPU usage and disk bandwidth.
- Config toggles work.

### Milestone 2: GPU usage backends
- Implement at least one backend (NVML first), keep others stubbed but structured.

### Milestone 3: Benchmarks (backend gated)
- CPU FLOPS benchmark.
- PCIe bandwidth for CUDA/HIP when enabled.
- LibTorch matmul when enabled.

### Milestone 4: Debian packaging
- `dpkg-buildpackage` produces installable `.deb`.
