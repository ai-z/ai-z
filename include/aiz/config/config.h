#pragma once

#include <cstdint>
#include <string>

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
  bool showCpuHot = true;
  bool showGpu = true;
  bool showGpuMem = false;
  bool showGpuClock = false;
  bool showGpuMemClock = false;
  bool showGpuEnc = false;
  bool showGpuDec = false;
  // Backward-compat aggregate toggle (controls both read+write).
  bool showDisk = false;
  bool showDiskRead = false;
  bool showDiskWrite = false;
  bool showNetRx = false;
  bool showNetTx = false;
  bool showPcieRx = true;
  bool showPcieTx = false;
  bool showRam = false;
  bool showVram = false;

  // Sampling
  std::uint32_t refreshMs = 500;
  std::uint32_t timelineSamples = 120;

  // Timeline rendering
  TimelineAgg timelineAgg = TimelineAgg::Max;

  // UI colors
  MetricNameColor metricNameColor = MetricNameColor::Cyan;

  static Config load();
  static std::string path();
  void save() const;
};

}  // namespace aiz
