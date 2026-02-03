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

enum class TimelineGraphStyle {
  Block,   // Classic block characters (█▓)
  Braille, // Braille dot patterns for high resolution
  Smooth,  // Half-block characters for smooth lines
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

  // H. Bars view display toggles (independent from Timelines view).
  // Defaults are "all on" so H. Bars is maximally informative out of the box.
  bool showCpuBars = true;
  bool showCpuHotBars = true;
  bool showGpuBars = true;
  bool showGpuMemBars = true;
  bool showGpuClockBars = true;
  bool showGpuMemClockBars = true;
  bool showGpuEncBars = true;
  bool showGpuDecBars = true;
  bool showDiskReadBars = true;
  bool showDiskWriteBars = true;
  bool showNetRxBars = true;
  bool showNetTxBars = true;
  bool showPcieRxBars = true;
  bool showPcieTxBars = true;
  bool showRamBars = true;
  bool showVramBars = true;

  // Sampling
  std::uint32_t refreshMs = 500;
  std::uint32_t timelineSamples = 120;

  // Peak value display
  bool showPeakValues = true;
  std::uint32_t peakWindowSec = 30;  // Peak window in seconds (default 30s)

  // Timeline rendering
  TimelineAgg timelineAgg = TimelineAgg::Max;
  TimelineGraphStyle timelineGraphStyle = TimelineGraphStyle::Braille;

  // UI colors
  MetricNameColor metricNameColor = MetricNameColor::Cyan;

  static Config load();
  static std::string path();
  void save() const;
};

}  // namespace aiz
