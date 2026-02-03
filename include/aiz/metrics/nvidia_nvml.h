#pragma once

#include <optional>
#include <string>
#include <vector>

namespace aiz {

struct NvmlTelemetry {
  double gpuUtilPct = 0.0;     // 0..100
  double memUtilPct = 0.0;     // 0..100 (memory controller utilization)
  double memUsedGiB = 0.0;
  double memTotalGiB = 0.0;
  double powerWatts = 0.0;
  double tempC = 0.0;
  std::string pstate;          // e.g. "P8"

  // Encoder/decoder utilization (0..100). -1 indicates unavailable.
  double encoderUtilPct = -1.0;
  double decoderUtilPct = -1.0;

  // Best-effort extra metadata (not always available on all drivers/devices).
  unsigned int gpuClockMHz = 0;  // current graphics clock
  unsigned int memClockMHz = 0;  // current memory clock
  // Memory transfer rate (MT/s) is the spec-style way memory speed is often expressed.
  // When available, this may be more comparable to vendor specs than memClockMHz.
  unsigned int memTransferRateMHz = 0;
  unsigned int smMajor = 0;      // CUDA compute capability major
  unsigned int smMinor = 0;      // CUDA compute capability minor
  unsigned int multiprocessorCount = 0;  // number of SMs (streaming multiprocessors)

  // Power and bandwidth (best-effort).
  double maxPowerLimitWatts = 0.0;
  unsigned int memBusWidthBits = 0;
  double maxMemBandwidthGBps = 0.0;
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

struct NvmlProcessInfo {
  unsigned int pid = 0;
  unsigned int gpuIndex = 0;
  double vramUsedGiB = 0.0;
  std::optional<double> gpuUtilPct;
};

// Returns the number of NVML-visible GPUs.
// Returns nullopt if NVML isn't present/usable.
std::optional<unsigned int> nvmlGpuCount();

// In-process NVML queries (no fork/timeout wrapper). These are intended for
// lightweight, best-effort UI metadata like device names where avoiding fork
// in a multi-threaded process matters more than guarding against rare driver hangs.
std::optional<unsigned int> nvmlGpuCountNoFork();

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

// Reads the GPU device name for a specific GPU via NVML.
// Returns nullopt if NVML isn't present/usable or index is invalid.
std::optional<std::string> readNvmlGpuNameForGpu(unsigned int index);

// In-process NVML query (no fork/timeout wrapper).
std::optional<std::string> readNvmlGpuNameForGpuNoFork(unsigned int index);

// Reads aggregated PCIe throughput (RX/TX) across all NVML-visible GPUs.
// Aggregation rules:
// - rxMBps/txMBps: sum across GPUs
// Returns nullopt if NVML isn't present/usable.
std::optional<NvmlPcieThroughput> readNvmlPcieThroughput();

// Reads the NVML library version string (e.g. "12.555.43").
// Returns nullopt if NVML isn't present/usable or the symbol isn't available.
std::optional<std::string> readNvmlLibraryVersion();

// Reads the NVIDIA driver version string via NVML (e.g. "550.54.14").
// Returns nullopt if NVML isn't present/usable or the symbol isn't available.
std::optional<std::string> readNvmlDriverVersion();

// Reads per-process GPU usage (best-effort). Each entry is tagged with a GPU index.
// Returns an empty vector if NVML isn't present/usable or no data is available.
std::vector<NvmlProcessInfo> readNvmlProcessInfo();

}  // namespace aiz
