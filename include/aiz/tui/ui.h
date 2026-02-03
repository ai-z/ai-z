#pragma once

#include <memory>

namespace aiz {

struct Config;

class Ui {
public:
  virtual ~Ui() = default;
  virtual int run(Config& cfg, bool debugMode) = 0;
};

// Returns the platform UI implementation.
std::unique_ptr<Ui> makeUi();

}  // namespace aiz
