#pragma once

#include <optional>
#include <string>

namespace aiz {

struct NvmlTelemetry {
  double gpuUtilPct = 0.0;     // 0..100
  double memUsedGiB = 0.0;
  double memTotalGiB = 0.0;
  double powerWatts = 0.0;
  double tempC = 0.0;
  std::string pstate;          // e.g. "P8"
};

// Returns the number of NVML-visible GPUs.
// Returns nullopt if NVML isn't present/usable.
std::optional<unsigned int> nvmlGpuCount();

// Reads telemetry for a specific GPU via NVML.
// Returns nullopt if NVML isn't present/usable or index is invalid.
std::optional<NvmlTelemetry> readNvmlTelemetryForGpu(unsigned int index);

// Reads aggregated telemetry across all GPUs via NVML.
// Aggregation rules:
// - gpuUtilPct: max utilization across GPUs
// - memUsed/Total: sum across GPUs
// - powerWatts: sum across GPUs
// - tempC: max across GPUs
// - pstate: best-performance state among GPUs (minimum P number)
// Returns nullopt if NVML isn't present/usable.
std::optional<NvmlTelemetry> readNvmlTelemetry();

}  // namespace aiz
