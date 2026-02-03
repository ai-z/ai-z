#include <aiz/platform/metrics/disk.h>

#include <cctype>
#include <fstream>
#include <sstream>

namespace aiz::platform {

std::optional<DiskCounters> readDiskCounters(const std::string& deviceFilter) {
  std::ifstream in("/proc/diskstats");
  if (!in.is_open()) return std::nullopt;

  std::uint64_t readSectors = 0;
  std::uint64_t writeSectors = 0;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream iss(line);
    int major = 0, minor = 0;
    std::string dev;
    iss >> major >> minor >> dev;
    if (dev.empty()) continue;

    if (!deviceFilter.empty()) {
      if (dev.rfind(deviceFilter, 0) != 0) continue;
    } else {
      const bool endsWithDigit = (!dev.empty() && std::isdigit(static_cast<unsigned char>(dev.back())));
      const bool isNvme = (dev.rfind("nvme", 0) == 0);
      if (isNvme) {
        if (endsWithDigit && dev.find('p') != std::string::npos) continue;
      } else {
        if (endsWithDigit) continue;
      }
    }

    std::uint64_t readsCompleted = 0, readsMerged = 0, sectorsRead = 0, msReading = 0;
    std::uint64_t writesCompleted = 0, writesMerged = 0, sectorsWritten = 0, msWriting = 0;
    iss >> readsCompleted >> readsMerged >> sectorsRead >> msReading;
    iss >> writesCompleted >> writesMerged >> sectorsWritten >> msWriting;

    readSectors += sectorsRead;
    writeSectors += sectorsWritten;
  }

  DiskCounters result;
  result.readBytes = readSectors * 512ULL;
  result.writeBytes = writeSectors * 512ULL;
  return result;
}

}  // namespace aiz::platform
