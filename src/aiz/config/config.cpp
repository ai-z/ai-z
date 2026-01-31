#include <aiz/config/config.h>

#include <aiz/platform/config_paths.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace aiz {

static fs::path configPath() {
  return platform::configDirectory() / "config.ini";
}

static bool parseBool(std::string v, bool fallback) {
  for (char& c : v) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
  if (v == "0" || v == "false" || v == "no" || v == "off") return false;
  return fallback;
}

static TimelineAgg parseTimelineAgg(std::string v, TimelineAgg fallback) {
  for (char& c : v) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (v == "max" || v == "peak" || v == "highest") return TimelineAgg::Max;
  if (v == "avg" || v == "average" || v == "mean") return TimelineAgg::Avg;
  return fallback;
}

static const char* timelineAggToString(TimelineAgg v) {
  switch (v) {
    case TimelineAgg::Max:
      return "max";
    case TimelineAgg::Avg:
      return "avg";
    default:
      return "max";
  }
}

static MetricNameColor parseMetricNameColor(std::string v, MetricNameColor fallback) {
  for (char& c : v) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (v == "cyan" || v == "lightblue" || v == "light-blue" || v == "light blue") return MetricNameColor::Cyan;
  if (v == "white" || v == "gray" || v == "grey") return MetricNameColor::White;
  if (v == "green") return MetricNameColor::Green;
  if (v == "yellow" || v == "amber") return MetricNameColor::Yellow;
  return fallback;
}

static const char* metricNameColorToString(MetricNameColor v) {
  switch (v) {
    case MetricNameColor::White:
      return "white";
    case MetricNameColor::Green:
      return "green";
    case MetricNameColor::Yellow:
      return "yellow";
    case MetricNameColor::Cyan:
    default:
      return "cyan";
  }
}

static TimelineGraphStyle parseTimelineGraphStyle(std::string v, TimelineGraphStyle fallback) {
  for (char& c : v) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (v == "block" || v == "blocks" || v == "classic") return TimelineGraphStyle::Block;
  if (v == "braille" || v == "dots" || v == "highres") return TimelineGraphStyle::Braille;
  if (v == "smooth" || v == "halfblock" || v == "half-block") return TimelineGraphStyle::Smooth;
  return fallback;
}

static const char* timelineGraphStyleToString(TimelineGraphStyle v) {
  switch (v) {
    case TimelineGraphStyle::Block:
      return "block";
    case TimelineGraphStyle::Smooth:
      return "smooth";
    case TimelineGraphStyle::Braille:
    default:
      return "braille";
  }
}

Config Config::load() {
  Config cfg;
  bool sawCpuHot = false;

  bool sawCpuHotBars = false;

  std::ifstream in(configPath());
  if (!in.is_open()) {
    return cfg;
  }

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);

    if (key == "showCpu") {
      cfg.showCpu = parseBool(val, cfg.showCpu);
      if (!sawCpuHot) cfg.showCpuHot = cfg.showCpu;
    }
    else if (key == "showCpuHot") {
      cfg.showCpuHot = parseBool(val, cfg.showCpuHot);
      sawCpuHot = true;
    }
    else if (key == "showGpu") cfg.showGpu = parseBool(val, cfg.showGpu);
    else if (key == "showGpuMem") cfg.showGpuMem = parseBool(val, cfg.showGpuMem);
    else if (key == "showGpuClock") cfg.showGpuClock = parseBool(val, cfg.showGpuClock);
    else if (key == "showGpuMemClock") cfg.showGpuMemClock = parseBool(val, cfg.showGpuMemClock);
    else if (key == "showGpuEnc") cfg.showGpuEnc = parseBool(val, cfg.showGpuEnc);
    else if (key == "showGpuDec") cfg.showGpuDec = parseBool(val, cfg.showGpuDec);
    else if (key == "showDisk") {
      // Backward compat: old single toggle controls both.
      const bool v = parseBool(val, cfg.showDisk);
      cfg.showDisk = v;
      cfg.showDiskRead = v;
      cfg.showDiskWrite = v;
    }
    else if (key == "showDiskRead") cfg.showDiskRead = parseBool(val, cfg.showDiskRead);
    else if (key == "showDiskWrite") cfg.showDiskWrite = parseBool(val, cfg.showDiskWrite);
    else if (key == "showNet") {
      // Backward compat: single toggle controls both.
      const bool v = parseBool(val, true);
      cfg.showNetRx = v;
      cfg.showNetTx = v;
    }
    else if (key == "showNetRx") cfg.showNetRx = parseBool(val, cfg.showNetRx);
    else if (key == "showNetTx") cfg.showNetTx = parseBool(val, cfg.showNetTx);
    else if (key == "showPcie") {
      // Backward compat: old single toggle controls both.
      const bool v = parseBool(val, true);
      cfg.showPcieRx = v;
      cfg.showPcieTx = v;
    }
    else if (key == "showPcieRx") cfg.showPcieRx = parseBool(val, cfg.showPcieRx);
    else if (key == "showPcieTx") cfg.showPcieTx = parseBool(val, cfg.showPcieTx);
    else if (key == "showRam") cfg.showRam = parseBool(val, cfg.showRam);
    else if (key == "showVram") cfg.showVram = parseBool(val, cfg.showVram);

    // Bars view toggles (independent from Timelines)
    else if (key == "showCpuBars") {
      cfg.showCpuBars = parseBool(val, cfg.showCpuBars);
      if (!sawCpuHotBars) cfg.showCpuHotBars = cfg.showCpuBars;
    }
    else if (key == "showCpuHotBars") {
      cfg.showCpuHotBars = parseBool(val, cfg.showCpuHotBars);
      sawCpuHotBars = true;
    }
    else if (key == "showGpuBars") { cfg.showGpuBars = parseBool(val, cfg.showGpuBars); }
    else if (key == "showGpuMemBars") { cfg.showGpuMemBars = parseBool(val, cfg.showGpuMemBars); }
    else if (key == "showGpuClockBars") { cfg.showGpuClockBars = parseBool(val, cfg.showGpuClockBars); }
    else if (key == "showGpuMemClockBars") { cfg.showGpuMemClockBars = parseBool(val, cfg.showGpuMemClockBars); }
    else if (key == "showGpuEncBars") { cfg.showGpuEncBars = parseBool(val, cfg.showGpuEncBars); }
    else if (key == "showGpuDecBars") { cfg.showGpuDecBars = parseBool(val, cfg.showGpuDecBars); }
    else if (key == "showDiskReadBars") { cfg.showDiskReadBars = parseBool(val, cfg.showDiskReadBars); }
    else if (key == "showDiskWriteBars") { cfg.showDiskWriteBars = parseBool(val, cfg.showDiskWriteBars); }
    else if (key == "showNetRxBars") { cfg.showNetRxBars = parseBool(val, cfg.showNetRxBars); }
    else if (key == "showNetTxBars") { cfg.showNetTxBars = parseBool(val, cfg.showNetTxBars); }
    else if (key == "showPcieRxBars") { cfg.showPcieRxBars = parseBool(val, cfg.showPcieRxBars); }
    else if (key == "showPcieTxBars") { cfg.showPcieTxBars = parseBool(val, cfg.showPcieTxBars); }
    else if (key == "showRamBars") { cfg.showRamBars = parseBool(val, cfg.showRamBars); }
    else if (key == "showVramBars") { cfg.showVramBars = parseBool(val, cfg.showVramBars); }
    else if (key == "refreshMs") cfg.refreshMs = static_cast<std::uint32_t>(std::stoul(val));
    else if (key == "timelineSamples") cfg.timelineSamples = static_cast<std::uint32_t>(std::stoul(val));
    else if (key == "showPeakValues") cfg.showPeakValues = parseBool(val, cfg.showPeakValues);
    else if (key == "peakWindowSec") cfg.peakWindowSec = static_cast<std::uint32_t>(std::stoul(val));
    else if (key == "timelineAgg") cfg.timelineAgg = parseTimelineAgg(val, cfg.timelineAgg);
    else if (key == "timelineGraphStyle") cfg.timelineGraphStyle = parseTimelineGraphStyle(val, cfg.timelineGraphStyle);
    else if (key == "metricNameColor") cfg.metricNameColor = parseMetricNameColor(val, cfg.metricNameColor);
  }

  return cfg;
}

std::string Config::path() {
  return configPath().string();
}

void Config::save() const {
  const fs::path path = configPath();
  fs::create_directories(path.parent_path());

  std::ofstream out(path);
  out << "# ai-z config\n";
  out << "showCpu=" << (showCpu ? "true" : "false") << "\n";
  out << "showCpuHot=" << (showCpuHot ? "true" : "false") << "\n";
  out << "showGpu=" << (showGpu ? "true" : "false") << "\n";
  out << "showGpuMem=" << (showGpuMem ? "true" : "false") << "\n";
  out << "showGpuClock=" << (showGpuClock ? "true" : "false") << "\n";
  out << "showGpuMemClock=" << (showGpuMemClock ? "true" : "false") << "\n";
  out << "showGpuEnc=" << (showGpuEnc ? "true" : "false") << "\n";
  out << "showGpuDec=" << (showGpuDec ? "true" : "false") << "\n";
  // Prefer explicit per-direction toggles.
  out << "showDiskRead=" << (showDiskRead ? "true" : "false") << "\n";
  out << "showDiskWrite=" << (showDiskWrite ? "true" : "false") << "\n";
  out << "showNetRx=" << (showNetRx ? "true" : "false") << "\n";
  out << "showNetTx=" << (showNetTx ? "true" : "false") << "\n";
  out << "showPcieRx=" << (showPcieRx ? "true" : "false") << "\n";
  out << "showPcieTx=" << (showPcieTx ? "true" : "false") << "\n";
  out << "showRam=" << (showRam ? "true" : "false") << "\n";
  out << "showVram=" << (showVram ? "true" : "false") << "\n";

  // Bars view toggles
  out << "showCpuBars=" << (showCpuBars ? "true" : "false") << "\n";
  out << "showCpuHotBars=" << (showCpuHotBars ? "true" : "false") << "\n";
  out << "showGpuBars=" << (showGpuBars ? "true" : "false") << "\n";
  out << "showGpuMemBars=" << (showGpuMemBars ? "true" : "false") << "\n";
  out << "showGpuClockBars=" << (showGpuClockBars ? "true" : "false") << "\n";
  out << "showGpuMemClockBars=" << (showGpuMemClockBars ? "true" : "false") << "\n";
  out << "showGpuEncBars=" << (showGpuEncBars ? "true" : "false") << "\n";
  out << "showGpuDecBars=" << (showGpuDecBars ? "true" : "false") << "\n";
  out << "showDiskReadBars=" << (showDiskReadBars ? "true" : "false") << "\n";
  out << "showDiskWriteBars=" << (showDiskWriteBars ? "true" : "false") << "\n";
  out << "showNetRxBars=" << (showNetRxBars ? "true" : "false") << "\n";
  out << "showNetTxBars=" << (showNetTxBars ? "true" : "false") << "\n";
  out << "showPcieRxBars=" << (showPcieRxBars ? "true" : "false") << "\n";
  out << "showPcieTxBars=" << (showPcieTxBars ? "true" : "false") << "\n";
  out << "showRamBars=" << (showRamBars ? "true" : "false") << "\n";
  out << "showVramBars=" << (showVramBars ? "true" : "false") << "\n";
  out << "refreshMs=" << refreshMs << "\n";
  out << "timelineSamples=" << timelineSamples << "\n";
  out << "showPeakValues=" << (showPeakValues ? "true" : "false") << "\n";
  out << "peakWindowSec=" << peakWindowSec << "\n";
  out << "timelineAgg=" << timelineAggToString(timelineAgg) << "\n";
  out << "timelineGraphStyle=" << timelineGraphStyleToString(timelineGraphStyle) << "\n";
  out << "metricNameColor=" << metricNameColorToString(metricNameColor) << "\n";
}

}  // namespace aiz
