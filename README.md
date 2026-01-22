# ai-z

C++ TUI app for performance timelines (CPU/GPU/Disk/PCIe) and benchmarks.

Current platform support:
- Linux: ncurses TUI

## Build

### Linux

```bash
sudo apt-get install -y build-essential cmake libncurses-dev
cmake -S . -B build
cmake --build build -j
./build/ai-z
```

Optional (developer): enable extra compiler warnings:

```bash
cmake -S . -B build -DAI_Z_ENABLE_WARNINGS=ON
cmake --build build -j
```

CUDA benchmarks (Linux):

- Runtime prerequisite: an NVIDIA driver (for `libcuda.so` + GPU access).
- The CUDA Toolkit (`nvcc`, headers, libraries) is **not required** to build or run ai-z's current CUDA benchmarks.
  The project uses the CUDA **driver API** and JITs small **embedded PTX** kernels at runtime.

Notes:

- On machines without an NVIDIA driver/GPU, CUDA benchmarks will show as unavailable.
- The CUDA Toolkit is still useful for development (e.g. generating PTX/CUBIN with `nvcc`/`ptxas`, adding new kernels,
  or using CUDA profiling/debugging tools), but it is not an end-user dependency for the shipped benchmarks.

OpenCL benchmarks (PCIe bandwidth + FP32 FLOPS):

- Requires OpenCL headers + an ICD loader.
	- Ubuntu/Debian: `sudo apt-get install -y ocl-icd-opencl-dev`
- Auto-enabled by default when OpenCL dev packages are detected.
- Force-enable/disable with:

```bash
cmake -S . -B build -DAI_Z_ENABLE_OPENCL=ON
cmake --build build -j
```

Vulkan benchmarks (PCIe bandwidth + FP32 FLOPS):

- Requires Vulkan headers + loader.
	- Ubuntu/Debian: `sudo apt-get install -y libvulkan-dev`
- Auto-enabled by default when Vulkan dev packages are detected.
- Force-enable/disable with:

```bash
cmake -S . -B build -DAI_Z_ENABLE_VULKAN=ON
cmake --build build -j
```

ONNX Runtime benchmarks (CPU inference):

- Runtime-loaded via `dlopen` (no link-time dependency), so the Debian/Ubuntu package stays Launchpad/PPA-friendly.
- ai-z vendors the ONNX Runtime C API headers (MIT-licensed) to build the real MatMul benchmark without external dev packages.
- Optional: use a specific ONNX Runtime release header set via:
	`-DAI_Z_ONNXRUNTIME_ROOT=/path/to/onnxruntime-linux-x64-*/`

- Provides CPU benchmarks:
	- FP32 MatMul GFLOPS (real ORT execution when headers are available)
	- Memory bandwidth (GB/s)
- **Easy installation** (choose one method):

	**Option 1: Via pip** (easiest, auto-detected):
	```bash
	pip install onnxruntime
	# ai-z will automatically find it - no configuration needed!
	```

	**Option 2: System-wide** (for all users):
	```bash
	wget https://github.com/microsoft/onnxruntime/releases/download/v1.18.0/onnxruntime-linux-x64-1.18.0.tgz
	tar xzf onnxruntime-linux-x64-1.18.0.tgz
	sudo cp onnxruntime-linux-x64-1.18.0/lib/libonnxruntime.so* /usr/local/lib/
	sudo ldconfig
	```

	**Option 3: Custom location** (advanced):
	```bash
	export AI_Z_ONNXRUNTIME_PATH=/path/to/libonnxruntime.so
	```

- Benchmarks appear as "unavailable" if ONNX Runtime is not found
- **PPA-compatible**: The `.deb` package has zero ONNX Runtime dependencies—users can install it anytime

## Testing

Unit tests use Catch2 and are wired through CTest.

Ubuntu/Debian prereqs:

```bash
sudo apt-get install -y catch2
```

Configure + build + run tests:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Notes:
- By default the build does not fetch dependencies from the network.
- If Catch2 is not installed, you can point CMake at a local Catch2 checkout:
	`-DAI_Z_CATCH2_SOURCE_DIR=/path/to/Catch2`
- Or explicitly allow FetchContent (requires network access): `-DAI_Z_FETCH_CATCH2=ON`

## Debug

Run with synthetic/fake timelines (useful for debugging the timeline renderer):

```bash
./build/ai-z --debug
```

## Install (Debian/Ubuntu)

This project can produce a `.deb` package. Installing the package puts the binary at `/usr/bin/ai-z`, so users can run:

```bash
ai-z
```

### Build a `.deb`

Prereqs:

```bash
sudo apt-get install -y build-essential cmake dpkg-dev
```

Build + package:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cpack --config build/CPackConfig.cmake -G DEB
```

This produces something like `ai-z_0.1.0_amd64.deb` in the build directory.

### Install locally

For local installs (or downloading from a release page), users can install via apt:

```bash
sudo apt install ./ai-z_0.1.0_amd64.deb
```

### Making `sudo apt install ai-z` work

`sudo apt install ai-z` (without a local `.deb` path) requires that you publish the `.deb` into an APT repository users have added as a source (or publish to a Launchpad PPA / Debian/Ubuntu official repos).

Common options:

- Launchpad PPA (Ubuntu): users add the PPA, then `sudo apt install ai-z`.
- Host your own APT repo (Debian/Ubuntu): generate `Packages.gz`/`Release` metadata and publish via HTTPS.

## Timelines

Timeline graphs are rendered as vertical bars that scroll over time.

When there are more stored samples than visible terminal columns, the renderer downsamples by taking the **maximum value in each bucket** ("max in bucket") so short spikes remain visible.

You can change the aggregation mode via config:
- `timelineAgg=max` (default)
- `timelineAgg=avg`

## Keys
- `F1` or `H`: Help
- `F2` or `W`: Hardware info
- `F3` or `B`: Benchmarks
- `F4` or `C`: Config display
- `F5` or `T`: Timelines
- `F10` or `Q`: Quit

## Config

Config is stored at `~/.config/ai-z/config.ini` (or `$XDG_CONFIG_HOME/ai-z/config.ini`).

Keys:
- `showCpu`, `showGpu`, `showGpuMem`, `showRam`, `showVram` (true/false)
- `showDiskRead`, `showDiskWrite` (true/false)
- `showNetRx`, `showNetTx` (true/false)
- `showPcieRx`, `showPcieTx` (true/false)
- `refreshMs` (integer)
- `timelineSamples` (integer)
- `timelineAgg` (`max` or `avg`)
