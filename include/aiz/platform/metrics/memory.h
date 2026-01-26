#pragma once

#include <cstdint>
#include <optional>

namespace aiz::platform {

struct MemoryInfo {
  std::uint64_t totalBytes = 0;
  std::uint64_t availableBytes = 0;
};

std::optional<MemoryInfo> readMemoryInfo();

}  // namespace aiz::platform
