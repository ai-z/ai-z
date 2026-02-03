# AI-Z Copilot Instructions

## Project Overview
AI-Z is a cross-platform (Windows/Linux) C++20 terminal application for real-time monitoring of CPU/NPU/GPU performance timelines and AI-related benchmarks. It supports NVIDIA, AMD, and Intel GPUs with vendor-specific telemetry backends.

## Architecture

### Core Library + CLI Split
- **`aiz-core`** (static library): Platform-agnostic metrics, benchmarks, and hardware probing. Shared by CLI and Python bindings.
- **`ai-z`** (executable): TUI app using FTXUI, links `aiz-core`.
- **`python/`**: pybind11 bindings (`_aiz_core` module) linking `aiz-core` for Linux-only Python package.

### Key Directories
- `include/aiz/` — Public headers. Include `<aiz/aiz.h>` for the full API.
- `src/aiz/metrics/` — GPU/CPU/RAM/disk/network telemetry implementations
- `src/aiz/bench/` — Benchmark implementations (CUDA, OpenCL, Vulkan, ONNX Runtime)
- `src/aiz/dyn/` — Runtime-loaded APIs (CUDA, Vulkan, OpenCL, ONNX RT)
- `src/aiz/platform/{linux,windows}/` — OS-specific implementations
- `src/aiz/snapshot/` — JSON snapshot output for headless telemetry export
- `src/aiz/tui/` — FTXUI-based terminal UI components

### Dynamic Loading Pattern
GPU vendor APIs (CUDA, NVML, ROCm SMI, ADLX, IGCL, Vulkan, OpenCL, ONNX Runtime) are **runtime-loaded via dlopen/LoadLibrary**, not link-time dependencies. This allows building without SDK installations. See `src/aiz/dyn/*.cpp` for the pattern:
```cpp
// Example: aiz::dyn::cuda::api() returns nullptr if CUDA unavailable
if (auto* api = aiz::dyn::cuda::api()) {
    api->cuInit(0);
}
```

## Build Commands

### Windows (Visual Studio 2022)
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=.\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
.\build\Release\ai-z.exe
```

### Linux
```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build -j
./build/ai-z
```

### Run Tests
```bash
ctest --test-dir build -C Release --output-on-failure
```

### JSON Snapshot Output
```bash
# Single snapshot of all device telemetry
ai-z --snapshot

# Continuous loop (default 500ms refresh)
ai-z --snapshot --snapshot-loop

# Custom refresh interval (200ms)
ai-z --snapshot --snapshot-loop 200
```

## CMake Options
| Option | Default | Description |
|--------|---------|-------------|
| `AI_Z_ENABLE_NVML` | ON | NVIDIA Management Library backend |
| `AI_Z_ENABLE_CUDA` | ON | CUDA benchmarks |
| `AI_Z_ENABLE_VULKAN` | auto | Vulkan benchmarks (auto-detects SDK) |
| `AI_Z_ENABLE_OPENCL` | auto | OpenCL benchmarks (auto-detects SDK) |
| `AI_Z_BUILD_CLI` | ON | Build the TUI executable (needs FTXUI) |
| `AI_Z_ENABLE_WARNINGS` | OFF | Enable `-Wall -Wextra` / `/W4` |

## Code Conventions

### Platform Conditionals
Use compile-time defines, not runtime checks:
```cpp
#if defined(AI_Z_PLATFORM_WINDOWS)
  // Windows-specific code
#elif defined(AI_Z_PLATFORM_LINUX)
  // Linux-specific code
#endif
```

### Adding New Metrics
1. Create header in `include/aiz/metrics/` and implementation in `src/aiz/metrics/`
2. Add platform-specific variants under `src/aiz/platform/{linux,windows}/metrics/`
3. Add source files to `AI_Z_CORE_SOURCES` or `AI_Z_PLATFORM_SOURCES` in root `CMakeLists.txt`
4. Expose through `include/aiz/aiz.h` if part of public API

### Test Framework
Tests use a dual-backend system (Catch2 preferred, minitest fallback). Use the abstraction header:
```cpp
#include "test_framework.h"  // Wraps Catch2 or minitest
TEST_CASE("MyTest") { REQUIRE(condition); }
```

### GPU Telemetry Pattern
All GPU data flows through `GpuTelemetry` struct with `std::optional<>` for nullable metrics:
```cpp
struct GpuTelemetry {
    std::optional<double> utilPct;  // 0..100 or nullopt
    std::optional<double> tempC;
    std::string source;  // "nvml", "rocm-smi", "sysfs", "adlx", etc.
};
```

## Vendor SDK Integration

### Intel IGCL (Windows)
Place `igcl_api.h` + `cApiWrapper.cpp` in `third_party/igcl/` or set `-DAI_Z_IGCL_ROOT=...`

### AMD ADLX (Windows)
Set `-DAI_Z_ADLX_ROOT=...` pointing to ADLX SDK with `SDK/Include/`

### ONNX Runtime
Place headers in `third_party/onnxruntime/` or set `-DAI_Z_ONNXRUNTIME_ROOT=...`

## Versioning
Version is derived from git tags (`v0.1.17`) or falls back to `AI_Z_VERSION_FALLBACK` in CMakeLists.txt. Override with `-DAI_Z_VERSION_OVERRIDE=x.y.z`.
