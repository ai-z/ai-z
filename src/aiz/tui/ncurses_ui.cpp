#include <aiz/tui/ncurses_ui.h>

#include <aiz/bench/factory.h>
#include <aiz/hw/hardware_info.h>
#include <aiz/metrics/cpu_usage.h>
#include <aiz/metrics/disk_bandwidth.h>
#include <aiz/metrics/gpu_usage.h>
#include <aiz/metrics/nvidia_nvml.h>
#include <aiz/metrics/pcie_bandwidth.h>
#include <aiz/metrics/ram_usage.h>
#include <aiz/metrics/timeline.h>
#include <aiz/version.h>

#include <curses.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <random>

namespace aiz {

namespace {

enum class Screen { Timelines, Help, Benchmarks, Config, Hardware };

static void drawScrollingBars(
    int topY,
    int leftX,
    int height,
    int width,
    const Timeline& tl,
    double maxV,
    TimelineAgg agg) {
  if (height <= 0 || width <= 0) return;

  const auto vals = tl.values();
  const int colsToDraw = width;
  const int available = static_cast<int>(vals.size());

  // Clear region.
  for (int r = 0; r < height; ++r) {
    mvhline(topY + r, leftX, ' ', colsToDraw);
  }

  if (available <= 0 || maxV <= 0.0) return;

  const int cols = colsToDraw;
  const int count = std::min(cols, available);
  const int xOffset = cols - count;

  const bool needsBucketing = available > cols;
  const int bucket = needsBucketing ? static_cast<int>((available + cols - 1) / cols) : 1;

  for (int i = 0; i < count; ++i) {
    const int x = xOffset + i;

    double v = 0.0;
    if (!needsBucketing) {
      // Draw last N samples across columns.
      // If we have fewer samples than the terminal width, right-align the data so the newest samples
      // end at the right edge (htop-style).
      const int startIdx = std::max(0, available - count);
      const int idx = startIdx + i;
      if (idx < 0 || idx >= available) continue;
      v = vals[static_cast<std::size_t>(idx)];
    } else {
      // Map the full history into the available columns.
      const int start = i * bucket;
      const int end = std::min(available, start + bucket);
      if (start >= end) continue;

      if (agg == TimelineAgg::Avg) {
        double sum = 0.0;
        int n = 0;
        for (int j = start; j < end; ++j) {
          double s = vals[static_cast<std::size_t>(j)];
          if (!std::isfinite(s)) continue;
          sum += s;
          ++n;
        }
        v = (n > 0) ? (sum / static_cast<double>(n)) : 0.0;
      } else {
        double maxBucket = -std::numeric_limits<double>::infinity();
        for (int j = start; j < end; ++j) {
          double s = vals[static_cast<std::size_t>(j)];
          if (!std::isfinite(s)) continue;
          maxBucket = std::max(maxBucket, s);
        }
        if (!std::isfinite(maxBucket)) maxBucket = 0.0;
        v = maxBucket;
      }
    }

    if (!std::isfinite(v)) v = 0.0;
    v = std::clamp(v, 0.0, maxV);

    const int barH = static_cast<int>(std::round((v / maxV) * static_cast<double>(height)));
    const int clampedH = std::clamp(barH, 0, height);

    for (int r = 0; r < clampedH; ++r) {
      // Draw from bottom upward.
      mvaddch(topY + (height - 1 - r), leftX + x, ACS_CKBOARD);
    }
  }
}

static void ensureTimelineCapacity(Timeline& tl, std::size_t desiredCapacity) {
  if (tl.capacity() >= desiredCapacity) return;
  Timeline resized(desiredCapacity);
  for (double v : tl.values()) {
    resized.push(v);
  }
  tl = std::move(resized);
}

static void drawFooter(int rows, int cols, Screen screen) {
  (void)screen;
  const int y = rows - 1;
  int x = 0;

  mvhline(y, 0, ' ', cols);

  const bool colors = has_colors() != 0;

  auto addPlain = [&](const std::string& s) {
    if (x >= cols) return;
    mvaddnstr(y, x, s.c_str(), cols - x);
    x += static_cast<int>(s.size());
  };

  auto addKeyBlock = [&](const std::string& keyToken) {
    if (x >= cols) return;
    if (colors) attron(COLOR_PAIR(1) | A_BOLD);
    mvaddnstr(y, x, keyToken.c_str(), cols - x);
    if (colors) attroff(COLOR_PAIR(1) | A_BOLD);
    x += static_cast<int>(keyToken.size());
  };

  auto addLabelWithHot = [&](const std::string& label, char hot) {
    if (x >= cols) return;
    for (char c : label) {
      if (x >= cols) break;
      const bool isHot = (std::toupper(static_cast<unsigned char>(c)) == std::toupper(static_cast<unsigned char>(hot)));
      if (colors && isHot) {
        attron(COLOR_PAIR(2) | A_BOLD);
        mvaddch(y, x, c);
        attroff(COLOR_PAIR(2) | A_BOLD);
      } else {
        mvaddch(y, x, c);
      }
      ++x;
    }
  };

  // Format: [F-key block][space][Label]
  // Also supports letter shortcuts: H/W/B/C/Q.
  addKeyBlock(" F1 ");
  addPlain(" ");
  addLabelWithHot("Help", 'H');
  addPlain("  ");

  addKeyBlock(" F2 ");
  addPlain(" ");
  addLabelWithHot("Hardware", 'W');
  addPlain("  ");

  addKeyBlock(" F3 ");
  addPlain(" ");
  addLabelWithHot("Benchmarks", 'B');
  addPlain("  ");

  addKeyBlock(" F4 ");
  addPlain(" ");
  addLabelWithHot("Config", 'C');
  addPlain("  ");

  addKeyBlock(" F10 ");
  addPlain(" ");
  addLabelWithHot("Quit", 'Q');
}

static void drawHeader(int cols, const std::string& title) {
  const bool colors = has_colors() != 0;

  // Clear the whole title bar with default background.
  mvhline(0, 0, ' ', cols);

  // If the title starts with "AI-Z", draw that prefix with green background,
  // and draw the remaining metrics text with default (black) background.
  constexpr const char* kPrefix = "AI-Z";
  const bool startsWithAiz = title.rfind(kPrefix, 0) == 0;

  int x = 0;
  if (colors && startsWithAiz) {
    const std::string block = " AI-Z ";
    attron(COLOR_PAIR(3) | A_BOLD);
    mvaddnstr(0, 0, block.c_str(), cols);
    attroff(COLOR_PAIR(3) | A_BOLD);
    x = static_cast<int>(block.size());

    // Skip the literal "AI-Z" from the incoming title.
    std::string rest = title.substr(4);
    // Ensure at least one space after the green block.
    if (rest.empty() || rest.front() != ' ') rest = " " + rest;
    mvaddnstr(0, x, rest.c_str(), std::max(0, cols - x));
  } else {
    mvaddnstr(0, 0, title.c_str(), cols);
  }
}

struct VramUsage {
  double usedGiB = 0.0;
  double totalGiB = 0.0;
  double usedPct = 0.0;
};

struct GpuTelemetry {
  std::optional<double> utilPct;
  std::optional<double> vramUsedGiB;
  std::optional<double> vramTotalGiB;
  std::optional<double> watts;
  std::optional<double> tempC;
  std::string pstate;
};

static std::string trim(std::string s) {
  auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

static std::optional<std::string> runCommand(const std::string& cmd) {
  std::array<char, 4096> buf{};
  std::string out;

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return std::nullopt;

  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
    out += buf.data();
    if (out.size() > 64 * 1024) break;
  }

  const int rc = pclose(pipe);
  if (rc != 0) return std::nullopt;

  out = trim(out);
  if (out.empty()) return std::nullopt;
  return out;
}

static std::optional<GpuTelemetry> readNvidiaGpuTelemetry() {
  // Query first GPU. nounits ensures raw numbers.
  // Example output: "23.45, 45, P8"
  const auto line = runCommand(
      "nvidia-smi --query-gpu=power.draw,temperature.gpu,pstate --format=csv,noheader,nounits 2>/dev/null | head -n 1");
  if (!line) return std::nullopt;

  std::string s = *line;
  std::string a, b, c;
  const auto p1 = s.find(',');
  if (p1 == std::string::npos) return std::nullopt;
  a = trim(s.substr(0, p1));
  s = s.substr(p1 + 1);
  const auto p2 = s.find(',');
  if (p2 == std::string::npos) return std::nullopt;
  b = trim(s.substr(0, p2));
  c = trim(s.substr(p2 + 1));

  if (a.empty() || b.empty() || c.empty()) return std::nullopt;

  try {
    GpuTelemetry t;
    t.watts = std::stod(a);
    t.tempC = std::stod(b);
    t.pstate = c;
    return t;
  } catch (...) {
    return std::nullopt;
  }
}

static std::optional<GpuTelemetry> readGpuTelemetryPreferNvml(unsigned int index) {
  if (const auto nv = readNvmlTelemetryForGpu(index)) {
    GpuTelemetry t;
    t.utilPct = nv->gpuUtilPct;
    t.vramUsedGiB = nv->memUsedGiB;
    t.vramTotalGiB = nv->memTotalGiB;
    t.watts = nv->powerWatts;
    t.tempC = nv->tempC;
    t.pstate = nv->pstate;
    return t;
  }

  // Fallback only supports first GPU (nvidia-smi has no multi-GPU loop here).
  if (index == 0) return readNvidiaGpuTelemetry();
  return std::nullopt;
}

static std::string fmt1(double v) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(1);
  oss << v;
  return oss.str();
}

static std::string fmt0(double v) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(0);
  oss << v;
  return oss.str();
}

static std::string makeTitleBar(const std::optional<Sample>& cpu,
                                const std::optional<Sample>& gpu,
                                const std::optional<Sample>& pcie,
                                const std::optional<RamUsage>& ram,
                                const std::optional<VramUsage>& vram) {
  std::string cpuStr = "CPU --";
  if (cpu) cpuStr = "CPU " + fmt0(cpu->value) + "%";

  std::string ramStr = "RAM --";
  if (ram) {
    ramStr = "RAM " + fmt1(ram->usedGiB) + "/" + fmt1(ram->totalGiB) + "G(" + fmt0(ram->usedPct) + "%)";
  }

  std::string gpuStr = "GPU --";
  if (gpu) gpuStr = "GPU " + fmt0(gpu->value) + "%";

  std::string vramStr = "VRAM --";
  if (vram) {
    vramStr = "VRAM " + fmt1(vram->usedGiB) + "/" + fmt1(vram->totalGiB) + "G(" + fmt0(vram->usedPct) + "%)";
  }

  std::string pcieStr = "PCIe --";
  if (pcie) {
    // Example unit may be MB/s.
    pcieStr = "PCIe " + fmt0(pcie->value) + pcie->unit;
  }

  return "AI-Z  " + cpuStr + "  " + ramStr + "  " + vramStr + "  " + gpuStr + "  " + pcieStr;
}

static void drawTitleBarTimelines(int cols,
                                  const std::optional<Sample>& cpu,
                                  const std::optional<Sample>& pcieRx,
                                  const std::optional<Sample>& pcieTx,
                                  const std::optional<RamUsage>& ram,
                                  const std::vector<std::optional<GpuTelemetry>>& gpus) {
  const bool colors = has_colors() != 0;
  mvhline(0, 0, ' ', cols);
  mvhline(1, 0, ' ', cols);
  int x0 = 0;
  int x1 = 0;

  auto addPlain = [&](int row, int& x, const std::string& s) {
    if (x >= cols) return;
    mvaddnstr(row, x, s.c_str(), cols - x);
    x += static_cast<int>(s.size());
  };

  auto addGreenLabel = [&](int row, int& x, const std::string& label) {
    if (x >= cols) return;
    const std::string block = " " + label + " ";
    if (colors) attron(COLOR_PAIR(3) | A_BOLD);
    mvaddnstr(row, x, block.c_str(), cols - x);
    if (colors) attroff(COLOR_PAIR(3) | A_BOLD);
    x += static_cast<int>(block.size());
  };

  auto addValue = [&](int row, int& x, const std::string& value) {
    addPlain(row, x, " " + value + " ");
  };

  // AI-Z prefix block stays green.
  if (colors) attron(COLOR_PAIR(3) | A_BOLD);
  mvaddnstr(0, 0, " AI-Z ", cols);
  if (colors) attroff(COLOR_PAIR(3) | A_BOLD);
  x0 = 6;
  addPlain(0, x0, " ");

  // Row 1 aligns under the metrics area after the AI-Z block.
  x1 = 0;
  addPlain(1, x1, std::string(7, ' '));

  // CPU
  addGreenLabel(0, x0, "CPU");
  addValue(0, x0, cpu ? (fmt0(cpu->value) + "%") : std::string("--"));

  // RAM
  addGreenLabel(0, x0, "RAM");
  if (ram) {
    addValue(0, x0, fmt1(ram->usedGiB) + "/" + fmt1(ram->totalGiB) + "G(" + fmt0(ram->usedPct) + "%)");
  } else {
    addValue(0, x0, "--");
  }

  // Per-GPU utilization + VRAM
  for (std::size_t i = 0; i < gpus.size(); ++i) {
    const auto& gt = gpus[i];
    const std::string gi = "G" + std::to_string(i);
    const std::string vi = "V" + std::to_string(i);

    addGreenLabel(0, x0, gi);
    if (gt && gt->utilPct) addValue(0, x0, fmt0(*gt->utilPct) + "%");
    else addValue(0, x0, "--");

    addGreenLabel(0, x0, vi);
    if (gt && gt->vramUsedGiB && gt->vramTotalGiB) {
      const double used = *gt->vramUsedGiB;
      const double total = *gt->vramTotalGiB;
      const double pct = total > 0.0 ? (100.0 * used / total) : 0.0;
      addValue(0, x0, fmt1(used) + "/" + fmt1(total) + "G(" + fmt0(pct) + "%)");
    } else {
      addValue(0, x0, "--");
    }
  }

  // PCIe RX/TX
  addGreenLabel(0, x0, "PCIeRX");
  if (pcieRx) {
    addValue(0, x0, fmt0(pcieRx->value) + pcieRx->unit);
  } else {
    addValue(0, x0, "--");
  }

  addGreenLabel(0, x0, "PCIeTX");
  if (pcieTx) {
    addValue(0, x0, fmt0(pcieTx->value) + pcieTx->unit);
  } else {
    addValue(0, x0, "--");
  }

  // Row 1: per-GPU power/thermals/state
  for (std::size_t i = 0; i < gpus.size(); ++i) {
    const auto& gt = gpus[i];
    const std::string wi = "W" + std::to_string(i);
    const std::string ti = "T" + std::to_string(i);
    const std::string pi = "P" + std::to_string(i);

    addGreenLabel(1, x1, wi);
    if (gt && gt->watts) addValue(1, x1, fmt0(*gt->watts) + "W");
    else addValue(1, x1, "--");

    addGreenLabel(1, x1, ti);
    if (gt && gt->tempC) addValue(1, x1, fmt0(*gt->tempC) + "C");
    else addValue(1, x1, "--");

    addGreenLabel(1, x1, pi);
    if (gt && !gt->pstate.empty()) addValue(1, x1, gt->pstate);
    else addValue(1, x1, "--");
  }
}

static void drawSectionTitleLine(int y, int cols, const std::string& title) {
  const bool colors = has_colors() != 0;
  mvhline(y, 0, ' ', cols);

  if (!colors) {
    mvaddnstr(y, 0, title.c_str(), cols);
  } else {
    // Split into "label: " and "value" so the value can be green.
    const std::size_t sep = title.find(": ");
    const bool hasValue = (sep != std::string::npos) && (sep + 2 < title.size());

    const std::string left = hasValue ? title.substr(0, sep + 2) : title;
    const std::string value = hasValue ? title.substr(sep + 2) : std::string{};

    attron(COLOR_PAIR(4) | A_BOLD);
    mvaddnstr(y, 0, left.c_str(), cols);
    attroff(COLOR_PAIR(4) | A_BOLD);

    if (hasValue) {
      attron(COLOR_PAIR(5) | A_BOLD);
      mvaddnstr(y, static_cast<int>(left.size()), value.c_str(), std::max(0, cols - static_cast<int>(left.size())));
      attroff(COLOR_PAIR(5) | A_BOLD);
    }
  }

  // Fill rest of the line with '-' to separate sections.
  int start = static_cast<int>(title.size());
  if (start < cols) {
    if (start > 0 && start < cols) {
      mvaddch(y, start, ' ');
      ++start;
    }
    if (start < cols) {
      if (colors) attron(COLOR_PAIR(4) | A_BOLD);
      mvhline(y, start, '-', cols - start);
      if (colors) attroff(COLOR_PAIR(4) | A_BOLD);
    }
  }
}

static void drawTimelines(
    int rows,
    int cols,
    const Config& cfg,
  const std::string& cpuDevice,
  const std::string& gpuDevice,
    const std::optional<Sample>& cpu,
    const std::optional<Sample>& ram,
    const std::optional<Sample>& vram,
    const std::vector<std::optional<Sample>>& gpus,
    const std::optional<Sample>& disk,
    const std::optional<Sample>& pcieRx,
    const std::optional<Sample>& pcieTx,
    const Timeline& cpuTl,
    const Timeline& ramTl,
    const Timeline& vramTl,
    const std::vector<Timeline>& gpuTls,
    const Timeline& diskTl,
    const Timeline& pcieRxTl,
    const Timeline& pcieTxTl) {
  struct Panel {
    std::string name;
    bool enabled;
    const std::optional<Sample>* sample;
    const Timeline* tl;
    double maxV;
    std::string device;
  };

  std::vector<Panel> panels;
  panels.push_back(Panel{"CPU", cfg.showCpu, &cpu, &cpuTl, 100.0, cpuDevice});

  // Memory timelines are currently always shown when telemetry is available (no config toggles yet).
  panels.push_back(Panel{"RAM", cfg.showRam, &ram, &ramTl, 100.0, std::string{}});
  panels.push_back(Panel{"VRAM", cfg.showVram, &vram, &vramTl, 100.0, std::string{}});

  if (cfg.showGpu) {
    const std::size_t n = std::min(gpus.size(), gpuTls.size());
    for (std::size_t i = 0; i < n; ++i) {
      const std::string name = "GPU" + std::to_string(i);
      std::string dev = gpuDevice;
      if (!dev.empty()) dev += " #" + std::to_string(i);
      panels.push_back(Panel{name, true, &gpus[i], &gpuTls[i], 100.0, dev});
    }
  }

  panels.push_back(Panel{"Disk", cfg.showDisk, &disk, &diskTl, 5000.0, std::string{}});
  // PCIe samples are in MB/s (from NVML).
  panels.push_back(Panel{"PCIe RX", cfg.showPcieRx, &pcieRx, &pcieRxTl, 32'000.0, std::string{}});
  panels.push_back(Panel{"PCIe TX", cfg.showPcieTx, &pcieTx, &pcieTxTl, 32'000.0, std::string{}});

  panels.erase(
      std::remove_if(panels.begin(), panels.end(), [](const Panel& p) { return !p.enabled; }),
      panels.end());

  if (panels.empty()) {
    mvaddnstr(2, 0, "No timelines enabled. Use Config (F4 / C) to enable.", cols);
    return;
  }

  // Start immediately under the header bar.
  const int top = 2;
  const int bottomReserved = 1;  // footer
  const int usable = std::max(0, (rows - bottomReserved) - top);
  const int gap = 0;
  const int labelRows = 1;
  const int n = static_cast<int>(panels.size());

  // Allocate height fairly across panels: 1 label row + graph rows.
  const int totalGaps = (n - 1) * gap;
  const int minGraphRows = 3;
  const int perPanel = std::max(labelRows + minGraphRows, (usable - totalGaps) / n);

  int y = top;
  for (int i = 0; i < n; ++i) {
    const Panel& p = panels[static_cast<std::size_t>(i)];

    const int remainingPanels = n - i;
    const int remainingUsable = std::max(0, (rows - bottomReserved) - y);
    const int height = (remainingPanels == 1)
        ? std::max(0, remainingUsable)
        : std::min(perPanel, remainingUsable);
    if (height <= 0) break;

    // Section title line with device/context info.
    std::string title = p.name;
    if (!p.device.empty()) {
      title += " (" + p.device + ")";
    } else if (p.name == "Disk") {
      if (p.sample->has_value() && !(*p.sample)->label.empty()) {
        title += " (" + (*p.sample)->label + ")";
      }
    }

    if (p.sample->has_value()) {
      title += ": " + std::to_string((*p.sample)->value) + " " + (*p.sample)->unit;
    } else {
      title += ": unavailable";
    }
    drawSectionTitleLine(y, cols, title);

    // Graph area: fill remaining rows in this panel.
    const int graphTop = y + 1;
    const int graphH = std::max(0, height - 1);
    if (graphH > 0) {
      drawScrollingBars(graphTop, 0, graphH, cols, *p.tl, p.maxV, cfg.timelineAgg);
    }

    y += height;
    if (i != n - 1) {
      y += gap;
    }
  }
}

static void drawBenchmarks(
    int rows,
    int cols,
    int selected,
    const std::vector<std::unique_ptr<IBenchmark>>& benches,
    const std::vector<std::string>& benchResults,
    const std::string& lastResult) {
  const bool colors = has_colors() != 0;
  int y = 2;

  // Two-column layout: left = benchmark names, right = results.
  // Keep results in their own column rather than directly under the benchmark title.
  std::size_t maxNameLen = 0;
  for (const auto& b : benches) {
    if (!b) continue;
    std::string n = b->name();
    if (!b->isAvailable()) n += " [unavailable]";
    maxNameLen = std::max(maxNameLen, n.size());
  }
  // Value column starts after the name column, but clamp so it doesn't drift too far right.
  const int minValueCol = 24;
  const int maxValueCol = std::max(0, cols / 2);
  const int valueCol = std::min(cols - 1, std::max(minValueCol, std::min(maxValueCol, static_cast<int>(maxNameLen) + 4)));

  // Inside the results column, align "label: value" pairs.
  std::size_t maxResultLabelLen = 0;
  for (const auto& r : benchResults) {
    std::size_t start = 0;
    while (start <= r.size()) {
      const std::size_t end = r.find('\n', start);
      const std::string line = (end == std::string::npos) ? r.substr(start) : r.substr(start, end - start);
      const std::size_t sep = line.find(": ");
      if (sep != std::string::npos) maxResultLabelLen = std::max(maxResultLabelLen, sep);  // label only
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  maxResultLabelLen = std::min<std::size_t>(maxResultLabelLen, 8);
  constexpr int kResultIndent = 4;  // indent inside the results column
  const int labelCol = std::min(cols - 1, valueCol + kResultIndent);
  const int valCol = std::min(cols - 1, labelCol + static_cast<int>(maxResultLabelLen) + 2);

  // Special entry: run all.
  {
    const std::string line = (selected == 0 ? "> " : "  ") + std::string("Run all benchmarks");
    mvhline(y, 0, ' ', cols);
    mvaddnstr(y, 0, line.c_str(), cols);
    ++y;
    // Blank spacer line between "run all" and individual benches.
    if (y < rows - 2) {
      mvhline(y, 0, ' ', cols);
      ++y;
    }
  }

  for (int i = 0; i < static_cast<int>(benches.size()); ++i) {
    const int rowIndex = i + 1;  // shifted by "run all"
    const auto& b = benches[static_cast<std::size_t>(i)];
    mvhline(y, 0, ' ', cols);

    const std::string prefix = (rowIndex == selected ? "> " : "  ");
    const std::string name = b ? b->name() : std::string("(null)");
    const bool avail = b && b->isAvailable();
    const std::string suffix = (b && !avail) ? " [unavailable]" : std::string{};

    // Left column: benchmark name.
    mvaddnstr(y, 0, prefix.c_str(), cols);
    if (colors && avail) attron(COLOR_PAIR(4) | A_BOLD);
    mvaddnstr(y, static_cast<int>(prefix.size()), name.c_str(), std::max(0, cols - static_cast<int>(prefix.size())));
    if (colors && avail) attroff(COLOR_PAIR(4) | A_BOLD);
    if (!suffix.empty()) {
      const int sx = static_cast<int>(prefix.size() + name.size());
      if (sx < cols) mvaddnstr(y, sx, suffix.c_str(), cols - sx);
    }

    // Right column: results.
    if (i >= 0 && i < static_cast<int>(benchResults.size()) && !benchResults[static_cast<std::size_t>(i)].empty()) {
      std::string r = benchResults[static_cast<std::size_t>(i)];
      std::size_t start = 0;
      int ry = y;
      while (start <= r.size() && ry < rows - 2) {
        const std::size_t end = r.find('\n', start);
        const std::string line = (end == std::string::npos) ? r.substr(start) : r.substr(start, end - start);

        if (ry != y) {
          mvhline(ry, 0, ' ', cols);
        }

        const std::size_t sep = line.find(": ");
        if (sep != std::string::npos) {
          const std::string label = line.substr(0, sep + 1);
          const std::string val = line.substr(sep + 2);
          if (labelCol < cols) mvaddnstr(ry, labelCol, label.c_str(), cols - labelCol);
          if (valCol < cols) mvaddnstr(ry, valCol, val.c_str(), cols - valCol);
        } else {
          if (labelCol < cols) mvaddnstr(ry, labelCol, line.c_str(), cols - labelCol);
        }

        if (end == std::string::npos) break;
        start = end + 1;
        ++ry;
      }
      y = ry;
    }

    ++y;
  }

  mvhline(rows - 3, 0, ' ', cols);
  mvaddnstr(rows - 3, 0, "Enter: run   Esc: back", cols);

  mvhline(rows - 2, 0, ' ', cols);
  mvaddnstr(rows - 2, 0, lastResult.c_str(), cols);
}

static void drawConfig(int rows, int cols, int selected, const Config& cfg) {
  struct Item { const char* label; bool value; };
  const Item items[] = {
      {"Show CPU usage", cfg.showCpu},
      {"Show GPU usage", cfg.showGpu},
      {"Show Disk bandwidth", cfg.showDisk},
      {"Show PCIe RX", cfg.showPcieRx},
      {"Show PCIe TX", cfg.showPcieTx},
      {"Show RAM usage", cfg.showRam},
      {"Show VRAM usage", cfg.showVram},
  };

  int y = 2;
  constexpr int kCount = static_cast<int>(sizeof(items) / sizeof(items[0]));
  for (int i = 0; i < kCount; ++i) {
    std::string line = (i == selected ? "> " : "  ");
    line += items[i].label;
    line += ": ";
    line += items[i].value ? "ON" : "OFF";
    mvhline(y, 0, ' ', cols);
    mvaddnstr(y, 0, line.c_str(), cols);
    ++y;
  }
  mvhline(rows - 3, 0, ' ', cols);
  mvaddnstr(rows - 3, 0, "Space: toggle   s: save   Esc: back", cols);
}

static void drawHardware(int rows, int cols, const std::vector<std::string>& lines) {
  const bool colors = has_colors() != 0;

  struct Item {
    std::string key;
    std::string value;
  };

  std::vector<Item> items;
  items.reserve(lines.size());

  std::size_t maxKeyLen = 0;
  for (const auto& line : lines) {
    const std::size_t sep = line.find(": ");
    if (sep == std::string::npos) {
      items.push_back(Item{line, ""});
      maxKeyLen = std::max(maxKeyLen, line.size());
      continue;
    }
    const std::string key = line.substr(0, sep + 1);  // include ':'
    const std::string value = line.substr(sep + 2);
    items.push_back(Item{key, value});
    maxKeyLen = std::max(maxKeyLen, key.size());
  }

  const int yStart = 2;
  const int yEnd = rows - 2;
  const int valueCol = std::min(cols - 1, static_cast<int>(maxKeyLen) + 2);

  int y = yStart;
  for (const auto& it : items) {
    if (y >= yEnd) break;
    mvhline(y, 0, ' ', cols);

    if (colors) attron(COLOR_PAIR(4) | A_BOLD);
    mvaddnstr(y, 0, it.key.c_str(), cols);
    if (colors) attroff(COLOR_PAIR(4) | A_BOLD);

    if (!it.value.empty() && valueCol < cols) {
      mvaddnstr(y, valueCol, it.value.c_str(), cols - valueCol);
    }

    ++y;
  }

  mvhline(rows - 3, 0, ' ', cols);
  mvaddnstr(rows - 3, 0, "r: refresh   Esc: back", cols);
}

static void drawHelp(int rows, int cols) {
  int y = 2;

  const std::vector<std::string> lines = {
      std::string("ai-z ") + AIZ_VERSION,
      std::string("Website: ") + AIZ_WEBSITE,
      "",
      "Description:",
      "  Linux C++ TUI for performance timelines and benchmarks.",
      "  Timelines: CPU, RAM, VRAM, GPU, Disk bandwidth, PCIe RX/TX.",
      "",
      "Guide:",
      "  - Timelines are vertical bars that scroll over time.",
      "  - Use Config to enable/disable timelines and debug random CPU.",
      "  - Use Hardware to view OS/CPU/RAM/GPU/driver information.",
      "  - Use Benchmarks to run synthetic tests (stubs for now).",
      "",
      "Keys:",
      "  F1  Help        (H)",
      "  F2  Hardware    (W)",
      "  F3  Benchmarks  (B)",
      "  F4  Config      (C)",
      "  F10 Quit        (Q)",
      "  Esc Back to timelines",
  };

  for (const auto& line : lines) {
    if (y >= rows - 2) break;
    mvhline(y, 0, ' ', cols);
    mvaddnstr(y, 0, line.c_str(), cols);
    ++y;
  }

  mvhline(rows - 3, 0, ' ', cols);
  mvaddnstr(rows - 3, 0, "Esc: back", cols);
}

}  // namespace

namespace {

class RandomWalk {
public:
  RandomWalk(double minV, double maxV, double stepStd)
      : minV_(minV), maxV_(maxV), step_(0.0, stepStd) {
    std::random_device rd;
    rng_.seed(rd());
    std::uniform_real_distribution<double> init(minV_, maxV_);
    value_ = init(rng_);
  }

  double next() {
    value_ = std::clamp(value_ + step_(rng_), minV_, maxV_);
    return value_;
  }

private:
  double minV_;
  double maxV_;
  std::mt19937 rng_;
  std::normal_distribution<double> step_;
  double value_ = 0.0;
};

}  // namespace

int NcursesUi::run(Config& cfg, bool debugMode) {
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  nodelay(stdscr, TRUE);
  curs_set(0);

  if (has_colors()) {
    start_color();
    use_default_colors();
    // Pair 1: F-key blocks with background color.
    init_pair(1, COLOR_BLACK, COLOR_CYAN);
    // Pair 2: highlighted hot letters.
    init_pair(2, COLOR_YELLOW, -1);
    // Pair 3: top title bar background (AI-Z).
    init_pair(3, COLOR_BLACK, COLOR_GREEN);
    // Pair 4: section titles + dash separators (light blue).
    init_pair(4, COLOR_CYAN, -1);
    // Pair 5: usage values in section titles (light green).
    init_pair(5, COLOR_GREEN, -1);
  }

  Screen screen = Screen::Timelines;

  CpuUsageCollector cpuCol;
  GpuUsageCollector gpuCol;
  DiskBandwidthCollector diskCol;
  PcieBandwidthCollector pcieCol;
  PcieRxBandwidthCollector pcieRxCol;
  PcieTxBandwidthCollector pcieTxCol;

  // Debug generators (used only when --debug is passed)
  RandomWalk dbgCpu(0.0, 100.0, 10.0);
  RandomWalk dbgGpu(0.0, 100.0, 12.0);
  RandomWalk dbgDisk(0.0, 3000.0, 250.0);     // MB/s
  RandomWalk dbgPcieRx(0.0, 32000.0, 2500.0);  // MB/s (placeholder)
  RandomWalk dbgPcieTx(0.0, 32000.0, 2500.0);  // MB/s (placeholder)
  RandomWalk dbgRamPct(0.0, 100.0, 6.0);
  RandomWalk dbgVramPct(0.0, 100.0, 8.0);
  RandomWalk dbgGpuW(5.0, 350.0, 20.0);
  RandomWalk dbgGpuTemp(25.0, 92.0, 6.0);

  unsigned int gpuCount = 1;
  if (!debugMode) {
    if (const auto n = nvmlGpuCount(); n && *n > 0) {
      gpuCount = *n;
    }
  }

  std::vector<std::optional<GpuTelemetry>> cachedGpuTel;
  cachedGpuTel.resize(gpuCount);
  auto lastGpuTelQuery = std::chrono::steady_clock::now() - std::chrono::seconds(10);

  Timeline cpuTl(cfg.timelineSamples);
  Timeline ramTl(cfg.timelineSamples);
  Timeline vramTl(cfg.timelineSamples);
  std::vector<Timeline> gpuTls;
  gpuTls.reserve(gpuCount);
  for (unsigned int i = 0; i < gpuCount; ++i) {
    gpuTls.emplace_back(cfg.timelineSamples);
  }
  Timeline diskTl(cfg.timelineSamples);
  Timeline pcieRxTl(cfg.timelineSamples);
  Timeline pcieTxTl(cfg.timelineSamples);

  std::vector<std::unique_ptr<IBenchmark>> benches;
  benches.push_back(makePcieBandwidthBenchmark());
  benches.push_back(makeFlopsBenchmark());
  benches.push_back(makeTorchMatmulBenchmark());

  int benchSelected = 0;
  int configSelected = 0;
  std::string lastBenchResult;
  std::vector<std::string> benchResults;
  benchResults.resize(benches.size());

  HardwareInfo hwCache = HardwareInfo::probe();
  std::vector<std::string> hwLines = hwCache.toLines();

  bool running = true;
  while (running) {
    int rows = 0, cols = 0;
    getmaxyx(stdscr, rows, cols);

    // Ensure we can render a full-width scrolling graph without trailing empty columns.
    const std::size_t desiredSamples = std::max<std::size_t>(cfg.timelineSamples, static_cast<std::size_t>(std::max(0, cols)));
    ensureTimelineCapacity(cpuTl, desiredSamples);
    ensureTimelineCapacity(ramTl, desiredSamples);
    ensureTimelineCapacity(vramTl, desiredSamples);
    for (auto& tl : gpuTls) ensureTimelineCapacity(tl, desiredSamples);
    ensureTimelineCapacity(diskTl, desiredSamples);
    ensureTimelineCapacity(pcieRxTl, desiredSamples);
    ensureTimelineCapacity(pcieTxTl, desiredSamples);

    std::optional<Sample> cpu;
    std::optional<Sample> disk;
    std::optional<RamUsage> ram;
    std::optional<Sample> ramPct;
    std::optional<Sample> vramPct;
    std::optional<Sample> pcieRx;
    std::optional<Sample> pcieTx;
    std::vector<std::optional<Sample>> gpuSamples;
    gpuSamples.resize(gpuCount);
    std::vector<std::optional<GpuTelemetry>> gpuTel;
    gpuTel.resize(gpuCount);

    if (debugMode) {
      cpu = Sample{dbgCpu.next(), "%", "debug"};
      disk = Sample{dbgDisk.next(), "MB/s", "debug"};
      pcieRx = Sample{dbgPcieRx.next(), "MB/s", "debug"};
      pcieTx = Sample{dbgPcieTx.next(), "MB/s", "debug"};

      const double totalRam = readRamTotalGiB().value_or(16.0);
      const double ramPctV = dbgRamPct.next();
      const double usedRam = (ramPctV / 100.0) * totalRam;
      ram = RamUsage{usedRam, totalRam, ramPctV};
      ramPct = Sample{ramPctV, "%", "debug"};

      // Per-GPU telemetry (debug): only GPU0.
      const double util = dbgGpu.next();
      gpuSamples[0] = Sample{util, "%", "debug"};

      GpuTelemetry gt;
      gt.utilPct = util;
      const double totalVram = 10.0;  // placeholder
      const double vramPctV = dbgVramPct.next();
      const double usedVram = (vramPctV / 100.0) * totalVram;
      gt.vramUsedGiB = usedVram;
      gt.vramTotalGiB = totalVram;
      vramPct = Sample{vramPctV, "%", "debug"};
      gt.watts = dbgGpuW.next();
      gt.tempC = dbgGpuTemp.next();
      std::string ps = "P8";
      if (util > 80.0) ps = "P0";
      else if (util > 50.0) ps = "P2";
      else if (util > 20.0) ps = "P5";
      gt.pstate = ps;
      gpuTel[0] = gt;
    } else {
      cpu = cpuCol.sample();
      disk = diskCol.sample();
      ram = readRamUsage();

      pcieRx = pcieRxCol.sample();
      pcieTx = pcieTxCol.sample();

      // GPU utilization: if NVML is available, use per-GPU telemetry; otherwise fall back
      // to the collector (may be unavailable).
      const bool hasNvml = (nvmlGpuCount().value_or(0) > 0);

      if (!hasNvml) {
        gpuSamples[0] = gpuCol.sample();
      }

      // GPU telemetry (best-effort, cached)
      const auto now = std::chrono::steady_clock::now();
      if (now - lastGpuTelQuery > std::chrono::milliseconds(900)) {
        for (unsigned int i = 0; i < gpuCount; ++i) {
          cachedGpuTel[static_cast<std::size_t>(i)] = readGpuTelemetryPreferNvml(i);
        }
        lastGpuTelQuery = now;
      }
      gpuTel = cachedGpuTel;

      if (hasNvml) {
        for (unsigned int i = 0; i < gpuCount; ++i) {
          const auto& gt = gpuTel[static_cast<std::size_t>(i)];
          if (gt && gt->utilPct) {
            gpuSamples[static_cast<std::size_t>(i)] = Sample{*gt->utilPct, "%", "nvml"};
          }
        }
      }

      if (ram) {
        ramPct = Sample{ram->usedPct, "%", "os"};
      }

      // Aggregate VRAM usage percentage across GPUs when available.
      double sumUsed = 0.0;
      double sumTotal = 0.0;
      for (const auto& gt : gpuTel) {
        if (!gt || !gt->vramUsedGiB || !gt->vramTotalGiB) continue;
        sumUsed += *gt->vramUsedGiB;
        sumTotal += *gt->vramTotalGiB;
      }
      if (sumTotal > 0.0) {
        vramPct = Sample{(100.0 * sumUsed / sumTotal), "%", "nvml"};
      }
    }

    // Always advance timelines so the graphs keep scrolling.
    cpuTl.push(cpu ? cpu->value : 0.0);
    ramTl.push(ramPct ? ramPct->value : 0.0);
    vramTl.push(vramPct ? vramPct->value : 0.0);
    for (unsigned int i = 0; i < gpuCount; ++i) {
      const auto& s = gpuSamples[static_cast<std::size_t>(i)];
      gpuTls[static_cast<std::size_t>(i)].push(s ? s->value : 0.0);
    }
    diskTl.push(disk ? disk->value : 0.0);
    pcieRxTl.push(pcieRx ? pcieRx->value : 0.0);
    pcieTxTl.push(pcieTx ? pcieTx->value : 0.0);

    erase();

    if (screen == Screen::Timelines) {
      // Custom title bar: green labels, black value background.
      drawTitleBarTimelines(cols, cpu, pcieRx, pcieTx, ram, gpuTel);
      drawTimelines(rows, cols, cfg, hwCache.cpuName, hwCache.gpuName, cpu, ramPct, vramPct, gpuSamples, disk, pcieRx, pcieTx, cpuTl, ramTl, vramTl, gpuTls, diskTl, pcieRxTl, pcieTxTl);
    } else if (screen == Screen::Help) {
      drawHeader(cols, "AI-Z - Help");
      drawHelp(rows, cols);
    } else if (screen == Screen::Benchmarks) {
      // Show the same basic metrics header as the main screen.
      drawTitleBarTimelines(cols, cpu, pcieRx, pcieTx, ram, gpuTel);
      drawBenchmarks(rows, cols, benchSelected, benches, benchResults, lastBenchResult);
    } else if (screen == Screen::Config) {
      drawHeader(cols, "AI-Z - Config");
      drawConfig(rows, cols, configSelected, cfg);
    } else {
      drawHeader(cols, "AI-Z - Hardware");
      drawHardware(rows, cols, hwLines);
    }

    drawFooter(rows, cols, screen);
    refresh();

    const int ch = getch();
    if (ch != ERR) {
      // Function keys (htop-style)
      if (ch == KEY_F(1)) screen = Screen::Help;
      else if (ch == KEY_F(2)) {
        hwCache = HardwareInfo::probe();
        hwLines = hwCache.toLines();
        screen = Screen::Hardware;
      }
      else if (ch == KEY_F(3)) screen = Screen::Benchmarks;
      else if (ch == KEY_F(4)) screen = Screen::Config;
      else if (ch == KEY_F(10)) running = false;

      // Letter shortcuts
      else if (ch == 'h' || ch == 'H') screen = Screen::Help;
      else if (ch == 'w' || ch == 'W') {
        hwCache = HardwareInfo::probe();
        hwLines = hwCache.toLines();
        screen = Screen::Hardware;
      }
      else if (ch == 'b' || ch == 'B') screen = Screen::Benchmarks;
      else if (ch == 'c' || ch == 'C') screen = Screen::Config;
      else if (ch == 'q' || ch == 'Q') running = false;

      else if (ch == 27) screen = Screen::Timelines;  // ESC
      else {
        if (screen == Screen::Benchmarks) {
          const int maxSel = static_cast<int>(benches.size());  // +1 for "run all" (index 0)
          if (ch == KEY_UP) benchSelected = std::max(0, benchSelected - 1);
          else if (ch == KEY_DOWN) benchSelected = std::min(maxSel, benchSelected + 1);
          else if (ch == '\n' || ch == KEY_ENTER) {
            if (benchSelected == 0) {
              // Run all benchmarks.
              lastBenchResult = "Running all...";
              for (std::size_t i = 0; i < benches.size(); ++i) {
                auto& b = benches[i];
                const BenchResult r = b->run();
                benchResults[i] = r.ok ? r.summary : ("FAIL: " + r.summary);
              }
              lastBenchResult = "Done.";
            } else {
              const int benchIndex = benchSelected - 1;
              auto& b = benches[static_cast<std::size_t>(benchIndex)];
              const BenchResult r = b->run();
              benchResults[static_cast<std::size_t>(benchIndex)] = r.ok ? r.summary : ("FAIL: " + r.summary);
              lastBenchResult = r.ok ? ("OK: " + r.summary) : ("FAIL: " + r.summary);
            }
          }
        } else if (screen == Screen::Config) {
          if (ch == KEY_UP) configSelected = std::max(0, configSelected - 1);
          else if (ch == KEY_DOWN) configSelected = std::min(6, configSelected + 1);
          else if (ch == ' ') {
            switch (configSelected) {
              case 0: cfg.showCpu = !cfg.showCpu; break;
              case 1: cfg.showGpu = !cfg.showGpu; break;
              case 2: cfg.showDisk = !cfg.showDisk; break;
              case 3: cfg.showPcieRx = !cfg.showPcieRx; break;
              case 4: cfg.showPcieTx = !cfg.showPcieTx; break;
              case 5: cfg.showRam = !cfg.showRam; break;
              case 6: cfg.showVram = !cfg.showVram; break;
            }
          } else if (ch == 's') {
            cfg.save();
          }
        } else if (screen == Screen::Hardware) {
          if (ch == 'r') {
            hwCache = HardwareInfo::probe();
            hwLines = hwCache.toLines();
          }
        }
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.refreshMs));
  }

  endwin();
  return 0;
}

}  // namespace aiz
