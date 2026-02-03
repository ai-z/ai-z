#pragma once

#include <aiz/metrics/collectors.h>

#include <optional>
#include <random>

namespace aiz {

class RandomCpuUsageCollector final : public ICollector {
public:
  RandomCpuUsageCollector();

  std::string name() const override { return "CPU usage (debug)"; }
  std::optional<Sample> sample() override;

private:
  std::mt19937 rng_;
  std::normal_distribution<double> step_;
  double value_ = 0.0;
  bool initialized_ = false;
};

}  // namespace aiz
