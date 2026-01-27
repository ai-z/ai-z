#pragma once

#if defined(_WIN32)

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

// Best-effort: queries video memory information for a DXGI adapter via D3DKMT.
// Returns nullopt if D3DKMT is unavailable or the query fails.
std::optional<D3dkmtVideoMemoryInfo> queryD3dkmtLocalVideoMemoryForLuid(const LUID& luid);

// Diagnostics: list adapters and their D3DKMT local memory stats.
std::string d3dkmtDiagnostics();

}  // namespace aiz

#endif  // _WIN32
