#pragma once

#include <cstdint>

namespace aiz {

enum class TimelineAgg {
  Max,  // max in bucket (preserves spikes)
  Avg,  // average in bucket
};

enum class MetricNameColor {
  Cyan,
  White,
  Green,
  Yellow,
};

struct Config {
  // Display toggles
  bool showCpu = true;
  bool showGpu = true;
  bool showGpuMem = true;
  bool showGpuClock = true;
  bool showGpuMemClock = true;
  bool showGpuEnc = true;
  bool showGpuDec = true;
  // Backward-compat aggregate toggle (controls both read+write).
  bool showDisk = true;
  bool showDiskRead = true;
  bool showDiskWrite = true;
  bool showNetRx = true;
  bool showNetTx = true;
  bool showPcieRx = true;
  bool showPcieTx = true;
  bool showRam = true;
  bool showVram = true;

  // Sampling
  std::uint32_t refreshMs = 500;
  std::uint32_t timelineSamples = 120;

  // Timeline rendering
  TimelineAgg timelineAgg = TimelineAgg::Max;

  // UI colors
  MetricNameColor metricNameColor = MetricNameColor::Cyan;

  static Config load();
  void save() const;
};

}  // namespace aiz
