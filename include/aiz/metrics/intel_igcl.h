#pragma once

#include <optional>
#include <string>
#include <cstdint>

namespace aiz {

// Windows-only: identifies a DXGI adapter instance.
// Matches the Windows LUID layout (HighPart/LowPart).
struct IntelAdapterLuid {
  std::uint32_t lowPart = 0;
  std::int32_t highPart = 0;
};

// Best-effort identity for matching DXGI adapters to IGCL devices.
// These IDs come from DXGI (DXGI_ADAPTER_DESC1).
struct IntelAdapterPciIds {
  std::uint32_t vendorId = 0;
  std::uint32_t deviceId = 0;
  std::uint32_t revisionId = 0;
  std::uint32_t subSysId = 0;
};

struct IgclGpuTelemetry {
  std::optional<double> gpuUtilPct;
  std::optional<double> tempC;
  std::optional<double> powerW;
  std::optional<unsigned int> gpuClockMHz;
  std::optional<unsigned int> memClockMHz;

  // PCIe link info (via ctlPciGetState).
  std::optional<int> pcieLinkWidth;
  std::optional<int> pcieLinkGen;

  // Fan speed (RPM).
  std::optional<double> fanSpeedRpm;

  // VRAM bandwidth (MB/s).
  std::optional<double> vramReadBandwidthMBps;
  std::optional<double> vramWriteBandwidthMBps;

  // VRAM temperature (Celsius).
  std::optional<double> vramTempC;

  // Throttle state: empty if not throttled, otherwise indicates reason.
  // Possible values: "PWR" (power limited), "TMP" (thermal), "CUR" (current), "VLT" (voltage), "IDLE" (low utilization)
  std::string throttleState;
};

struct IgclAvailability {
  bool available = false;
  std::string backend;
  std::string dll;
};

enum class IgclStatus {
  Available,
  MissingDll,
  Unusable,
};

// Best-effort detection. On non-Windows this returns {false, "", ""}.
IgclAvailability igclAvailability();

// Short diagnostic string. Safe to call even if unavailable.
std::string igclDiagnostics();

// Detailed diagnostics (device list + supported telemetry fields). Safe to call even if unavailable.
std::string igclDiagnosticsDetailed();

// Best-effort status. On non-Windows returns MissingDll.
IgclStatus igclStatus();

// Windows-only: returns telemetry for Intel GPU matching the given DXGI LUID.
// When IGCL isn't present/usable or metrics are unavailable, returns nullopt.
std::optional<IgclGpuTelemetry> readIgclTelemetryForDxgi(const std::optional<IntelAdapterLuid>& adapterLuid);

// Windows-only: returns telemetry for Intel GPU matching the given PCI IDs.
// This is the preferred matching method for IGCL.
std::optional<IgclGpuTelemetry> readIgclTelemetryForPciIds(const IntelAdapterPciIds& ids);

}  // namespace aiz
