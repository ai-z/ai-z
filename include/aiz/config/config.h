#pragma once

#include <cstdint>

namespace aiz {

struct Config {
  // Display toggles
  bool showCpu = true;
  bool showGpu = true;
  bool showDisk = true;
  bool showPcie = true;

  // Sampling
  std::uint32_t refreshMs = 500;
  std::uint32_t timelineSamples = 120;

  static Config load();
  void save() const;
};

}  // namespace aiz
