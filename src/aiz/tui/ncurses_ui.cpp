#include <aiz/tui/ncurses_ui.h>

#include <aiz/bench/factory.h>
#include <aiz/hw/hardware_info.h>
#include <aiz/metrics/cpu_usage.h>
#include <aiz/metrics/disk_bandwidth.h>
#include <aiz/metrics/network_bandwidth.h>
#include <aiz/metrics/gpu_usage.h>
#include <aiz/metrics/gpu_memory_util.h>
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
#include <cstring>
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

static void drawFooter(int rows, int cols, Screen screen, std::uint32_t refreshMs) {
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
  addPlain("AI-Z  ");

  addKeyBlock(" +/- ");
  addPlain(" Speed ");
  addPlain(std::to_string(refreshMs));
  addPlain("ms  ");

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
  std::optional<unsigned int> pcieLinkWidth;
  std::optional<unsigned int> pcieLinkGen;
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
  GpuTelemetry t;
  bool any = false;

  if (const auto nv = readNvmlTelemetryForGpu(index)) {
    t.utilPct = nv->gpuUtilPct;
    t.vramUsedGiB = nv->memUsedGiB;
    t.vramTotalGiB = nv->memTotalGiB;
    t.watts = nv->powerWatts;
    t.tempC = nv->tempC;
    t.pstate = nv->pstate;
    any = true;
  }

  // Query PCIe link info independently: telemetry calls can fail while link queries still work.
  if (const auto link = readNvmlPcieLinkForGpu(index)) {
    t.pcieLinkWidth = link->width;
    t.pcieLinkGen = link->generation;
    any = true;
  }

  if (any) return t;

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

static int drawTitleBarTimelines(int cols,
                                  const std::optional<Sample>& cpu,
                                  bool showDiskRead,
                                  bool showDiskWrite,
                                  const std::optional<Sample>& diskRead,
                                  const std::optional<Sample>& diskWrite,
                                  bool showNetRx,
                                  bool showNetTx,
                                  const std::optional<Sample>& netRx,
                                  const std::optional<Sample>& netTx,
                                  const std::optional<Sample>& pcieRx,
                                  const std::optional<Sample>& pcieTx,
                                  const std::optional<RamUsage>& ram,
                                  const std::vector<std::optional<GpuTelemetry>>& gpus) {
  const bool colors = has_colors() != 0;
  const int headerRows = 1 + static_cast<int>(gpus.size());

  for (int row = 0; row < headerRows; ++row) {
    mvhline(row, 0, ' ', cols);
  }

  enum class Align {
    Left,
    Right,
  };

  auto fit = [](std::string s, std::size_t w, Align a) {
    if (s.size() > w) s.resize(w);
    if (s.size() < w) {
      const std::size_t pad = w - s.size();
      if (a == Align::Right) s = std::string(pad, ' ') + s;
      else s += std::string(pad, ' ');
    }
    return s;
  };

  auto fmtSampleOrDash = [&](const std::optional<Sample>& s, bool enabled) {
    if (!enabled) return std::string("--");
    if (!s) return std::string("--");
    return fmt1(s->value) + s->unit;
  };

  auto addPlain = [&](int row, int& x, const std::string& s) {
    if (x >= cols) return;
    mvaddnstr(row, x, s.c_str(), cols - x);
    x += static_cast<int>(s.size());
  };

  auto addLabel = [&](int row, int& x, const std::string& labelWithColon) {
    if (x >= cols) return;
    if (colors) attron(COLOR_PAIR(4) | A_BOLD);
    addPlain(row, x, labelWithColon);
    if (colors) attroff(COLOR_PAIR(4) | A_BOLD);
    addPlain(row, x, " ");
  };

  auto addValueField = [&](int row, int& x, const std::string& value, std::size_t width, Align align) {
    addPlain(row, x, fit(value, width, align));
    addPlain(row, x, "  ");
  };

  const std::string cpuStr = cpu ? (fmt0(cpu->value) + "%") : std::string("--");
  std::string ramStr = "--";
  if (ram) {
    ramStr = fmt1(ram->usedGiB) + "/" + fmt1(ram->totalGiB) + "G(" + fmt0(ram->usedPct) + "%)";
  }
  // Header is independent of timeline toggles: show values when available.
  const std::string diskStr = fmtSampleOrDash(diskRead, true) + "/" + fmtSampleOrDash(diskWrite, true);
  const std::string netStr = fmtSampleOrDash(netRx, true) + "/" + fmtSampleOrDash(netTx, true);

  // Line 1: CPU: RAM: DISKr/W: Net R/T:
  {
    int x = 0;
    addLabel(0, x, "CPU:");
    addValueField(0, x, cpuStr, 4, Align::Right);

    addLabel(0, x, "RAM:");
    addValueField(0, x, ramStr, 18, Align::Left);

    addLabel(0, x, "DISK R/W:");
    addValueField(0, x, diskStr, 24, Align::Left);

    addLabel(0, x, "NET R/T:");
    addValueField(0, x, netStr, 24, Align::Left);
  }

  // Lines 2..N: one line per GPU present
  for (std::size_t i = 0; i < gpus.size(); ++i) {
    const auto& gt = gpus[i];
    const int row = static_cast<int>(1 + i);
    int x = 0;

    addLabel(row, x, "GPU " + std::to_string(i) + ":");

    const std::string usageStr = (gt && gt->utilPct) ? (fmt0(*gt->utilPct) + "%") : std::string("--");
    addLabel(row, x, "USAGE");
    addValueField(row, x, usageStr, 4, Align::Right);

    std::string vramStr = "--";
    if (gt && gt->vramUsedGiB && gt->vramTotalGiB) {
      const double used = *gt->vramUsedGiB;
      const double total = *gt->vramTotalGiB;
      const double pct = total > 0.0 ? (100.0 * used / total) : 0.0;
      vramStr = fmt1(used) + "/" + fmt1(total) + "G(" + fmt0(pct) + "%)";
    }
    addLabel(row, x, "VRAM:");
    addValueField(row, x, vramStr, 18, Align::Left);

    const std::string wattsStr = (gt && gt->watts) ? (fmt0(*gt->watts) + "W") : std::string("--");
    addLabel(row, x, "W:");
    addValueField(row, x, wattsStr, 5, Align::Right);

    const std::string tempStr = (gt && gt->tempC) ? (fmt0(*gt->tempC) + "C") : std::string("--");
    addLabel(row, x, "T:");
    addValueField(row, x, tempStr, 4, Align::Right);

    const std::string powerStr = (gt && !gt->pstate.empty()) ? gt->pstate : std::string("--");
    addLabel(row, x, "POWER:");
    addValueField(row, x, powerStr, 4, Align::Left);

    std::string linkStr = "--";
    if (gt && gt->pcieLinkWidth && gt->pcieLinkGen) {
      linkStr = std::to_string(*gt->pcieLinkWidth) + "x@" + fmt1(static_cast<double>(*gt->pcieLinkGen));
    }
    addLabel(row, x, "LINK:");
    addValueField(row, x, linkStr, 9, Align::Left);

    const std::string pcieTStr = pcieTx ? (fmt0(pcieTx->value) + pcieTx->unit) : std::string("--");
    const std::string pcieRStr = pcieRx ? (fmt0(pcieRx->value) + pcieRx->unit) : std::string("--");
    addLabel(row, x, "PCIE T/R:");
    addValueField(row, x, pcieTStr + "/" + pcieRStr, 22, Align::Left);
  }

  return headerRows;
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
    int headerRows,
    const Config& cfg,
  const std::string& cpuDevice,
  const std::string& gpuDevice,
    const std::optional<Sample>& cpu,
    const std::optional<Sample>& ram,
    const std::optional<Sample>& vram,
    const std::optional<Sample>& gpuMemUtil,
    const std::vector<std::optional<Sample>>& gpus,
    const std::optional<Sample>& disk,
    const std::optional<Sample>& diskRead,
    const std::optional<Sample>& diskWrite,
    const std::optional<Sample>& netRx,
    const std::optional<Sample>& netTx,
    const std::optional<Sample>& pcieRx,
    const std::optional<Sample>& pcieTx,
    const Timeline& cpuTl,
    const Timeline& ramTl,
    const Timeline& vramTl,
    const Timeline& gpuMemUtilTl,
    const std::vector<Timeline>& gpuTls,
    const Timeline& diskTl,
    const Timeline& diskReadTl,
    const Timeline& diskWriteTl,
    const Timeline& netRxTl,
    const Timeline& netTxTl,
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

  // GPU memory controller utilization (NVML util.memory). This is a percentage, not bandwidth.
  panels.push_back(Panel{"MemCtrl", cfg.showGpuMem, &gpuMemUtil, &gpuMemUtilTl, 100.0, gpuDevice});

  if (cfg.showGpu) {
    const std::size_t n = std::min(gpus.size(), gpuTls.size());
    for (std::size_t i = 0; i < n; ++i) {
      const std::string name = "GPU" + std::to_string(i);
      std::string dev = gpuDevice;
      if (!dev.empty()) dev += " #" + std::to_string(i);
      panels.push_back(Panel{name, true, &gpus[i], &gpuTls[i], 100.0, dev});
    }
  }

  panels.push_back(Panel{"DiskR", cfg.showDiskRead, &diskRead, &diskReadTl, 5000.0, std::string{}});
  panels.push_back(Panel{"DiskW", cfg.showDiskWrite, &diskWrite, &diskWriteTl, 5000.0, std::string{}});
  panels.push_back(Panel{"NetRX", cfg.showNetRx, &netRx, &netRxTl, 5000.0, std::string{}});
  panels.push_back(Panel{"NetTX", cfg.showNetTx, &netTx, &netTxTl, 5000.0, std::string{}});
  // PCIe samples are in MB/s (from NVML).
  panels.push_back(Panel{"PCIe RX", cfg.showPcieRx, &pcieRx, &pcieRxTl, 32'000.0, std::string{}});
  panels.push_back(Panel{"PCIe TX", cfg.showPcieTx, &pcieTx, &pcieTxTl, 32'000.0, std::string{}});

  panels.erase(
      std::remove_if(panels.begin(), panels.end(), [](const Panel& p) { return !p.enabled; }),
      panels.end());

  if (panels.empty()) {
    mvaddnstr(headerRows, 0, "No timelines enabled. Use Config (F4 / C) to enable.", cols);
    return;
  }

  // Start immediately under the header bar.
  const int top = headerRows;
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
    } else if (p.name == "Disk" || p.name == "DiskR" || p.name == "DiskW") {
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
    int headerRows,
    int selected,
    const std::vector<std::string>& rowTitles,
    const std::vector<bool>& rowIsHeader,
    const std::vector<std::unique_ptr<IBenchmark>>& rowBenches,
    const std::vector<std::string>& rowResults) {
  const bool colors = has_colors() != 0;
  int y = headerRows;

  auto placeholderForName = [&](const std::string& n) -> std::string {
    if (n.find("PCIe") != std::string::npos || n.find("PCI") != std::string::npos) return "-- GB/s";
    if (n.find("FP") != std::string::npos || n.find("FLOPS") != std::string::npos) return "-- GFLOPS";
    if (n.find("INT") != std::string::npos) return "-- GOPS";
    return "--";
  };

  auto resultTextForRow = [&](int row) -> std::string {
    if (row < 0 || row >= static_cast<int>(rowResults.size())) return "--";
    const auto& s = rowResults[static_cast<std::size_t>(row)];
    if (!s.empty()) return s;
    return placeholderForName(rowTitles[static_cast<std::size_t>(row)]);
  };

  // Special entry: run all.
  {
    // Spacer between the header bar and the first action.
    if (y < rows - 2) {
      mvhline(y, 0, ' ', cols);
      ++y;
    }

    const std::string prefix = (selected == 0 ? "> " : "  ");
    const std::string label = "Run all benchmarks";
    mvhline(y, 0, ' ', cols);
    mvaddnstr(y, 0, prefix.c_str(), cols);
    if (colors) attron(COLOR_PAIR(6) | A_BOLD);
    if (static_cast<int>(prefix.size()) < cols) {
      mvaddnstr(y, static_cast<int>(prefix.size()), label.c_str(), cols - static_cast<int>(prefix.size()));
    }
    if (colors) attroff(COLOR_PAIR(6) | A_BOLD);
    ++y;
    // Blank spacer line between "run all" and individual benches.
    if (y < rows - 2) {
      mvhline(y, 0, ' ', cols);
      ++y;
    }
  }

  for (int row = 0; row < static_cast<int>(rowTitles.size()); ++row) {
    const int rowIndex = row + 1;  // shifted by "run all"
    const bool isHeader = rowIsHeader[static_cast<std::size_t>(row)];
    const auto& b = rowBenches[static_cast<std::size_t>(row)];
    mvhline(y, 0, ' ', cols);

    const std::string prefix = (!isHeader && rowIndex == selected) ? "> " : "  ";
    const std::string name = rowTitles[static_cast<std::size_t>(row)];
    const bool avail = (!isHeader && b && b->isAvailable());

    mvaddnstr(y, 0, prefix.c_str(), cols);

    // Headers: just a section title line.
    if (isHeader) {
      if (colors) attron(COLOR_PAIR(4) | A_BOLD);
      mvaddnstr(y, static_cast<int>(prefix.size()), name.c_str(), std::max(0, cols - static_cast<int>(prefix.size())));
      if (colors) attroff(COLOR_PAIR(4) | A_BOLD);
      ++y;
      continue;
    }

    // Benchmark rows: show "NAME: value" on a single line.
    std::string r;
    if (b && !b->isAvailable()) {
      r = "unavailable";
    } else {
      r = resultTextForRow(row);
    }

    const std::size_t end0 = r.find('\n');
    const std::string firstLine = (end0 == std::string::npos) ? r : r.substr(0, end0);
    constexpr int kMetricIndent = 4;
    const std::string indent(kMetricIndent, ' ');
    const std::string namePart = indent + name;
    const std::string valuePart = ": " + firstLine;

    // Draw name (styled) and value (default) separately so values stay white.
    int x = static_cast<int>(prefix.size());
    if (colors && avail) attron(COLOR_PAIR(4) | A_BOLD);
    if (x < cols) mvaddnstr(y, x, namePart.c_str(), cols - x);
    if (colors && avail) attroff(COLOR_PAIR(4) | A_BOLD);
    x += static_cast<int>(namePart.size());
    if (x < cols) mvaddnstr(y, x, valuePart.c_str(), cols - x);
    ++y;

    // If the result has extra lines (errors/details), show them underneath indented.
    if (end0 != std::string::npos) {
      std::size_t start = end0 + 1;
      while (start <= r.size() && y < rows - 2) {
        const std::size_t end = r.find('\n', start);
        const std::string extra = (end == std::string::npos) ? r.substr(start) : r.substr(start, end - start);
        mvhline(y, 0, ' ', cols);
        const int ix = static_cast<int>(prefix.size()) + 2;
        if (ix < cols) mvaddnstr(y, ix, extra.c_str(), cols - ix);
        ++y;
        if (end == std::string::npos) break;
        start = end + 1;
      }
    }
  }

  mvhline(rows - 3, 0, ' ', cols);
  mvaddnstr(rows - 3, 0, "Enter: run   Esc: back", cols);
}

static void drawConfig(int rows, int cols, int selected, const Config& cfg) {
  struct Item { const char* label; bool value; };
  const Item items[] = {
      {"CPU usage", cfg.showCpu},
      {"GPU usage", cfg.showGpu},
      {"GPU memory util", cfg.showGpuMem},
      {"Disk read", cfg.showDiskRead},
      {"Disk write", cfg.showDiskWrite},
      {"Net RX", cfg.showNetRx},
      {"Net TX", cfg.showNetTx},
      {"PCIe RX", cfg.showPcieRx},
      {"PCIe TX", cfg.showPcieTx},
      {"RAM usage", cfg.showRam},
      {"VRAM usage", cfg.showVram},
  };

  const bool colors = has_colors() != 0;

  // Section header
  int y = 2;
  mvhline(y, 0, ' ', cols);
  if (colors) attron(COLOR_PAIR(4) | A_BOLD);
  mvaddnstr(y, 0, "Timelines", cols);
  if (colors) attroff(COLOR_PAIR(4) | A_BOLD);
  ++y;

  std::size_t maxLabelLen = 0;
  for (const auto& it : items) {
    maxLabelLen = std::max(maxLabelLen, std::strlen(it.label));
  }

  const int prefixW = 2;  // "> " or "  "
  const int colonCol = prefixW + static_cast<int>(maxLabelLen);
  const int valueCol = colonCol + 2;  // ": "

  constexpr int kCount = static_cast<int>(sizeof(items) / sizeof(items[0]));
  for (int i = 0; i < kCount; ++i) {
    mvhline(y, 0, ' ', cols);
    const char* prefix = (i == selected ? "> " : "  ");
    if (0 < cols) mvaddnstr(y, 0, prefix, cols);
    if (prefixW < cols) mvaddnstr(y, prefixW, items[i].label, cols - prefixW);

    // Pad label area so the ':' aligns.
    const int labelEnd = prefixW + static_cast<int>(std::strlen(items[i].label));
    for (int x = labelEnd; x < colonCol && x < cols; ++x) {
      mvaddch(y, x, ' ');
    }

    if (colonCol < cols) mvaddch(y, colonCol, ':');
    if (colonCol + 1 < cols) mvaddch(y, colonCol + 1, ' ');
    if (valueCol < cols) mvaddnstr(y, valueCol, (items[i].value ? "ON" : "OFF"), cols - valueCol);
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
    // Pair 6: warning / emphasis (light red).
    init_pair(6, COLOR_RED, -1);
  }

  Screen screen = Screen::Timelines;

  CpuUsageCollector cpuCol;
  GpuUsageCollector gpuCol;
  GpuMemoryUtilCollector gpuMemUtilCol;
  DiskBandwidthCollector diskCol;
  DiskBandwidthCollector diskReadCol(DiskBandwidthMode::Read);
  DiskBandwidthCollector diskWriteCol(DiskBandwidthMode::Write);
  NetworkBandwidthCollector netRxCol(NetworkBandwidthMode::Rx);
  NetworkBandwidthCollector netTxCol(NetworkBandwidthMode::Tx);
  PcieBandwidthCollector pcieCol;
  PcieRxBandwidthCollector pcieRxCol;
  PcieTxBandwidthCollector pcieTxCol;

  // Debug generators (used only when --debug is passed)
  RandomWalk dbgCpu(0.0, 100.0, 10.0);
  RandomWalk dbgGpu(0.0, 100.0, 12.0);
  RandomWalk dbgDisk(0.0, 3000.0, 250.0);     // MB/s
  RandomWalk dbgDiskR(0.0, 3000.0, 250.0);    // MB/s
  RandomWalk dbgDiskW(0.0, 3000.0, 250.0);    // MB/s
  RandomWalk dbgNetRx(0.0, 5000.0, 350.0);    // MB/s
  RandomWalk dbgNetTx(0.0, 5000.0, 350.0);    // MB/s
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
  Timeline gpuMemUtilTl(cfg.timelineSamples);
  std::vector<Timeline> gpuTls;
  gpuTls.reserve(gpuCount);
  for (unsigned int i = 0; i < gpuCount; ++i) {
    gpuTls.emplace_back(cfg.timelineSamples);
  }
  Timeline diskTl(cfg.timelineSamples);
  Timeline diskReadTl(cfg.timelineSamples);
  Timeline diskWriteTl(cfg.timelineSamples);
  Timeline netRxTl(cfg.timelineSamples);
  Timeline netTxTl(cfg.timelineSamples);
  Timeline pcieRxTl(cfg.timelineSamples);
  Timeline pcieTxTl(cfg.timelineSamples);

  HardwareInfo hwCache = HardwareInfo::probe();
  std::vector<std::string> hwLines = hwCache.toLines();

  auto parseGpuNames = [](const HardwareInfo& hw, unsigned int gpuCount) {
    std::vector<std::string> names;
    names.resize(gpuCount);
    for (unsigned int i = 0; i < gpuCount; ++i) names[i] = "GPU" + std::to_string(i);
    for (const auto& l : hw.perGpuLines) {
      // Format: "GPU0: <name>".
      if (l.rfind("GPU", 0) != 0) continue;
      const std::size_t sep = l.find(": ");
      if (sep == std::string::npos) continue;
      const std::string left = l.substr(0, sep);
      const std::string right = l.substr(sep + 2);
      if (left.size() < 4) continue;
      try {
        const unsigned int idx = static_cast<unsigned int>(std::stoul(left.substr(3)));
        if (idx < names.size() && !right.empty()) names[idx] = right;
      } catch (...) {
        continue;
      }
    }
    return names;
  };

  std::vector<std::string> benchRowTitles;
  std::vector<bool> benchRowIsHeader;
  std::vector<std::unique_ptr<IBenchmark>> benchRowBenches;
  std::vector<std::string> benchRowResults;

  auto rebuildBenchRows = [&]() {
    benchRowTitles.clear();
    benchRowIsHeader.clear();
    benchRowBenches.clear();
    benchRowResults.clear();

    const std::vector<std::string> gpuNames = parseGpuNames(hwCache, gpuCount);

    auto addHeader = [&](const std::string& title) {
      benchRowTitles.push_back(title);
      benchRowIsHeader.push_back(true);
      benchRowBenches.push_back(nullptr);
      benchRowResults.emplace_back();
    };

    auto addBench = [&](std::unique_ptr<IBenchmark> b) {
      const std::string title = b ? b->name() : std::string("(null)");
      benchRowTitles.push_back(title);
      benchRowIsHeader.push_back(false);
      benchRowBenches.push_back(std::move(b));
      benchRowResults.emplace_back();
    };

    for (unsigned int gi = 0; gi < gpuCount; ++gi) {
      addHeader("GPU" + std::to_string(gi) + " - " + gpuNames[static_cast<std::size_t>(gi)]);
      addBench(makeGpuPcieRxBenchmark(gi));
      addBench(makeGpuPcieTxBenchmark(gi));
      addBench(makeGpuFp16Benchmark(gi));
      addBench(makeGpuFp32Benchmark(gi));
      addBench(makeGpuFp64Benchmark(gi));
      addBench(makeGpuInt4Benchmark(gi));
      addBench(makeGpuInt8Benchmark(gi));
    }

    addHeader("CPU0 - " + (hwCache.cpuName.empty() ? std::string("unknown") : hwCache.cpuName));
    addBench(makeCpuFp16FlopsBenchmark());
    addBench(makeCpuFp32FlopsBenchmark());
  };

  rebuildBenchRows();

  auto isSelectableBenchRow = [&](int selectedRow) {
    if (selectedRow <= 0) return true;  // 0 = run all
    const int row = selectedRow - 1;
    if (row < 0 || row >= static_cast<int>(benchRowIsHeader.size())) return false;
    return !benchRowIsHeader[static_cast<std::size_t>(row)];
  };

  auto clampBenchSelected = [&](int sel) {
    const int maxSel = static_cast<int>(benchRowTitles.size());
    sel = std::clamp(sel, 0, maxSel);
    if (isSelectableBenchRow(sel)) return sel;

    // Find nearest selectable row (prefer downward).
    for (int d = 1; d <= maxSel; ++d) {
      const int down = sel + d;
      if (down <= maxSel && isSelectableBenchRow(down)) return down;
      const int up = sel - d;
      if (up >= 0 && isSelectableBenchRow(up)) return up;
    }
    return 0;
  };

  int benchSelected = clampBenchSelected(0);
  int configSelected = 0;
  std::string lastBenchResult;

  bool running = true;

  constexpr std::uint32_t kRefreshMinMs = 50;
  constexpr std::uint32_t kRefreshMaxMs = 5000;

  auto adjustRefreshMs = [&](bool faster) {
    const std::uint32_t cur = cfg.refreshMs;
    const std::uint32_t step = std::max<std::uint32_t>(10u, cur / 10u);

    std::uint64_t next = cur;
    if (faster) {
      next = (cur > step) ? (static_cast<std::uint64_t>(cur) - step) : 0ull;
    } else {
      next = static_cast<std::uint64_t>(cur) + step;
    }

    next = std::clamp<std::uint64_t>(next, kRefreshMinMs, kRefreshMaxMs);
    cfg.refreshMs = static_cast<std::uint32_t>(next);
  };

  while (running) {
    int rows = 0, cols = 0;
    getmaxyx(stdscr, rows, cols);

    // Ensure we can render a full-width scrolling graph without trailing empty columns.
    const std::size_t desiredSamples = std::max<std::size_t>(cfg.timelineSamples, static_cast<std::size_t>(std::max(0, cols)));
    ensureTimelineCapacity(cpuTl, desiredSamples);
    ensureTimelineCapacity(ramTl, desiredSamples);
    ensureTimelineCapacity(vramTl, desiredSamples);
    ensureTimelineCapacity(gpuMemUtilTl, desiredSamples);
    for (auto& tl : gpuTls) ensureTimelineCapacity(tl, desiredSamples);
    ensureTimelineCapacity(diskTl, desiredSamples);
    ensureTimelineCapacity(diskReadTl, desiredSamples);
    ensureTimelineCapacity(diskWriteTl, desiredSamples);
    ensureTimelineCapacity(netRxTl, desiredSamples);
    ensureTimelineCapacity(netTxTl, desiredSamples);
    ensureTimelineCapacity(pcieRxTl, desiredSamples);
    ensureTimelineCapacity(pcieTxTl, desiredSamples);

    std::optional<Sample> cpu;
    std::optional<Sample> disk;
    std::optional<Sample> diskRead;
    std::optional<Sample> diskWrite;
    std::optional<Sample> netRx;
    std::optional<Sample> netTx;
    std::optional<RamUsage> ram;
    std::optional<Sample> ramPct;
    std::optional<Sample> vramPct;
    std::optional<Sample> gpuMemUtil;
    std::optional<Sample> pcieRx;
    std::optional<Sample> pcieTx;
    std::vector<std::optional<Sample>> gpuSamples;
    gpuSamples.resize(gpuCount);
    std::vector<std::optional<GpuTelemetry>> gpuTel;
    gpuTel.resize(gpuCount);

    if (debugMode) {
      cpu = Sample{dbgCpu.next(), "%", "debug"};
      // Keep disk labels empty in debug so titles don't show "(debug)".
      diskRead = Sample{dbgDiskR.next(), "MB/s", ""};
      diskWrite = Sample{dbgDiskW.next(), "MB/s", ""};
      netRx = Sample{dbgNetRx.next(), "MB/s", ""};
      netTx = Sample{dbgNetTx.next(), "MB/s", ""};
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

      gpuMemUtil = Sample{util, "%", "debug"};

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

      // Even in debug mode, show real PCIe link info when NVML is available.
      if (const auto link = readNvmlPcieLinkForGpu(0)) {
        gt.pcieLinkWidth = link->width;
        gt.pcieLinkGen = link->generation;
      }
      gpuTel[0] = gt;
    } else {
      cpu = cpuCol.sample();
      diskRead = diskReadCol.sample();
      diskWrite = diskWriteCol.sample();
      netRx = netRxCol.sample();
      netTx = netTxCol.sample();
      ram = readRamUsage();

      gpuMemUtil = gpuMemUtilCol.sample();

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
    gpuMemUtilTl.push(gpuMemUtil ? gpuMemUtil->value : 0.0);
    for (unsigned int i = 0; i < gpuCount; ++i) {
      const auto& s = gpuSamples[static_cast<std::size_t>(i)];
      gpuTls[static_cast<std::size_t>(i)].push(s ? s->value : 0.0);
    }
    diskReadTl.push(diskRead ? diskRead->value : 0.0);
    diskWriteTl.push(diskWrite ? diskWrite->value : 0.0);
    netRxTl.push(netRx ? netRx->value : 0.0);
    netTxTl.push(netTx ? netTx->value : 0.0);
    pcieRxTl.push(pcieRx ? pcieRx->value : 0.0);
    pcieTxTl.push(pcieTx ? pcieTx->value : 0.0);

    erase();

    if (screen == Screen::Timelines) {
      // Custom title bar: green labels, black value background.
      const int headerRows = drawTitleBarTimelines(cols, cpu, cfg.showDiskRead, cfg.showDiskWrite, diskRead, diskWrite, cfg.showNetRx, cfg.showNetTx, netRx, netTx, pcieRx, pcieTx, ram, gpuTel);
      drawTimelines(rows, cols, headerRows, cfg, hwCache.cpuName, hwCache.gpuName, cpu, ramPct, vramPct, gpuMemUtil, gpuSamples, disk, diskRead, diskWrite, netRx, netTx, pcieRx, pcieTx, cpuTl, ramTl, vramTl, gpuMemUtilTl, gpuTls, diskTl, diskReadTl, diskWriteTl, netRxTl, netTxTl, pcieRxTl, pcieTxTl);
    } else if (screen == Screen::Help) {
      drawHeader(cols, "AI-Z - Help");
      drawHelp(rows, cols);
    } else if (screen == Screen::Benchmarks) {
      // Show the same basic metrics header as the main screen.
      const int headerRows = drawTitleBarTimelines(cols, cpu, cfg.showDiskRead, cfg.showDiskWrite, diskRead, diskWrite, cfg.showNetRx, cfg.showNetTx, netRx, netTx, pcieRx, pcieTx, ram, gpuTel);
      drawBenchmarks(rows, cols, headerRows, benchSelected, benchRowTitles, benchRowIsHeader, benchRowBenches, benchRowResults);
    } else if (screen == Screen::Config) {
      drawHeader(cols, "AI-Z - Config");
      drawConfig(rows, cols, configSelected, cfg);
    } else {
      drawHeader(cols, "AI-Z - Hardware");
      drawHardware(rows, cols, hwLines);
    }

    drawFooter(rows, cols, screen, cfg.refreshMs);
    refresh();

    // Wait up to refreshMs for input; wake immediately on keypress.
    timeout(static_cast<int>(cfg.refreshMs));
    const int ch = getch();
    if (ch != ERR) {
      // Function keys (htop-style)
      if (ch == KEY_F(1)) screen = Screen::Help;
      else if (ch == KEY_F(2)) {
        hwCache = HardwareInfo::probe();
        hwLines = hwCache.toLines();
        rebuildBenchRows();
        benchSelected = clampBenchSelected(benchSelected);
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
        rebuildBenchRows();
        benchSelected = clampBenchSelected(benchSelected);
        screen = Screen::Hardware;
      }
      else if (ch == 'b' || ch == 'B') screen = Screen::Benchmarks;
      else if (ch == 'c' || ch == 'C') screen = Screen::Config;
      else if (ch == 'q' || ch == 'Q') running = false;

      // Sampling / scroll speed
      else if (ch == '+' || ch == '=') adjustRefreshMs(true);
      else if (ch == '-' || ch == '_') adjustRefreshMs(false);

      else if (ch == 27) screen = Screen::Timelines;  // ESC
      else {
        if (screen == Screen::Benchmarks) {
          if (ch == KEY_UP) benchSelected = clampBenchSelected(benchSelected - 1);
          else if (ch == KEY_DOWN) benchSelected = clampBenchSelected(benchSelected + 1);
          else if (ch == '\n' || ch == KEY_ENTER) {
            if (benchSelected == 0) {
              // Run all benchmarks.
              lastBenchResult = "Running all...";
              for (std::size_t row = 0; row < benchRowBenches.size(); ++row) {
                if (benchRowIsHeader[row]) continue;
                auto& b = benchRowBenches[row];
                const BenchResult r = b->run();
                benchRowResults[row] = r.ok ? r.summary : ("FAIL: " + r.summary);
              }
              lastBenchResult = "Done.";
            } else {
              const int row = benchSelected - 1;
              if (row >= 0 && row < static_cast<int>(benchRowBenches.size()) && !benchRowIsHeader[static_cast<std::size_t>(row)]) {
                auto& b = benchRowBenches[static_cast<std::size_t>(row)];
                const BenchResult r = b->run();
                benchRowResults[static_cast<std::size_t>(row)] = r.ok ? r.summary : ("FAIL: " + r.summary);
                lastBenchResult = r.ok ? ("OK: " + r.summary) : ("FAIL: " + r.summary);
              } else {
                lastBenchResult = "(not runnable)";
              }
            }
          }
        } else if (screen == Screen::Config) {
          if (ch == KEY_UP) configSelected = std::max(0, configSelected - 1);
          else if (ch == KEY_DOWN) configSelected = std::min(10, configSelected + 1);
          else if (ch == ' ') {
            switch (configSelected) {
              case 0: cfg.showCpu = !cfg.showCpu; break;
              case 1: cfg.showGpu = !cfg.showGpu; break;
              case 2: cfg.showGpuMem = !cfg.showGpuMem; break;
              case 3: cfg.showDiskRead = !cfg.showDiskRead; break;
              case 4: cfg.showDiskWrite = !cfg.showDiskWrite; break;
              case 5: cfg.showNetRx = !cfg.showNetRx; break;
              case 6: cfg.showNetTx = !cfg.showNetTx; break;
              case 7: cfg.showPcieRx = !cfg.showPcieRx; break;
              case 8: cfg.showPcieTx = !cfg.showPcieTx; break;
              case 9: cfg.showRam = !cfg.showRam; break;
              case 10: cfg.showVram = !cfg.showVram; break;
            }
          } else if (ch == 's') {
            cfg.save();
          }
        } else if (screen == Screen::Hardware) {
          if (ch == 'r') {
            hwCache = HardwareInfo::probe();
            hwLines = hwCache.toLines();
            rebuildBenchRows();
            benchSelected = clampBenchSelected(benchSelected);
          }
        }
      }
    }
  }

  endwin();
  return 0;
}

}  // namespace aiz
