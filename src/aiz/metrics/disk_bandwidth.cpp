#include <aiz/metrics/disk_bandwidth.h>

#include <aiz/platform/metrics/disk.h>

namespace aiz {

DiskBandwidthCollector::DiskBandwidthCollector(std::string devicePrefix)
    : DiskBandwidthCollector(DiskBandwidthMode::Total, std::move(devicePrefix)) {}

DiskBandwidthCollector::DiskBandwidthCollector(DiskBandwidthMode mode, std::string devicePrefix)
    : mode_(mode), devicePrefix_(std::move(devicePrefix)) {}

DiskBandwidthCollector::~DiskBandwidthCollector() = default;

std::optional<Sample> DiskBandwidthCollector::sample() {
  const auto counters = platform::readDiskCounters(devicePrefix_);
  if (!counters) return std::nullopt;

  std::uint64_t bytesTotal = 0;
  switch (mode_) {
    case DiskBandwidthMode::Read:
      bytesTotal = counters->readBytes;
      break;
    case DiskBandwidthMode::Write:
      bytesTotal = counters->writeBytes;
      break;
    case DiskBandwidthMode::Total:
    default:
      bytesTotal = counters->readBytes + counters->writeBytes;
      break;
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
