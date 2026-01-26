#include <aiz/metrics/ram_usage.h>

#include <aiz/platform/metrics/memory.h>

namespace aiz {

std::optional<double> readRamTotalGiB() {
  const auto mem = platform::readMemoryInfo();
  if (!mem) return std::nullopt;
  const double totalGiB = static_cast<double>(mem->totalBytes) / (1024.0 * 1024.0 * 1024.0);
  return totalGiB;
}

std::optional<RamUsage> readRamUsage() {
  const auto mem = platform::readMemoryInfo();
  if (!mem) return std::nullopt;

  const double totalGiB = static_cast<double>(mem->totalBytes) / (1024.0 * 1024.0 * 1024.0);
  const double availGiB = static_cast<double>(mem->availableBytes) / (1024.0 * 1024.0 * 1024.0);
  const double usedGiB = (totalGiB >= availGiB) ? (totalGiB - availGiB) : 0.0;
  const double usedPct = (totalGiB > 0.0) ? (100.0 * usedGiB / totalGiB) : 0.0;

  return RamUsage{usedGiB, totalGiB, usedPct};
}

}  // namespace aiz
