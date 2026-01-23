#pragma once

#include <aiz/metrics/collectors.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace aiz {

class CpuUsageCollector final : public ICollector {
public:
  std::string name() const override { return "CPU usage"; }
  std::optional<Sample> sample() override;

private:
  bool hasPrev_ = false;
  std::uint64_t prevIdle_ = 0;
  std::uint64_t prevTotal_ = 0;
};

class CpuMaxCoreUsageCollector final : public ICollector {
public:
  std::string name() const override { return "CPU max-core usage"; }
  std::optional<Sample> sample() override;

private:
  bool hasPrev_ = false;
  std::vector<std::uint64_t> prevIdle_;
  std::vector<std::uint64_t> prevTotal_;
};

}  // namespace aiz
