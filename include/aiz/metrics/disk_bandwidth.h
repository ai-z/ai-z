#pragma once

#include <aiz/metrics/collectors.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace aiz {

enum class DiskBandwidthMode {
  Total,
  Read,
  Write,
};

class DiskBandwidthCollector final : public ICollector {
public:
  explicit DiskBandwidthCollector(std::string devicePrefix = "");
  DiskBandwidthCollector(DiskBandwidthMode mode, std::string devicePrefix = "");

  ~DiskBandwidthCollector() override;

  std::string name() const override { return "Disk bandwidth"; }
  std::optional<Sample> sample() override;

private:
  DiskBandwidthMode mode_ = DiskBandwidthMode::Total;
  std::string devicePrefix_;
  bool hasPrev_ = false;
  std::uint64_t prevBytes_ = 0;
  std::chrono::steady_clock::time_point prevTime_{};
};

}  // namespace aiz
