#include <aiz/metrics/network_bandwidth.h>

#include <aiz/platform/metrics/network.h>

namespace aiz {

NetworkBandwidthCollector::NetworkBandwidthCollector(NetworkBandwidthMode mode, std::string ifacePrefix)
    : mode_(mode), ifacePrefix_(std::move(ifacePrefix)) {}

NetworkBandwidthCollector::~NetworkBandwidthCollector() = default;

std::optional<Sample> NetworkBandwidthCollector::sample() {
  const auto counters = platform::readNetworkCounters(ifacePrefix_);
  if (!counters) return std::nullopt;

  const std::uint64_t bytesTotal = (mode_ == NetworkBandwidthMode::Rx)
      ? counters->rxBytes
      : counters->txBytes;

  const auto now = std::chrono::steady_clock::now();
  if (!hasPrev_) {
    hasPrev_ = true;
    prevBytes_ = bytesTotal;
    prevTime_ = now;
    return Sample{0.0, "MB/s", ""};
  }

  const auto dt = std::chrono::duration_cast<std::chrono::duration<double>>(now - prevTime_).count();
  prevTime_ = now;

  const std::uint64_t dbytes = bytesTotal - prevBytes_;
  prevBytes_ = bytesTotal;

  if (dt <= 0.0) return Sample{0.0, "MB/s", ""};

  const double mbps = (static_cast<double>(dbytes) / (1024.0 * 1024.0)) / dt;
  return Sample{mbps, "MB/s", ifacePrefix_.empty() ? "" : ifacePrefix_};
}

}  // namespace aiz
