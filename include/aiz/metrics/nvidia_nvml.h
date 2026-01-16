#pragma once

#include <optional>
#include <string>

namespace aiz {

struct NvmlTelemetry {
  double gpuUtilPct = 0.0;     // 0..100
  double memUtilPct = 0.0;     // 0..100 (memory controller utilization)
  double memUsedGiB = 0.0;
  double memTotalGiB = 0.0;
  double powerWatts = 0.0;
  double tempC = 0.0;
  std::string pstate;          // e.g. "P8"
};

struct NvmlPcieThroughput {
  // NVML reports PCIe throughput in KB/s. We return MB/s.
  double rxMBps = 0.0;
  double txMBps = 0.0;
};

struct NvmlPcieLink {
  // PCIe link width is lanes (e.g. 16 for x16).
  unsigned int width = 0;
  // PCIe generation as an integer (e.g. 4 for Gen4).
  unsigned int generation = 0;
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

// Reads PCIe throughput (RX/TX) for a specific GPU via NVML.
// Returns nullopt if NVML isn't present/usable or the driver doesn't expose the counter.
std::optional<NvmlPcieThroughput> readNvmlPcieThroughputForGpu(unsigned int index);

// Reads PCIe link information (current link width + generation) for a specific GPU via NVML.
// Returns nullopt if NVML isn't present/usable or the driver doesn't expose the fields.
std::optional<NvmlPcieLink> readNvmlPcieLinkForGpu(unsigned int index);

// Reads aggregated PCIe throughput (RX/TX) across all NVML-visible GPUs.
// Aggregation rules:
// - rxMBps/txMBps: sum across GPUs
// Returns nullopt if NVML isn't present/usable.
std::optional<NvmlPcieThroughput> readNvmlPcieThroughput();

}  // namespace aiz
