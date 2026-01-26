#pragma once

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <optional>

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

}  // namespace aiz

#endif  // _WIN32
