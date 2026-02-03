#pragma once

#include <aiz/metrics/collectors.h>

#include <optional>

namespace aiz {

class PcieBandwidthCollector final : public ICollector {
public:
  std::string name() const override { return "PCIe bandwidth"; }
  std::optional<Sample> sample() override;
};

class PcieRxBandwidthCollector final : public ICollector {
public:
  std::string name() const override { return "PCIe RX bandwidth"; }
  std::optional<Sample> sample() override;
};

class PcieTxBandwidthCollector final : public ICollector {
public:
  std::string name() const override { return "PCIe TX bandwidth"; }
  std::optional<Sample> sample() override;
};

}  // namespace aiz
