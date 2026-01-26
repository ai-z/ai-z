# ai-z Windows 11 Porting Plan

This document outlines the strategy for porting ai-z to Windows 11 with minimal code duplication, while maintaining the existing Linux support and preparing for future AMD/Intel GPU backends.

## Table of Contents

1. [Overview](#overview)
2. [Architecture: Platform Abstraction Layer](#architecture-platform-abstraction-layer)
3. [Component Porting Details](#component-porting-details)
4. [GPU Backend Strategy](#gpu-backend-strategy)
5. [Build System Changes](#build-system-changes)
6. [Implementation Phases](#implementation-phases)
7. [Testing Strategy](#testing-strategy)
8. [Appendix: API Reference](#appendix-api-reference)

---

## Overview

### Goals

- **Minimal code duplication**: Shared business logic, platform-specific implementations behind clean interfaces
- **PDCurses for TUI**: Drop-in ncurses replacement requiring minimal UI code changes
- **Future-proof GPU support**: Architecture ready for AMD (ADL/ROCm) and Intel (Level Zero/oneAPI) backends
- **Graceful degradation**: Features unavailable on a platform should fail gracefully

### Current Linux Dependencies

| Component | Linux API | Windows Equivalent |
|-----------|-----------|-------------------|
| TUI | ncurses | PDCurses |
| Dynamic Loading | `dlopen`/`dlsym` | `LoadLibrary`/`GetProcAddress` |
| CPU Metrics | `/proc/stat` | `GetSystemTimes()` / PDH |
| RAM Metrics | `/proc/meminfo` | `GlobalMemoryStatusEx()` |
| Disk Metrics | `/proc/diskstats` | PDH / WMI |
| Network Metrics | `/proc/net/dev` | `GetIfTable2()` |
| Process List | `/proc/<pid>/*` | Tool Help API / `EnumProcesses()` |
| GPU Sysfs | `/sys/class/drm` | Vendor APIs (NVAPI, ADL, etc.) |
| NVML | `libnvml.so` + `fork()` | `nvml.dll` (direct calls) |
| Config Path | `$XDG_CONFIG_HOME` | `FOLDERID_RoamingAppData` |

---

## Architecture: Platform Abstraction Layer

### Directory Structure

```
include/aiz/platform/
├── platform.h              # Platform detection macros
├── dynlib.h                # Dynamic library loading interface
├── config_paths.h          # Config/data directory paths
├── process.h               # Process inspection interface
└── metrics/
    ├── cpu.h               # CPU usage metrics interface
    ├── memory.h            # RAM metrics interface
    ├── disk.h              # Disk I/O metrics interface
    └── network.h           # Network I/O metrics interface

src/aiz/platform/
├── linux/
│   ├── dynlib_linux.cpp
│   ├── config_paths_linux.cpp
│   ├── process_linux.cpp
│   └── metrics/
│       ├── cpu_linux.cpp
│       ├── memory_linux.cpp
│       ├── disk_linux.cpp
│       └── network_linux.cpp
└── windows/
    ├── dynlib_windows.cpp
    ├── config_paths_windows.cpp
    ├── process_windows.cpp
    └── metrics/
        ├── cpu_windows.cpp
        ├── memory_windows.cpp
        ├── disk_windows.cpp
        └── network_windows.cpp
```

### Core Interfaces

#### Dynamic Library Loading

```cpp
// include/aiz/platform/dynlib.h
#pragma once

#include <string>
#include <vector>

namespace aiz::platform {

class DynamicLibrary {
public:
    virtual ~DynamicLibrary() = default;
    virtual void* getSymbol(const char* name) = 0;
    virtual bool isValid() const = 0;

    template <typename T>
    bool loadSymbol(const char* name, T& fn) {
        fn = reinterpret_cast<T>(getSymbol(name));
        return fn != nullptr;
    }

    template <typename T>
    bool loadRequired(const char* name, T& fn, std::string& err) {
        if (!loadSymbol(name, fn)) {
            err = std::string("Missing symbol: ") + name;
            return false;
        }
        return true;
    }
};

// Factory function - returns nullptr on failure
// Searches candidates in order, returns first successful load
std::unique_ptr<DynamicLibrary> loadLibrary(
    const std::vector<const char*>& candidates,
    std::string* errorOut = nullptr
);

// Platform-specific library name helpers
const char* cudaLibraryName();      // "libcuda.so" or "nvcuda.dll"
const char* nvmlLibraryName();      // "libnvml.so" or "nvml.dll"
const char* openclLibraryName();    // "libOpenCL.so" or "OpenCL.dll"
const char* vulkanLibraryName();    // "libvulkan.so" or "vulkan-1.dll"
const char* onnxruntimeLibraryName(); // "libonnxruntime.so" or "onnxruntime.dll"

} // namespace aiz::platform
```

#### CPU Metrics

```cpp
// include/aiz/platform/metrics/cpu.h
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace aiz::platform {

struct CpuTimes {
    std::uint64_t idle = 0;
    std::uint64_t total = 0;
};

// Aggregate CPU times across all cores
std::optional<CpuTimes> readCpuTimes();

// Per-core CPU times (for max-core tracking)
std::optional<std::vector<CpuTimes>> readPerCoreCpuTimes();

} // namespace aiz::platform
```

#### Memory Metrics

```cpp
// include/aiz/platform/metrics/memory.h
#pragma once

#include <cstdint>
#include <optional>

namespace aiz::platform {

struct MemoryInfo {
    std::uint64_t totalBytes = 0;
    std::uint64_t availableBytes = 0;
};

std::optional<MemoryInfo> readMemoryInfo();

} // namespace aiz::platform
```

#### Disk Metrics

```cpp
// include/aiz/platform/metrics/disk.h
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace aiz::platform {

struct DiskCounters {
    std::uint64_t readBytes = 0;
    std::uint64_t writeBytes = 0;
};

// Aggregate disk I/O across all physical disks
// deviceFilter: empty = all disks, otherwise prefix filter (e.g., "sda", "C:")
std::optional<DiskCounters> readDiskCounters(const std::string& deviceFilter = "");

} // namespace aiz::platform
```

#### Network Metrics

```cpp
// include/aiz/platform/metrics/network.h
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace aiz::platform {

struct NetworkCounters {
    std::uint64_t rxBytes = 0;
    std::uint64_t txBytes = 0;
};

// Aggregate network I/O across all interfaces (excluding loopback)
// interfaceFilter: empty = all interfaces, otherwise prefix filter
std::optional<NetworkCounters> readNetworkCounters(const std::string& interfaceFilter = "");

} // namespace aiz::platform
```

#### Config Paths

```cpp
// include/aiz/platform/config_paths.h
#pragma once

#include <filesystem>

namespace aiz::platform {

// Returns platform-appropriate config directory
// Linux:   $XDG_CONFIG_HOME/ai-z or ~/.config/ai-z
// Windows: %APPDATA%/ai-z
std::filesystem::path configDirectory();

// Returns platform-appropriate data directory
// Linux:   $XDG_DATA_HOME/ai-z or ~/.local/share/ai-z
// Windows: %LOCALAPPDATA%/ai-z
std::filesystem::path dataDirectory();

} // namespace aiz::platform
```

#### Process Inspection

```cpp
// include/aiz/platform/process.h
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aiz::platform {

using ProcessId = std::uint32_t;

struct ProcessInfo {
    ProcessId pid = 0;
    std::string name;
    std::string cmdline;
    std::uint64_t cpuJiffies = 0;  // Platform-specific CPU time unit
    std::uint64_t memoryBytes = 0;
};

// Enumerate all user-owned processes
std::vector<ProcessInfo> enumerateUserProcesses();

// Read total CPU jiffies (for calculating percentages)
std::optional<std::uint64_t> readTotalCpuJiffies();

// Check if a process is owned by current user
bool isUserProcess(ProcessId pid);

} // namespace aiz::platform
```

---

## Component Porting Details

### 1. TUI: PDCurses (Option A)

PDCurses is a drop-in ncurses replacement for Windows. Minimal code changes required.

#### Setup

```cmake
# CMakeLists.txt
if (WIN32)
    # PDCurses can be built from source or installed via vcpkg
    find_package(PDCurses REQUIRED)
    target_link_libraries(ai-z PRIVATE PDCurses::PDCurses)
    target_compile_definitions(ai-z PRIVATE PDC_WIDE=1)
else()
    set(CURSES_NEED_NCURSES TRUE)
    set(CURSES_NEED_WIDE TRUE)
    find_package(Curses REQUIRED)
    target_link_libraries(ai-z PRIVATE ${CURSES_LIBRARIES})
endif()
```

#### Code Changes

Most ncurses code works unchanged. Minor adjustments:

```cpp
// src/aiz/tui/ncurses_ui.cpp

// Add platform-specific include
#ifdef _WIN32
#include <curses.h>  // PDCurses
#else
#include <curses.h>  // ncurses
#endif

// PDCurses doesn't have set_escdelay
#if defined(NCURSES_VERSION)
  set_escdelay(1);
#elif defined(PDC_BUILD)
  // PDCurses handles escape differently, no action needed
#endif
```

#### PDCurses Installation

**vcpkg (Recommended):**
```powershell
vcpkg install pdcurses:x64-windows
```

**Manual Build:**
```powershell
git clone https://github.com/wmcbrine/PDCurses.git
cd PDCurses/wincon
nmake -f Makefile.vc WIDE=Y UTF8=Y
```

### 2. Dynamic Library Loading

#### Linux Implementation (existing, refactored)

```cpp
// src/aiz/platform/linux/dynlib_linux.cpp
#include <aiz/platform/dynlib.h>
#include <dlfcn.h>

namespace aiz::platform {

class LinuxDynamicLibrary : public DynamicLibrary {
public:
    explicit LinuxDynamicLibrary(void* handle) : handle_(handle) {}
    
    ~LinuxDynamicLibrary() override {
        if (handle_) dlclose(handle_);
    }
    
    void* getSymbol(const char* name) override {
        dlerror(); // Clear
        return dlsym(handle_, name);
    }
    
    bool isValid() const override { return handle_ != nullptr; }
    
private:
    void* handle_ = nullptr;
};

std::unique_ptr<DynamicLibrary> loadLibrary(
    const std::vector<const char*>& candidates,
    std::string* errorOut
) {
    for (const char* name : candidates) {
        void* handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
        if (handle) {
            return std::make_unique<LinuxDynamicLibrary>(handle);
        }
    }
    if (errorOut) {
        *errorOut = dlerror() ? dlerror() : "Library not found";
    }
    return nullptr;
}

const char* cudaLibraryName() { return "libcuda.so.1"; }
const char* nvmlLibraryName() { return "libnvml.so.1"; }
const char* openclLibraryName() { return "libOpenCL.so.1"; }
const char* vulkanLibraryName() { return "libvulkan.so.1"; }
const char* onnxruntimeLibraryName() { return "libonnxruntime.so"; }

} // namespace aiz::platform
```

#### Windows Implementation

```cpp
// src/aiz/platform/windows/dynlib_windows.cpp
#include <aiz/platform/dynlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace aiz::platform {

class WindowsDynamicLibrary : public DynamicLibrary {
public:
    explicit WindowsDynamicLibrary(HMODULE handle) : handle_(handle) {}
    
    ~WindowsDynamicLibrary() override {
        if (handle_) FreeLibrary(handle_);
    }
    
    void* getSymbol(const char* name) override {
        return reinterpret_cast<void*>(GetProcAddress(handle_, name));
    }
    
    bool isValid() const override { return handle_ != nullptr; }
    
private:
    HMODULE handle_ = nullptr;
};

std::unique_ptr<DynamicLibrary> loadLibrary(
    const std::vector<const char*>& candidates,
    std::string* errorOut
) {
    for (const char* name : candidates) {
        HMODULE handle = LoadLibraryA(name);
        if (handle) {
            return std::make_unique<WindowsDynamicLibrary>(handle);
        }
    }
    if (errorOut) {
        DWORD err = GetLastError();
        char buf[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, 0, buf, sizeof(buf), nullptr);
        *errorOut = buf;
    }
    return nullptr;
}

const char* cudaLibraryName() { return "nvcuda.dll"; }
const char* nvmlLibraryName() { return "nvml.dll"; }
const char* openclLibraryName() { return "OpenCL.dll"; }
const char* vulkanLibraryName() { return "vulkan-1.dll"; }
const char* onnxruntimeLibraryName() { return "onnxruntime.dll"; }

} // namespace aiz::platform
```

### 3. CPU Metrics

#### Windows Implementation

```cpp
// src/aiz/platform/windows/metrics/cpu_windows.cpp
#include <aiz/platform/metrics/cpu.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace aiz::platform {

static std::uint64_t fileTimeToU64(const FILETIME& ft) {
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::optional<CpuTimes> readCpuTimes() {
    FILETIME idleTime, kernelTime, userTime;
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        return std::nullopt;
    }
    
    const std::uint64_t idle = fileTimeToU64(idleTime);
    const std::uint64_t kernel = fileTimeToU64(kernelTime);
    const std::uint64_t user = fileTimeToU64(userTime);
    
    // Note: kernel time includes idle time on Windows
    return CpuTimes{idle, kernel + user};
}

std::optional<std::vector<CpuTimes>> readPerCoreCpuTimes() {
    // Use PDH (Performance Data Helper) for per-core metrics
    // This requires more complex setup - see Appendix
    
    // Simplified: fallback to aggregate
    auto agg = readCpuTimes();
    if (!agg) return std::nullopt;
    
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    const DWORD numCores = sysInfo.dwNumberOfProcessors;
    
    // Return aggregate divided by cores (approximation)
    // Full implementation would use NtQuerySystemInformation or PDH
    std::vector<CpuTimes> result(numCores, *agg);
    return result;
}

} // namespace aiz::platform
```

### 4. Memory Metrics

#### Windows Implementation

```cpp
// src/aiz/platform/windows/metrics/memory_windows.cpp
#include <aiz/platform/metrics/memory.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace aiz::platform {

std::optional<MemoryInfo> readMemoryInfo() {
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    
    if (!GlobalMemoryStatusEx(&mem)) {
        return std::nullopt;
    }
    
    return MemoryInfo{
        mem.ullTotalPhys,
        mem.ullAvailPhys
    };
}

} // namespace aiz::platform
```

### 5. Disk Metrics

#### Windows Implementation

```cpp
// src/aiz/platform/windows/metrics/disk_windows.cpp
#include <aiz/platform/metrics/disk.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>

namespace aiz::platform {

std::optional<DiskCounters> readDiskCounters(const std::string& deviceFilter) {
    // Use DeviceIoControl with IOCTL_DISK_PERFORMANCE
    // Or PDH counters: \PhysicalDisk(*)\Disk Read Bytes/sec
    
    DiskCounters result{};
    
    // Enumerate physical drives
    for (int i = 0; i < 16; ++i) {
        std::string path = "\\\\.\\PhysicalDrive" + std::to_string(i);
        
        HANDLE hDevice = CreateFileA(
            path.c_str(),
            0,  // No access needed for IOCTL_DISK_PERFORMANCE
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        
        if (hDevice == INVALID_HANDLE_VALUE) continue;
        
        DISK_PERFORMANCE perf{};
        DWORD bytesReturned = 0;
        
        if (DeviceIoControl(
                hDevice,
                IOCTL_DISK_PERFORMANCE,
                nullptr, 0,
                &perf, sizeof(perf),
                &bytesReturned,
                nullptr)) {
            result.readBytes += perf.BytesRead.QuadPart;
            result.writeBytes += perf.BytesWritten.QuadPart;
        }
        
        CloseHandle(hDevice);
    }
    
    return result;
}

} // namespace aiz::platform
```

### 6. Network Metrics

#### Windows Implementation

```cpp
// src/aiz/platform/windows/metrics/network_windows.cpp
#include <aiz/platform/metrics/network.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")

namespace aiz::platform {

std::optional<NetworkCounters> readNetworkCounters(const std::string& interfaceFilter) {
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR) {
        return std::nullopt;
    }
    
    NetworkCounters result{};
    
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        
        // Skip loopback
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        
        // Skip disconnected interfaces
        if (row.OperStatus != IfOperStatusUp) continue;
        
        result.rxBytes += row.InOctets;
        result.txBytes += row.OutOctets;
    }
    
    FreeMibTable(table);
    return result;
}

} // namespace aiz::platform
```

### 7. NVML on Windows

On Windows, NVML is generally more stable and doesn't require the `fork()` timeout protection used on Linux.

```cpp
// src/aiz/metrics/nvidia_nvml.cpp

#ifdef _WIN32
// Windows: Direct NVML calls (no fork needed)
template <typename MsgT, typename Fn>
static std::optional<MsgT> callWithTimeout(Fn&& fn, std::chrono::milliseconds timeout) {
    // Option 1: Just call directly (NVML is stable on Windows)
    MsgT msg{};
    fn(msg);
    return msg;
    
    // Option 2: Use std::async with timeout if paranoid
    // auto future = std::async(std::launch::async, [&]() { ... });
    // if (future.wait_for(timeout) == std::future_status::ready) { ... }
}
#else
// Linux: Keep existing fork() implementation
// ... existing code ...
#endif
```

### 8. Hardware Info Probing

#### Windows-Specific Probes

```cpp
// src/aiz/hw/hardware_info_windows.cpp (new file)
#ifdef _WIN32

#include <aiz/hw/hardware_info.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

namespace aiz {

static std::string probeOsPrettyNameWindows() {
    // Use RtlGetVersion or registry for version info
    OSVERSIONINFOEXW osvi{};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    
    // Note: GetVersionEx is deprecated; use RtlGetVersion via ntdll
    // For simplicity, use registry
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, 
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t productName[256]{};
        DWORD size = sizeof(productName);
        RegQueryValueExW(hKey, L"ProductName", nullptr, nullptr,
            reinterpret_cast<LPBYTE>(productName), &size);
        RegCloseKey(hKey);
        
        // Convert to UTF-8
        int len = WideCharToMultiByte(CP_UTF8, 0, productName, -1, nullptr, 0, nullptr, nullptr);
        std::string result(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, productName, -1, result.data(), len, nullptr, nullptr);
        return result;
    }
    return "Windows";
}

static std::string probeCpuNameWindows() {
    int cpuInfo[4]{};
    char brand[49]{};
    
    __cpuid(cpuInfo, 0x80000000);
    if (static_cast<unsigned int>(cpuInfo[0]) >= 0x80000004) {
        __cpuid(reinterpret_cast<int*>(brand), 0x80000002);
        __cpuid(reinterpret_cast<int*>(brand + 16), 0x80000003);
        __cpuid(reinterpret_cast<int*>(brand + 32), 0x80000004);
        return std::string(brand);
    }
    return "Unknown CPU";
}

static std::string probeCpuLogicalCoresWindows() {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return std::to_string(sysInfo.dwNumberOfProcessors);
}

static std::string probeCpuPhysicalCoresWindows() {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    
    std::vector<char> buffer(length);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) {
        return "--";
    }
    
    DWORD count = 0;
    char* ptr = buffer.data();
    while (ptr < buffer.data() + length) {
        auto* current = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(ptr);
        if (current->Relationship == RelationProcessorCore) {
            ++count;
        }
        ptr += current->Size;
    }
    
    return std::to_string(count);
}

} // namespace aiz

#endif // _WIN32
```

---

## GPU Backend Strategy

### Current State: NVIDIA Only

- CUDA via `libcuda.so` / `nvcuda.dll` (driver API)
- NVML for telemetry
- Both already use dynamic loading

### Future: AMD GPU Support

**Linux:**
- ROCm/HIP for compute benchmarks
- `librocm_smi64.so` for telemetry (similar to NVML)
- `/sys/class/drm/cardN` sysfs fallback (already partially implemented)

**Windows:**
- ADL (AMD Display Library) for telemetry
- HIP via `amdhip64.dll` for compute
- AGS (AMD GPU Services) for advanced features

### Future: Intel GPU Support

**Linux:**
- Level Zero (`libze_loader.so`) for compute
- `/sys/class/drm` sysfs for telemetry
- oneAPI for advanced features

**Windows:**
- Level Zero (`ze_loader.dll`)
- Intel GPU Tools / IGC for telemetry

### Recommended Abstraction

```cpp
// include/aiz/gpu/gpu_backend.h
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aiz::gpu {

enum class Vendor { Unknown, Nvidia, Amd, Intel };

struct GpuInfo {
    unsigned int index = 0;
    Vendor vendor = Vendor::Unknown;
    std::string name;
    std::uint64_t memoryBytes = 0;
};

struct GpuTelemetry {
    double utilizationPct = 0.0;
    double memoryUsedGiB = 0.0;
    double memoryTotalGiB = 0.0;
    double temperatureC = 0.0;
    double powerWatts = 0.0;
    std::uint32_t gpuClockMHz = 0;
    std::uint32_t memClockMHz = 0;
};

class IGpuBackend {
public:
    virtual ~IGpuBackend() = default;
    virtual Vendor vendor() const = 0;
    virtual bool isAvailable() const = 0;
    virtual std::vector<GpuInfo> enumerateGpus() = 0;
    virtual std::optional<GpuTelemetry> readTelemetry(unsigned int gpuIndex) = 0;
};

// Factory - returns all available backends
std::vector<std::unique_ptr<IGpuBackend>> createGpuBackends();

} // namespace aiz::gpu
```

---

## Build System Changes

### CMakeLists.txt Updates

```cmake
cmake_minimum_required(VERSION 3.16)
project(ai-z VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform check
if (NOT CMAKE_SYSTEM_NAME MATCHES "Linux|Windows")
    message(FATAL_ERROR "ai-z supports Linux and Windows only")
endif()

# Platform detection
set(AI_Z_PLATFORM_LINUX OFF)
set(AI_Z_PLATFORM_WINDOWS OFF)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(AI_Z_PLATFORM_LINUX ON)
elseif (WIN32)
    set(AI_Z_PLATFORM_WINDOWS ON)
endif()

# Common sources
set(AI_Z_COMMON_SOURCES
    src/main.cpp
    src/aiz/app.cpp
    src/aiz/i18n.cpp
    src/aiz/config/config.cpp
    src/aiz/dyn/cuda.cpp
    src/aiz/dyn/opencl.cpp
    src/aiz/dyn/vulkan.cpp
    src/aiz/dyn/onnxruntime.cpp
    src/aiz/hw/hardware_info.cpp
    src/aiz/metrics/timeline.cpp
    src/aiz/metrics/gpu_memory_util.cpp
    src/aiz/metrics/gpu_stub.cpp
    src/aiz/metrics/pcie_stub.cpp
    src/aiz/bench/bench_stub.cpp
    # ... other shared sources
)

# Platform-specific sources
if (AI_Z_PLATFORM_LINUX)
    set(AI_Z_PLATFORM_SOURCES
        src/aiz/platform/linux/dynlib_linux.cpp
        src/aiz/platform/linux/config_paths_linux.cpp
        src/aiz/platform/linux/process_linux.cpp
        src/aiz/platform/linux/metrics/cpu_linux.cpp
        src/aiz/platform/linux/metrics/memory_linux.cpp
        src/aiz/platform/linux/metrics/disk_linux.cpp
        src/aiz/platform/linux/metrics/network_linux.cpp
        src/aiz/metrics/linux_gpu_sysfs.cpp
    )
elseif (AI_Z_PLATFORM_WINDOWS)
    set(AI_Z_PLATFORM_SOURCES
        src/aiz/platform/windows/dynlib_windows.cpp
        src/aiz/platform/windows/config_paths_windows.cpp
        src/aiz/platform/windows/process_windows.cpp
        src/aiz/platform/windows/metrics/cpu_windows.cpp
        src/aiz/platform/windows/metrics/memory_windows.cpp
        src/aiz/platform/windows/metrics/disk_windows.cpp
        src/aiz/platform/windows/metrics/network_windows.cpp
        src/aiz/hw/hardware_info_windows.cpp
    )
endif()

add_executable(ai-z ${AI_Z_COMMON_SOURCES} ${AI_Z_PLATFORM_SOURCES})

# TUI library
if (AI_Z_PLATFORM_LINUX)
    set(CURSES_NEED_NCURSES TRUE)
    set(CURSES_NEED_WIDE TRUE)
    find_package(Curses REQUIRED)
    find_library(AI_Z_NCURSESW_LIBRARY NAMES ncursesw)
    if (AI_Z_NCURSESW_LIBRARY)
        target_link_libraries(ai-z PRIVATE ${AI_Z_NCURSESW_LIBRARY})
    else()
        target_link_libraries(ai-z PRIVATE ${CURSES_LIBRARIES})
    endif()
    target_include_directories(ai-z PRIVATE ${CURSES_INCLUDE_DIR})
    target_link_libraries(ai-z PRIVATE dl)
elseif (AI_Z_PLATFORM_WINDOWS)
    # PDCurses via vcpkg or manual
    find_package(unofficial-pdcurses CONFIG QUIET)
    if (unofficial-pdcurses_FOUND)
        target_link_libraries(ai-z PRIVATE unofficial::pdcurses::pdcurses)
    else()
        find_library(PDCURSES_LIBRARY NAMES pdcurses)
        find_path(PDCURSES_INCLUDE_DIR curses.h)
        if (PDCURSES_LIBRARY AND PDCURSES_INCLUDE_DIR)
            target_link_libraries(ai-z PRIVATE ${PDCURSES_LIBRARY})
            target_include_directories(ai-z PRIVATE ${PDCURSES_INCLUDE_DIR})
        else()
            message(FATAL_ERROR "PDCurses not found. Install via vcpkg or set PDCURSES_LIBRARY/PDCURSES_INCLUDE_DIR")
        endif()
    endif()
    target_compile_definitions(ai-z PRIVATE PDC_WIDE=1)
    target_link_libraries(ai-z PRIVATE iphlpapi pdh)
endif()

# Platform compile definitions
target_compile_definitions(ai-z PRIVATE
    $<$<BOOL:${AI_Z_PLATFORM_LINUX}>:AI_Z_PLATFORM_LINUX>
    $<$<BOOL:${AI_Z_PLATFORM_WINDOWS}>:AI_Z_PLATFORM_WINDOWS>
)
```

### vcpkg Integration (Windows)

Create `vcpkg.json`:

```json
{
    "name": "ai-z",
    "version": "0.1.0",
    "dependencies": [
        {
            "name": "pdcurses",
            "features": ["wide"]
        }
    ]
}
```

---

## Implementation Phases

### Phase 1: Foundation (Week 1-2)

**Goal:** Build on Windows, basic TUI working

| Task | Effort | Priority |
|------|--------|----------|
| Create platform abstraction headers | 1 day | P0 |
| Implement `dynlib_windows.cpp` | 1 day | P0 |
| Implement `config_paths_windows.cpp` | 0.5 days | P0 |
| Add PDCurses to build | 1 day | P0 |
| Fix TUI for PDCurses compatibility | 1 day | P0 |
| Basic Windows CMake setup | 1 day | P0 |
| CI: Add Windows build | 1 day | P1 |

**Deliverable:** ai-z compiles and runs on Windows with empty/stub metrics

### Phase 2: Core Metrics (Week 3-4)

**Goal:** Real-time monitoring works on Windows

| Task | Effort | Priority |
|------|--------|----------|
| Implement `cpu_windows.cpp` | 1 day | P0 |
| Implement `memory_windows.cpp` | 0.5 days | P0 |
| Implement `disk_windows.cpp` | 2 days | P0 |
| Implement `network_windows.cpp` | 1 day | P0 |
| Implement `process_windows.cpp` | 2 days | P1 |
| Port NVML (remove fork) | 1 day | P0 |
| Test CUDA/OpenCL/Vulkan loading | 1 day | P0 |

**Deliverable:** Timelines work for CPU, RAM, Disk, Network on Windows

### Phase 3: Hardware Probing (Week 5)

**Goal:** System Info tab works on Windows

| Task | Effort | Priority |
|------|--------|----------|
| Implement `hardware_info_windows.cpp` | 3 days | P1 |
| OS version detection | 0.5 days | P1 |
| CPU info via CPUID | 1 day | P1 |
| GPU info via NVML/vendor APIs | 1 day | P1 |
| Disk/NIC enumeration | 1 day | P2 |

**Deliverable:** System Info shows accurate Windows hardware details

### Phase 4: Polish & Release (Week 6)

**Goal:** Production-ready Windows build

| Task | Effort | Priority |
|------|--------|----------|
| Benchmarks testing on Windows | 2 days | P1 |
| Per-core CPU metrics (PDH) | 1 day | P2 |
| Windows installer (WiX/NSIS) | 1 day | P2 |
| Documentation updates | 1 day | P1 |
| Release testing | 1 day | P0 |

**Deliverable:** Windows release build with installer

---

## Testing Strategy

### Unit Tests

Existing tests should work cross-platform (they test logic, not platform code).

### Integration Tests

Add platform-specific test fixtures:

```cpp
// tests/platform/test_metrics.cpp
#include <aiz/platform/metrics/cpu.h>
#include <aiz/platform/metrics/memory.h>

TEST_CASE("CPU metrics are available") {
    auto times = aiz::platform::readCpuTimes();
    REQUIRE(times.has_value());
    REQUIRE(times->total > 0);
}

TEST_CASE("Memory metrics are available") {
    auto mem = aiz::platform::readMemoryInfo();
    REQUIRE(mem.has_value());
    REQUIRE(mem->totalBytes > 0);
    REQUIRE(mem->availableBytes <= mem->totalBytes);
}
```

### CI Configuration

```yaml
# .github/workflows/build.yml
name: Build

on: [push, pull_request]

jobs:
  linux:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install dependencies
        run: sudo apt-get install -y build-essential cmake libncurses-dev
      - name: Build
        run: |
          cmake -S . -B build
          cmake --build build -j
      - name: Test
        run: ctest --test-dir build --output-on-failure

  windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - name: Setup vcpkg
        uses: lukka/run-vcpkg@v11
        with:
          vcpkgGitCommitId: # pin to specific commit
      - name: Build
        run: |
          cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
          cmake --build build --config Release
      - name: Test
        run: ctest --test-dir build -C Release --output-on-failure
```

---

## Appendix: API Reference

### Windows API Functions Used

| Function | Header | Library | Purpose |
|----------|--------|---------|---------|
| `LoadLibraryA` | `windows.h` | kernel32 | Dynamic library loading |
| `GetProcAddress` | `windows.h` | kernel32 | Symbol lookup |
| `GetSystemTimes` | `windows.h` | kernel32 | CPU usage |
| `GlobalMemoryStatusEx` | `windows.h` | kernel32 | RAM usage |
| `DeviceIoControl` | `windows.h` | kernel32 | Disk performance |
| `GetIfTable2` | `iphlpapi.h` | iphlpapi | Network stats |
| `CreateToolhelp32Snapshot` | `tlhelp32.h` | kernel32 | Process list |
| `SHGetKnownFolderPath` | `shlobj.h` | shell32 | Config paths |
| `GetLogicalProcessorInformationEx` | `windows.h` | kernel32 | CPU topology |

### PDCurses Differences from ncurses

| Feature | ncurses | PDCurses | Notes |
|---------|---------|----------|-------|
| `set_escdelay()` | ✅ | ❌ | Not needed on Windows |
| `use_default_colors()` | ✅ | ✅ | Works |
| Wide character support | ncursesw | `PDC_WIDE` | Compile-time flag |
| Mouse support | ✅ | ✅ | Different event format |
| Resize handling | `KEY_RESIZE` | `KEY_RESIZE` | Same |

### Library Names by Platform

| Backend | Linux | Windows |
|---------|-------|---------|
| CUDA Driver | `libcuda.so.1`, `libcuda.so` | `nvcuda.dll` |
| NVML | `libnvml.so.1`, `libnvml.so` | `nvml.dll` |
| OpenCL | `libOpenCL.so.1`, `libOpenCL.so` | `OpenCL.dll` |
| Vulkan | `libvulkan.so.1`, `libvulkan.so` | `vulkan-1.dll` |
| ONNX Runtime | `libonnxruntime.so`, `libonnxruntime.so.1` | `onnxruntime.dll` |
| AMD ROCm SMI | `librocm_smi64.so` | N/A (use ADL) |
| AMD ADL | N/A | `atiadlxx.dll` |
| Intel Level Zero | `libze_loader.so` | `ze_loader.dll` |
