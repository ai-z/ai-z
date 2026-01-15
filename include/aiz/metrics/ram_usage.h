#pragma once

#include <optional>

namespace aiz {

struct RamUsage {
  double usedGiB = 0.0;
  double totalGiB = 0.0;
  double usedPct = 0.0;
};

std::optional<RamUsage> readRamUsage();
std::optional<double> readRamTotalGiB();

}  // namespace aiz
