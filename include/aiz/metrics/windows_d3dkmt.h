#pragma once

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

namespace aiz {

struct D3dkmtVideoMemoryInfo {
  std::uint64_t budgetBytes = 0;
  std::uint64_t currentUsageBytes = 0;
  std::uint64_t availableForReservationBytes = 0;
  std::uint64_t currentReservationBytes = 0;
};

// Adapter performance data from D3DKMT (works for any GPU vendor).
struct D3dkmtAdapterPerfData {
  double temperatureC = 0.0;                // GPU temperature in Celsius
  std::optional<double> powerWatts;         // Power draw in watts (if available)
  std::uint32_t fanRpm = 0;                 // Fan speed in RPM
  std::uint64_t memoryFrequencyHz = 0;      // Memory clock frequency
  std::uint64_t maxMemoryFrequencyHz = 0;   // Max memory clock frequency
  std::uint64_t memoryBandwidthBytes = 0;   // Memory bandwidth used
  std::uint64_t pcieBandwidthBytes = 0;     // PCIe bandwidth used
  std::uint8_t powerStateOverride = 0;      // 1 if GPU powered on
};

// Best-effort: queries video memory information for a DXGI adapter via D3DKMT.
// Returns nullopt if D3DKMT is unavailable or the query fails.
std::optional<D3dkmtVideoMemoryInfo> queryD3dkmtLocalVideoMemoryForLuid(const LUID& luid);

// Best-effort: queries adapter performance data (temp, power, fan) via D3DKMT.
// Returns nullopt if D3DKMT is unavailable or the query fails.
std::optional<D3dkmtAdapterPerfData> queryD3dkmtAdapterPerfData(const LUID& luid);

// PCIe link info from Windows SetupAPI (works for any GPU vendor).
struct WinPcieLinkInfo {
  int currentLinkSpeed = 0;   // PCIe generation (1=2.5GT/s, 2=5GT/s, 3=8GT/s, 4=16GT/s, 5=32GT/s)
  int currentLinkWidth = 0;   // Number of lanes (e.g., 16)
  int maxLinkSpeed = 0;       // Max supported generation
  int maxLinkWidth = 0;       // Max supported lanes
};

// Best-effort: queries PCIe link info via Windows SetupAPI for a DXGI adapter.
// Returns nullopt if the query fails or info is unavailable.
std::optional<WinPcieLinkInfo> queryWinPcieLinkInfo(const LUID& luid);

// Diagnostics: list adapters and their D3DKMT local memory stats.
std::string d3dkmtDiagnostics();

}  // namespace aiz

#endif  // _WIN32
