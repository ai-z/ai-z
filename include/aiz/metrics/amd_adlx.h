#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace aiz {

struct AmdPciLocation {
  std::uint32_t bus = 0;
  std::uint32_t device = 0;
  std::uint32_t function = 0;
};

// Windows-only: identifies a DXGI adapter instance.
// Matches the Windows LUID layout (HighPart/LowPart).
struct AmdAdapterLuid {
  std::uint32_t lowPart = 0;
  std::int32_t highPart = 0;
};

struct AmdPcieLink {
  unsigned int width = 0;         // lanes (e.g. 16 for x16)
  unsigned int generation = 0;    // PCIe gen (e.g. 4 for Gen4)
  unsigned int maxWidth = 0;
  unsigned int maxGeneration = 0;
};

struct AdlxGpuTelemetry {
  std::optional<double> gpuUtilPct;
  std::optional<double> tempC;
  std::optional<double> powerW;
  std::optional<unsigned int> gpuClockMHz;
  std::optional<unsigned int> memClockMHz;
  std::optional<double> vramUsedGiB;
  std::optional<double> vramTotalGiB;
};

struct AdlxAvailability {
  bool available = false;
  // Human-readable backend hint, e.g. "ADLX".
  std::string backend;
  // Loaded DLL name if known (e.g. "amdadlx64.dll").
  std::string dll;
};

enum class AdlxStatus {
  Available,
  MissingDll,   // amdadlx*.dll not present
  Unusable,     // present but init failed or missing required exports
};

// Windows-only: checks whether AMD ADLX can be loaded/initialized.
// On non-Windows platforms this returns {false, "", ""}.
AdlxAvailability adlxAvailability();

// Returns a multi-line human-readable diagnostics string for ADLX.
// Safe to call even if ADLX isn't present.
std::string adlxDiagnostics();

// Windows-only: prints PCIe link diagnostics using DXGI adapter enumeration.
// This uses ADLX for AMD PCIe link width/gen.
std::string pcieDiagnostics();

// Windows-only: returns a short user-facing reason string when PCIe link info is
// unavailable for an AMD GPU. Returns empty string when no note is available.
std::string amdPcieLinkNoteForDxgi(const std::optional<AmdAdapterLuid>& adapterLuid);

// Best-effort ADLX status (Windows-only). On non-Windows this returns MissingDll.
AdlxStatus adlxStatus();

// Windows-only: runtime-loads AMD ADLX (amdadlx*.dll) and queries PCIe link info.
// Returns nullopt when ADLX isn't present/usable.
std::optional<AmdPcieLink> readAdlxPcieLinkForDxgi(const std::optional<AmdAdapterLuid>& adapterLuid);

// Windows-only: queries current AMD GPU telemetry via ADLX for the given DXGI LUID.
// Returns nullopt when ADLX isn't present/usable or metrics are unavailable.
std::optional<AdlxGpuTelemetry> readAdlxTelemetryForDxgi(const std::optional<AmdAdapterLuid>& adapterLuid);

}  // namespace aiz
