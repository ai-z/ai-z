#pragma once

#include <optional>
#include <string>

namespace aiz {

struct Sample {
  double value = 0.0;
  std::string unit;
  std::string label;
};

class ICollector {
public:
  virtual ~ICollector() = default;
  virtual std::string name() const = 0;
  // Return nullopt if unavailable.
  virtual std::optional<Sample> sample() = 0;
};

}  // namespace aiz
