#include <aiz/metrics/cpu_random.h>

#include <algorithm>
#include <chrono>

namespace aiz {

RandomCpuUsageCollector::RandomCpuUsageCollector()
    : rng_(static_cast<std::mt19937::result_type>(
          std::chrono::high_resolution_clock::now().time_since_epoch().count())),
      step_(0.0, 12.0) {}

std::optional<Sample> RandomCpuUsageCollector::sample() {
  if (!initialized_) {
    std::uniform_real_distribution<double> initDist(0.0, 100.0);
    value_ = initDist(rng_);
    initialized_ = true;
  } else {
    value_ = std::clamp(value_ + step_(rng_), 0.0, 100.0);
  }

  return Sample{value_, "%", "debug"};
}

}  // namespace aiz
