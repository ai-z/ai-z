// SPDX-License-Identifier: MIT
// AMD ROCm SMI stubs for Windows (ROCm SMI is Linux-only)

#include <aiz/metrics/amd_rocm_smi.h>

#if defined(AI_Z_PLATFORM_WINDOWS)

namespace aiz {

std::optional<unsigned int> rocmSmiGpuCount() {
  return std::nullopt;
}

std::optional<LinuxGpuTelemetry> readRocmSmiTelemetryForIndex(unsigned int) {
  return std::nullopt;
}

std::optional<LinuxGpuTelemetry> readRocmSmiTelemetryForPciBusId(const std::string&) {
  return std::nullopt;
}

}  // namespace aiz

#endif  // AI_Z_PLATFORM_WINDOWS
