# <img src="assets/logo.png" alt="AI-Z Logo" width="32" align="absmiddle"> AI-Z

[![CI](https://github.com/ai-z/ai-z/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/ai-z/ai-z/actions/workflows/ci.yml)
[![CI (Linux)](https://github.com/ai-z/ai-z/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/ai-z/ai-z/actions/workflows/ci-linux.yml)
[![CI (Windows)](https://github.com/ai-z/ai-z/actions/workflows/ci-windows.yml/badge.svg?branch=master)](https://github.com/ai-z/ai-z/actions/workflows/ci-windows.yml)

Terminal app for monitoring CPU/GPU performance and AI related benchmarks.

| GPU   | Linux | Windows |
|-------|:-----:|:-------:|
| NVIDIA | ✓ | ✓ |
| AMD | ✓ | ✓* |
| Intel | ✓ | ✓ |

\* PCIE bandwidth is missing

## Install
### Ubuntu/Debian
```shell
echo "deb [trusted=yes] https://www.ai-z.org/ stable main" | sudo tee /etc/apt/sources.list.d/ai-z.list
sudo apt update
sudo apt install ai-z
```

### Windows 10/11

Download either the portable ZIP or the installer from the latest GitHub Release:

https://github.com/ai-z/ai-z/releases/latest

Direct downloads (always point at the latest release):

- https://github.com/ai-z/ai-z/releases/latest/download/ai-z-windows-x64.zip
- https://github.com/ai-z/ai-z/releases/latest/download/ai-z-windows-x64-setup.exe

- Portable: unzip and run `ai-z.exe`
- Installer: run `ai-z-windows-x64-setup.exe`

## Build

### Windows

**Prerequisites:**
- Visual Studio 2019 or later (with C++ desktop development workload)
- CMake 3.16+
- vcpkg (for PDCurses dependency management)

**One-time setup:**

```powershell
# 1. Install vcpkg (if not already installed)
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat

# 2. (Optional) Add vcpkg to your environment for convenience
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\vcpkg', 'User')
```

**Build:**

```powershell
# Configure (tells CMake to use vcpkg for dependencies)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake

# Build
cmake --build build --config Release -j
```

**Run:**

```powershell
.\build\Release\ai-z.exe
```

The TUI application will launch in your terminal. Use:
- Arrow keys or vim-style keys to navigate
- `q` or `Ctrl+C` to quit
- Check the UI for additional keyboard shortcuts

**Alternative:** If you have vcpkg in a different location, adjust the path accordingly:
```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
```

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


