#pragma once

#include <aiz/metrics/collectors.h>

#include <optional>

namespace aiz {

class GpuMemoryUtilCollector final : public ICollector {
public:
  std::string name() const override { return "GPU memory utilization"; }
  std::optional<Sample> sample() override;
};

}  // namespace aiz
