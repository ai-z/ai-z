#include <aiz/metrics/ram_usage.h>

#include <cstdint>

#include <fstream>
#include <sstream>

namespace aiz {

static std::optional<std::uint64_t> readMeminfoFieldKb(const char* key) {
  std::ifstream in("/proc/meminfo");
  if (!in.is_open()) return std::nullopt;

  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(key, 0) != 0) continue;
    std::istringstream iss(line);
    std::string k;
    std::uint64_t v = 0;
    iss >> k >> v;
    if (v == 0) return std::nullopt;
    return v;
  }
  return std::nullopt;
}

std::optional<double> readRamTotalGiB() {
  const auto totalKb = readMeminfoFieldKb("MemTotal:");
  if (!totalKb) return std::nullopt;
  const double totalGiB = static_cast<double>(*totalKb) / (1024.0 * 1024.0);
  return totalGiB;
}

std::optional<RamUsage> readRamUsage() {
  const auto totalKb = readMeminfoFieldKb("MemTotal:");
  const auto availKb = readMeminfoFieldKb("MemAvailable:");
  if (!totalKb || !availKb) return std::nullopt;

  const double totalGiB = static_cast<double>(*totalKb) / (1024.0 * 1024.0);
  const double availGiB = static_cast<double>(*availKb) / (1024.0 * 1024.0);
  const double usedGiB = (totalGiB >= availGiB) ? (totalGiB - availGiB) : 0.0;
  const double usedPct = (totalGiB > 0.0) ? (100.0 * usedGiB / totalGiB) : 0.0;

  return RamUsage{usedGiB, totalGiB, usedPct};
}

}  // namespace aiz
