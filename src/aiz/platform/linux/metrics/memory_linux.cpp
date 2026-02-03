#include <aiz/platform/metrics/memory.h>

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace aiz::platform {

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

std::optional<MemoryInfo> readMemoryInfo() {
  const auto totalKb = readMeminfoFieldKb("MemTotal:");
  const auto availKb = readMeminfoFieldKb("MemAvailable:");
  if (!totalKb || !availKb) return std::nullopt;

  MemoryInfo info;
  info.totalBytes = (*totalKb) * 1024ULL;
  info.availableBytes = (*availKb) * 1024ULL;
  return info;
}

}  // namespace aiz::platform
