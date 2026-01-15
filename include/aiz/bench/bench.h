#pragma once

#include <string>

namespace aiz {

struct BenchResult {
  bool ok = false;
  std::string summary;
};

class IBenchmark {
public:
  virtual ~IBenchmark() = default;
  virtual std::string name() const = 0;
  virtual bool isAvailable() const = 0;
  virtual BenchResult run() = 0;
};

}  // namespace aiz
