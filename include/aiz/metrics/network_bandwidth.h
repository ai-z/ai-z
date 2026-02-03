#pragma once

#include <aiz/metrics/collectors.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace aiz {

enum class NetworkBandwidthMode {
  Rx,
  Tx,
};

class NetworkBandwidthCollector final : public ICollector {
public:
  explicit NetworkBandwidthCollector(NetworkBandwidthMode mode, std::string ifacePrefix = "");
  ~NetworkBandwidthCollector() override;

  std::string name() const override { return "Network bandwidth"; }
  std::optional<Sample> sample() override;

private:
  NetworkBandwidthMode mode_;
  std::string ifacePrefix_;

  bool hasPrev_ = false;
  std::uint64_t prevBytes_ = 0;
  std::chrono::steady_clock::time_point prevTime_{};
};

}  // namespace aiz
