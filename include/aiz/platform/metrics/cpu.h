#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace aiz::platform {

struct CpuTimes {
  std::uint64_t idle = 0;
  std::uint64_t total = 0;
};

std::optional<CpuTimes> readCpuTimes();
std::optional<std::vector<CpuTimes>> readPerCoreCpuTimes();

}  // namespace aiz::platform
