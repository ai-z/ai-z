#pragma once

#include <aiz/metrics/collectors.h>

#include <optional>
#include <string>

namespace aiz {

struct TelemetrySnapshot {
  std::optional<Sample> cpu;
  std::optional<Sample> disk;
  std::optional<Sample> gpu;
  std::optional<Sample> pcie;

  // Pre-formatted memory strings are okay for now; we can refactor to typed later.
  std::string ramText;
};

}  // namespace aiz
