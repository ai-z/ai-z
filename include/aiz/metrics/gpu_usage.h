#pragma once

#include <aiz/metrics/collectors.h>

#include <optional>

namespace aiz {

class GpuUsageCollector final : public ICollector {
public:
  std::string name() const override { return "GPU usage"; }
  std::optional<Sample> sample() override;
};

}  // namespace aiz
