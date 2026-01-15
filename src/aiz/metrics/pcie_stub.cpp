#include <aiz/metrics/pcie_bandwidth.h>

namespace aiz {

std::optional<Sample> PcieBandwidthCollector::sample() {
  // TODO: Implement throughput counters where available. Initial milestone will show link capability.
  return std::nullopt;
}

}  // namespace aiz
