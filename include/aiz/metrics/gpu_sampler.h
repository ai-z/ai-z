#pragma once

#include <optional>
#include <string>
#include <vector>
#include <aiz/metrics/linux_gpu_sysfs.h>

namespace aiz {

/// Unified GPU telemetry structure for Python bindings.
/// Aggregates data from NVML, ROCm SMI, and sysfs sources.
struct GpuTelemetry {
  unsigned int index = 0;
  std::string name;
  GpuVendor vendor = GpuVendor::Unknown;

  std::optional<double> utilPct;       // 0..100
  std::optional<double> vramUsedGiB;
  std::optional<double> vramTotalGiB;
  std::optional<double> powerWatts;
  std::optional<double> tempC;
  std::string pstate;
  std::string source;  // e.g. "nvml", "rocm-smi", "sysfs"

  // Clocks (optional)
  std::optional<unsigned int> gpuClockMHz;
  std::optional<unsigned int> memClockMHz;
};

/// Returns total GPU count across all backends (NVML + Linux sysfs).
unsigned int gpuCount();

/// Sample telemetry for a specific GPU by index.
/// Tries NVML first for NVIDIA GPUs, then ROCm SMI for AMD, then sysfs fallback.
/// Returns nullopt if the index is invalid or no data available.
std::optional<GpuTelemetry> sampleGpuTelemetry(unsigned int index);

/// Sample telemetry for all detected GPUs.
std::vector<GpuTelemetry> sampleAllGpuTelemetry();

}  // namespace aiz
