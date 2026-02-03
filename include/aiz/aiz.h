// SPDX-License-Identifier: MIT
// aiz-core - Main header for AI-Z metrics library
//
// Include this single header to access all public aiz-core APIs.
// For more granular control, include individual headers instead.

#pragma once

// Version info
#include <aiz/version.h>

// Metrics - vendor-agnostic sampling APIs
#include <aiz/metrics/gpu_sampler.h>      // Unified GPU telemetry (all vendors)
#include <aiz/metrics/cpu_usage.h>        // CPU usage collectors
#include <aiz/metrics/ram_usage.h>        // RAM usage sampling
#include <aiz/metrics/disk_bandwidth.h>   // Disk I/O bandwidth
#include <aiz/metrics/network_bandwidth.h> // Network I/O bandwidth
#include <aiz/metrics/collectors.h>       // Base collector types

// Hardware info - system probing
#include <aiz/hw/hardware_info.h>         // HardwareInfo::probe()

// Snapshot - JSON telemetry output
#include <aiz/snapshot/snapshot.h>        // captureSystemSnapshot(), snapshotToJson()

// Vendor-specific (optional, for advanced use)
#include <aiz/metrics/nvidia_nvml.h>      // NVIDIA NVML direct access
#include <aiz/metrics/amd_rocm_smi.h>     // AMD ROCm SMI direct access
#include <aiz/metrics/linux_gpu_sysfs.h>  // Linux sysfs GPU enumeration

#if defined(_WIN32)
#include <aiz/metrics/intel_igcl.h>       // Intel IGCL (Windows)
#include <aiz/metrics/amd_adlx.h>         // AMD ADLX (Windows)
#include <aiz/metrics/windows_d3dkmt.h>   // Windows D3DKMT
#endif
