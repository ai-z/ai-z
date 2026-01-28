#pragma once

#include <optional>
#include <string>

#include <aiz/metrics/linux_gpu_sysfs.h>

namespace aiz {

// Returns the number of ROCm SMI-visible GPUs.
// Returns nullopt if ROCm SMI isn't present/usable.
std::optional<unsigned int> rocmSmiGpuCount();

// Reads telemetry for a specific ROCm SMI GPU index (best-effort).
// Returns nullopt if ROCm SMI isn't present/usable or index is invalid.
std::optional<LinuxGpuTelemetry> readRocmSmiTelemetryForIndex(unsigned int index);

// Reads telemetry for a GPU matching a PCI bus ID (e.g. "0000:03:00.0").
// Returns nullopt if ROCm SMI isn't present/usable or mapping fails.
std::optional<LinuxGpuTelemetry> readRocmSmiTelemetryForPciBusId(const std::string& pciBusId);

}  // namespace aiz
