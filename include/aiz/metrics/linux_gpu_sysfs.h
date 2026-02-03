#pragma once

#include <optional>
#include <string>
#include <vector>

namespace aiz {

enum class GpuVendor {
  Unknown,
  Nvidia,
  Amd,
  Intel,
};

struct LinuxGpuDevice {
  unsigned int index = 0;          // stable index within our enumeration
  std::string drmCard;             // e.g. "card0"
  std::string sysfsDevicePath;     // e.g. "/sys/class/drm/card0/device"
  std::string pciSlotName;         // e.g. "0000:03:00.0" (from PCI_SLOT_NAME)
  GpuVendor vendor = GpuVendor::Unknown;
  std::string driver;             // e.g. "amdgpu", "i915", "xe", "nvidia"
};

struct LinuxGpuTelemetry {
  std::optional<double> utilPct;        // 0..100
  std::optional<double> vramUsedGiB;
  std::optional<double> vramTotalGiB;
  std::optional<double> watts;
  std::optional<double> tempC;
  std::string pstate;
  std::string source;                  // e.g. "amdgpu-sysfs", "i915-sysfs"
};

// Returns a best-effort enumeration of GPUs on Linux via /sys/class/drm.
// On non-Linux platforms, returns an empty list.
std::vector<LinuxGpuDevice> enumerateLinuxGpus();

// Convenience wrapper around enumerateLinuxGpus().size().
unsigned int linuxGpuCount();

// Reads best-effort telemetry for the GPU with the given index in enumerateLinuxGpus().
// Returns nullopt if sysfs data is unavailable.
std::optional<LinuxGpuTelemetry> readLinuxGpuTelemetry(unsigned int index);

}  // namespace aiz
