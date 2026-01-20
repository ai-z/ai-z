#include <aiz/metrics/disk_bandwidth.h>
#include <cctype>
#include <fstream>
#include <sstream>

namespace aiz {

DiskBandwidthCollector::DiskBandwidthCollector(std::string devicePrefix)
    : DiskBandwidthCollector(DiskBandwidthMode::Total, std::move(devicePrefix)) {}

DiskBandwidthCollector::DiskBandwidthCollector(DiskBandwidthMode mode, std::string devicePrefix)
    : mode_(mode), devicePrefix_(std::move(devicePrefix)) {}

DiskBandwidthCollector::~DiskBandwidthCollector() = default;

static bool readDiskSectorsTotals(const std::string& devicePrefix, std::uint64_t& readSectorsOut, std::uint64_t& writeSectorsOut) {
  // /proc/diskstats fields (Linux): reads completed, reads merged, sectors read, ms reading,
  // writes completed, writes merged, sectors written, ms writing, ...
  // We sum read+write sectors across matching devices.
  std::ifstream in("/proc/diskstats");
  if (!in.is_open()) return false;

  std::uint64_t readSectors = 0;
  std::uint64_t writeSectors = 0;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream iss(line);
    int major = 0, minor = 0;
    std::string dev;
    iss >> major >> minor >> dev;
    if (dev.empty()) continue;

    if (!devicePrefix.empty()) {
      if (dev.rfind(devicePrefix, 0) != 0) continue;
    } else {
      // Default: ignore partitions like sda1, nvme0n1p1. Keep base devices like sda, nvme0n1.
      const bool endsWithDigit = (!dev.empty() && std::isdigit(static_cast<unsigned char>(dev.back())));
      const bool isNvme = (dev.rfind("nvme", 0) == 0);
      if (isNvme) {
        // nvme base devices typically end in a digit (nvme0n1). Partitions include 'p' (nvme0n1p1).
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

  readSectorsOut = readSectors;
  writeSectorsOut = writeSectors;
  return true;
}

std::optional<Sample> DiskBandwidthCollector::sample() {
  std::uint64_t readSectors = 0;
  std::uint64_t writeSectors = 0;
  if (!readDiskSectorsTotals(devicePrefix_, readSectors, writeSectors)) {
    return std::nullopt;
  }

  std::uint64_t sectors = 0;
  switch (mode_) {
    case DiskBandwidthMode::Read:
      sectors = readSectors;
      break;
    case DiskBandwidthMode::Write:
      sectors = writeSectors;
      break;
    case DiskBandwidthMode::Total:
    default:
      sectors = readSectors + writeSectors;
      break;
  }

  // Assume 512-byte sectors (common for diskstats). This is a known limitation.
  const std::uint64_t bytesTotal = sectors * 512ULL;

  const auto now = std::chrono::steady_clock::now();
  if (!hasPrev_) {
    hasPrev_ = true;
    prevBytes_ = bytesTotal;
    prevTime_ = now;
    return Sample{0.0, "MB/s", "warming"};
  }

  const auto dt = std::chrono::duration_cast<std::chrono::duration<double>>(now - prevTime_).count();
  prevTime_ = now;

  const std::uint64_t dbytes = bytesTotal - prevBytes_;
  prevBytes_ = bytesTotal;

  if (dt <= 0.0) return Sample{0.0, "MB/s", ""};

  const double mbps = (static_cast<double>(dbytes) / (1024.0 * 1024.0)) / dt;
  return Sample{mbps, "MB/s", devicePrefix_.empty() ? "all" : devicePrefix_};
}

}  // namespace aiz
