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

- Enabled by default if a CUDA Toolkit is found.
- Force enable/disable with `-DAI_Z_ENABLE_CUDA=ON/OFF`.

```bash
cmake -S . -B build -DAI_Z_ENABLE_CUDA=ON
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
- `showCpu`, `showGpu`, `showDisk`, `showPcie` (true/false)
- `refreshMs` (integer)
- `timelineSamples` (integer)
