#pragma once

#include <cstdint>

namespace aiz {

enum class TimelineAgg {
  Max,  // max in bucket (preserves spikes)
  Avg,  // average in bucket
};

struct Config {
  // Display toggles
  bool showCpu = true;
  bool showGpu = true;
  bool showDisk = true;
  bool showPcieRx = true;
  bool showPcieTx = true;
  bool showRam = true;
  bool showVram = true;

  // Sampling
  std::uint32_t refreshMs = 500;
  std::uint32_t timelineSamples = 120;

  // Timeline rendering
  TimelineAgg timelineAgg = TimelineAgg::Max;

  static Config load();
  void save() const;
};

}  // namespace aiz
