#pragma once

#include <aiz/metrics/ram_usage.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aiz::ncurses {

#if defined(_WIN32)
// Per-process GPU usage from Windows PDH counters (GPU Engine).
struct WindowsGpuProcessInfo {
  std::uint32_t pid = 0;
  double gpuUtilPct = 0.0;
  double vramUsedGiB = 0.0;
};

// Read per-process GPU utilization and VRAM usage from Windows PDH counters.
// Works for all GPU vendors (Intel, AMD, NVIDIA).
std::vector<WindowsGpuProcessInfo> readWindowsGpuProcessList();
#endif

struct GpuTelemetry {
  std::optional<double> utilPct;
  std::optional<double> memUtilPct;
  std::optional<double> vramUsedGiB;
  std::optional<double> vramTotalGiB;
  std::optional<double> watts;
  std::optional<double> tempC;
  std::string pstate;
  std::optional<unsigned int> gpuClockMHz;
  std::optional<unsigned int> memClockMHz;
  std::optional<double> encoderUtilPct;
  std::optional<double> decoderUtilPct;
  std::optional<unsigned int> pcieLinkWidth;
  std::optional<unsigned int> pcieLinkGen;
  // Short reason when PCIe link fields are unavailable (e.g. "ADLX missing").
  std::string pcieLinkNote;
  std::string source;
};

std::optional<GpuTelemetry> readGpuTelemetryPreferNvml(unsigned int index);

#if defined(_WIN32)
// Diagnostics for Windows PDH GPU memory counters.
std::string windowsPdhGpuMemoryDiagnostics();
#endif

std::string formatRamText(const std::optional<RamUsage>& ram);

}  // namespace aiz::ncurses
