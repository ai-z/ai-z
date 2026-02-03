#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace aiz::platform {

struct DiskCounters {
  std::uint64_t readBytes = 0;
  std::uint64_t writeBytes = 0;
};

std::optional<DiskCounters> readDiskCounters(const std::string& deviceFilter = "");

}  // namespace aiz::platform
