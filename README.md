# ai-z

C++ TUI app for performance timelines (CPU/GPU/Disk/PCIe) and benchmarks.

Current platform support:
- Linux: ncurses TUI
- Windows: minimal console UI (press `q` to quit)
- macOS: planned (not implemented yet)

## Build

### Linux

```bash
sudo apt-get install -y build-essential cmake libncurses-dev
cmake -S . -B build
cmake --build build -j
./build/ai-z
```

NVIDIA PCIe bandwidth benchmark (CUDA):

CUDA prerequisites (Linux):

- You need both:
	- An NVIDIA driver (for running on the GPU). Quick check: `nvidia-smi`
	- The CUDA Toolkit (for building CUDA code: `nvcc`, headers, libraries). Quick check: `nvcc --version`
- Install CUDA Toolkit:
	- Ubuntu/Debian (often older): `sudo apt-get install -y nvidia-cuda-toolkit`
	- Or install the CUDA Toolkit from NVIDIA's repo/installer (typically ends up in `/usr/local/cuda` or `/usr/local/cuda-12.x`)

Build notes:

- If CMake cannot find `CUDAToolkit`, this project will automatically disable CUDA (even if `-DAI_Z_ENABLE_CUDA=ON`).
- After installing CUDA, reconfigure from scratch:

```bash
rm -rf build
cmake -S . -B build -DAI_Z_ENABLE_CUDA=ON
cmake --build build -j
```

- If CMake still can't find it, point it at your CUDA install:

```bash
cmake -S . -B build -DAI_Z_ENABLE_CUDA=ON -DCUDAToolkit_ROOT=/usr/local/cuda
```

- Enabled by default if a CUDA Toolkit is found.
- Force enable/disable with `-DAI_Z_ENABLE_CUDA=ON/OFF`.

```bash
cmake -S . -B build -DAI_Z_ENABLE_CUDA=ON
cmake --build build -j
```

OpenCL benchmarks (PCIe bandwidth + FP32 FLOPS):

- Requires OpenCL headers + an ICD loader.
	- Ubuntu/Debian: `sudo apt-get install -y ocl-icd-opencl-dev`
- Enable with:

```bash
cmake -S . -B build -DAI_Z_ENABLE_OPENCL=ON
cmake --build build -j
```

Vulkan benchmarks (PCIe bandwidth + FP32 FLOPS):

- Requires Vulkan headers + loader.
	- Ubuntu/Debian: `sudo apt-get install -y libvulkan-dev`
- Enable with:

```bash
cmake -S . -B build -DAI_Z_ENABLE_VULKAN=ON
cmake --build build -j
```

### Windows

Prereqs:
- Visual Studio 2022 (or Build Tools) with C++ workload
- CMake (usually bundled with Visual Studio)

PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Release
./build/Release/ai-z.exe
```

## Debug

Run with synthetic/fake timelines (useful for debugging the timeline renderer):

```bash
./build/ai-z --debug
```

On Windows:

```powershell
./build/Release/ai-z.exe --debug
```

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
- `F10` or `Q`: Quit

Windows console UI:
- `q`: Quit

## Config

Config is stored at `~/.config/ai-z/config.ini` (or `$XDG_CONFIG_HOME/ai-z/config.ini`).

Keys:
- `showCpu`, `showGpu`, `showDisk`, `showPcieRx`, `showPcieTx`, `showRam`, `showVram` (true/false)
- `refreshMs` (integer)
- `timelineSamples` (integer)
- `timelineAgg` (`max` or `avg`)
