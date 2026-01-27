#pragma once

#include <aiz/metrics/ram_usage.h>

#include <optional>
#include <string>

namespace aiz::ncurses {

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
