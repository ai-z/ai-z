#include <aiz/metrics/disk_bandwidth.h>

#if defined(_WIN32)

#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <windows.h>

#include <pdh.h>
#include <pdhmsg.h>

#include <cstdio>
#include <string>

namespace aiz {

DiskBandwidthCollector::DiskBandwidthCollector(std::string devicePrefix) : devicePrefix_(std::move(devicePrefix)) {}

std::optional<Sample> DiskBandwidthCollector::sample() {
  static bool initialized = false;
  static bool warmed = false;
  static HQUERY query = nullptr;
  static HCOUNTER counter = nullptr;

  if (!initialized) {
    initialized = true;

    const PDH_STATUS qs = PdhOpenQueryW(nullptr, 0, &query);
    if (qs != ERROR_SUCCESS) {
      query = nullptr;
      return std::nullopt;
    }

    // Prefer total disk bytes/sec across all physical disks.
    // Notes:
    // - On some systems/locales/counter sets this may not exist; we fall back below.
    const wchar_t* totalPath = L"\\PhysicalDisk(_Total)\\Disk Bytes/sec";
    PDH_STATUS cs = PdhAddEnglishCounterW(query, totalPath, 0, &counter);
    if (cs != ERROR_SUCCESS) {
      // Fallback: LogicalDisk is often present.
      const wchar_t* fallbackPath = L"\\LogicalDisk(_Total)\\Disk Bytes/sec";
      cs = PdhAddEnglishCounterW(query, fallbackPath, 0, &counter);
    }

    if (cs != ERROR_SUCCESS) {
      PdhCloseQuery(query);
      query = nullptr;
      counter = nullptr;
      return std::nullopt;
    }

    // First collect is required to initialize the counter.
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
      PdhCloseQuery(query);
      query = nullptr;
      counter = nullptr;
      return std::nullopt;
    }

    warmed = false;
    return Sample{0.0, "MB/s", "warming"};
  }

  if (query == nullptr || counter == nullptr) {
    return std::nullopt;
  }

  const PDH_STATUS collectStatus = PdhCollectQueryData(query);
  if (collectStatus != ERROR_SUCCESS) {
    return std::nullopt;
  }

  PDH_FMT_COUNTERVALUE value;
  DWORD counterType = 0;
  const PDH_STATUS fmtStatus = PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, &counterType, &value);
  if (fmtStatus != ERROR_SUCCESS || value.CStatus != ERROR_SUCCESS) {
    return std::nullopt;
  }

  // PDH returns instantaneous bytes/sec for this counter.
  const double bytesPerSec = value.doubleValue;
  const double mbps = bytesPerSec / (1024.0 * 1024.0);

  if (!warmed) {
    warmed = true;
    return Sample{0.0, "MB/s", "warming"};
  }

  // devicePrefix_ is ignored on Windows for now; we report total.
  return Sample{mbps, "MB/s", "all"};
}

}  // namespace aiz

#else

#include <cctype>
#include <fstream>
#include <sstream>

namespace aiz {

DiskBandwidthCollector::DiskBandwidthCollector(std::string devicePrefix) : devicePrefix_(std::move(devicePrefix)) {}

static bool readDiskBytesTotal(const std::string& devicePrefix, std::uint64_t& bytesOut) {
  // /proc/diskstats fields (Linux): reads completed, reads merged, sectors read, ms reading,
  // writes completed, writes merged, sectors written, ms writing, ...
  // We sum read+write sectors across matching devices.
  std::ifstream in("/proc/diskstats");
  if (!in.is_open()) return false;

  std::uint64_t sectors = 0;
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

    sectors += sectorsRead + sectorsWritten;
  }

  // Assume 512-byte sectors (common for diskstats). This is a known limitation.
  bytesOut = sectors * 512ULL;
  return true;
}

std::optional<Sample> DiskBandwidthCollector::sample() {
  std::uint64_t bytesTotal = 0;
  if (!readDiskBytesTotal(devicePrefix_, bytesTotal)) {
    return std::nullopt;
  }

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

#endif
