#include <aiz/metrics/pcie_bandwidth.h>

#include <aiz/metrics/nvidia_nvml.h>

namespace aiz {

std::optional<Sample> PcieBandwidthCollector::sample() {
  const auto t = readNvmlPcieThroughput();
  if (!t) return std::nullopt;
  return Sample{t->rxMBps + t->txMBps, "MB/s", "nvml"};
}

std::optional<Sample> PcieRxBandwidthCollector::sample() {
  const auto t = readNvmlPcieThroughput();
  if (!t) return std::nullopt;
  return Sample{t->rxMBps, "MB/s", "nvml"};
}

std::optional<Sample> PcieTxBandwidthCollector::sample() {
  const auto t = readNvmlPcieThroughput();
  if (!t) return std::nullopt;
  return Sample{t->txMBps, "MB/s", "nvml"};
}

}  // namespace aiz
