#include <aiz/tui/ncurses_ui.h>

#include <aiz/tui/tui_core.h>

#include <aiz/bench/factory.h>
#include <aiz/dyn/cuda.h>
#include <aiz/hw/hardware_info.h>
#include <aiz/metrics/cpu_usage.h>
#include <aiz/metrics/disk_bandwidth.h>
#include <aiz/metrics/network_bandwidth.h>
#include <aiz/metrics/gpu_usage.h>
#include <aiz/metrics/gpu_memory_util.h>
#include <aiz/metrics/linux_gpu_sysfs.h>
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
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <random>

namespace aiz {

namespace {
static std::optional<Command> keyToCommand(int key, Screen screen) {
  // Function keys (htop-style)
  if (key == KEY_F(1)) return Command::NavHelp;
  if (key == KEY_F(2)) return Command::NavHardware;
  if (key == KEY_F(3)) return Command::NavBenchmarks;
  if (key == KEY_F(4)) return Command::NavConfig;
  if (key == KEY_F(5)) return Command::NavTimelines;
  if (key == KEY_F(10)) return Command::Quit;

  // Letter shortcuts
  if (key == 'h' || key == 'H') return Command::NavHelp;
  if (key == 'w' || key == 'W') return Command::NavHardware;
  if (key == 'b' || key == 'B') return Command::NavBenchmarks;
  if (key == 'c' || key == 'C') return Command::NavConfig;
  if (key == 'q' || key == 'Q') return Command::Quit;

  // Global back
  if (key == 27) return Command::Back;  // ESC

  // Screen-local navigation
  if (key == KEY_UP) return Command::Up;
  if (key == KEY_DOWN) return Command::Down;

  if (screen == Screen::Benchmarks) {
    if (key == '\n' || key == KEY_ENTER) return Command::Activate;
  }

  if (screen == Screen::Config) {
    if (key == ' ') return Command::Toggle;
    if (key == '\n' || key == KEY_ENTER) return Command::Activate;
    if (key == 's' || key == 'S') return Command::Save;
    if (key == 'd' || key == 'D') return Command::Defaults;
  }

  if (screen == Screen::Hardware) {
    if (key == 'r' || key == 'R') return Command::Refresh;
  }

  return std::nullopt;
}

static int styleToAttr(std::uint16_t style) {
  // Ncurses color pairs are configured in NcursesUi::run().
  const bool colors = has_colors() != 0;
  if (!colors) return A_NORMAL;

  switch (static_cast<Style>(style)) {
    case Style::Header:
      // Prefer colored text on default background (avoid full-row backgrounds).
      return COLOR_PAIR(5) | A_BOLD;
    case Style::FooterKey:
      return COLOR_PAIR(4) | A_BOLD;
    case Style::Hot:
      return COLOR_PAIR(2) | A_BOLD;
    case Style::Section:
      return COLOR_PAIR(4) | A_BOLD;
    case Style::Value:
      return COLOR_PAIR(5) | A_BOLD;
    case Style::Warning:
      return COLOR_PAIR(6) | A_BOLD;
    case Style::Default:
    default:
      return A_NORMAL;
  }
}

static chtype cellToChtype(wchar_t ch) {
  // Keep ncurses rendering conservative: prefer ASCII and ACS glyphs.
  // The core renderer uses Unicode block elements; map those to ncurses ACS.
  if (ch == 0x2593) return ACS_CKBOARD;
  if (ch == 0 || ch == L' ') return static_cast<chtype>(' ');
  if (ch >= 0 && ch <= 0x7f) return static_cast<chtype>(static_cast<unsigned char>(ch));
  return static_cast<chtype>('?');
}

static void ensureTimelineCapacity(Timeline& tl, std::size_t desiredCapacity) {
  if (tl.capacity() >= desiredCapacity) return;
  Timeline resized(desiredCapacity);
  for (double v : tl.values()) {
    resized.push(v);
  }
  tl = std::move(resized);
}

static std::string fmt1(double v);
static std::string fmt0(double v);

static std::string formatRamText(const std::optional<RamUsage>& ram) {
  if (!ram) return "--";
  return fmt1(ram->usedGiB) + "/" + fmt1(ram->totalGiB) + "G(" + fmt0(ram->usedPct) + "%)";
}

static std::vector<std::string> parseGpuNames(const HardwareInfo& hw, unsigned int gpuCount) {
  std::vector<std::string> names;
  names.resize(gpuCount);
  for (unsigned int i = 0; i < gpuCount; ++i) names[i] = "GPU" + std::to_string(i);

  // Fallback: if we only have a single GPU name (legacy field), use it for GPU0.
  if (gpuCount > 0 && !hw.gpuName.empty() && hw.gpuName != "--") {
    if (names[0] == "GPU0") names[0] = hw.gpuName;
  }

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
}

static std::string probeCpuNameFast() {
#if defined(__linux__)
  std::ifstream in("/proc/cpuinfo");
  std::string line;
  while (std::getline(in, line)) {
    // Examples:
    //  "model name\t: AMD Ryzen 9 7950X3D 16-Core Processor"
    //  "Hardware\t: ..." (some ARM)
    //  "Processor\t: ..."
    // Be tolerant to whitespace/casing.
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
    const std::size_t pos = line.find(':');
    if (pos == std::string::npos) continue;

    std::string k = line.substr(0, pos);
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
    std::string kLower;
    kLower.reserve(k.size());
    for (char c : k) kLower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    const bool isModel = (kLower == "model name");
    const bool isHardware = (kLower == "hardware");
    const bool isProcessor = (kLower == "processor");
    if (!isModel && !isHardware && !isProcessor) continue;

    std::string v = line.substr(pos + 1);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    if (!v.empty()) return v;
  }
#endif
  return {};
}

static std::vector<std::string> probeGpuNamesFast(unsigned int gpuCount, bool hasNvml) {
  std::vector<std::string> names;
  names.resize(gpuCount);
  for (unsigned int i = 0; i < gpuCount; ++i) names[i] = "GPU" + std::to_string(i);

  // Prefer NVML device names when available.
  // Try the in-process variant first, but fall back to the fork/timeout wrapper:
  // some NVML setups behave better when called in the isolated child.
  if (hasNvml) {
    for (unsigned int i = 0; i < gpuCount; ++i) {
      if (const auto n = readNvmlGpuNameForGpuNoFork(i)) {
        if (!n->empty() && *n != "--") {
          names[static_cast<std::size_t>(i)] = *n;
          continue;
        }
      }
      if (const auto n = readNvmlGpuNameForGpu(i)) {
        if (!n->empty() && *n != "--") names[static_cast<std::size_t>(i)] = *n;
      }
    }
  }

  return names;
}

static std::vector<std::string> probeGpuNamesCudaFast(unsigned int desiredCount) {
  std::vector<std::string> names;
  names.resize(desiredCount);
  for (unsigned int i = 0; i < desiredCount; ++i) names[i] = "GPU" + std::to_string(i);

  std::string err;
  const auto* cu = dyn::cuda::api(&err);
  if (!cu || !cu->cuInit || !cu->cuDeviceGetCount || !cu->cuDeviceGet || !cu->cuDeviceGetName) return names;

  if (cu->cuInit(0) != dyn::cuda::CUDA_SUCCESS) return names;
  int count = 0;
  if (cu->cuDeviceGetCount(&count) != dyn::cuda::CUDA_SUCCESS || count <= 0) return names;

  const unsigned int n = std::min<unsigned int>(desiredCount, static_cast<unsigned int>(count));
  for (unsigned int i = 0; i < n; ++i) {
    dyn::cuda::CUdevice dev{};
    if (cu->cuDeviceGet(&dev, static_cast<int>(i)) != dyn::cuda::CUDA_SUCCESS) continue;
    char buf[128]{};
    if (cu->cuDeviceGetName(buf, static_cast<int>(sizeof(buf)), dev) != dyn::cuda::CUDA_SUCCESS) continue;
    const std::string s(buf);
    if (!s.empty()) names[static_cast<std::size_t>(i)] = s;
  }

  return names;
}

static bool hasRealDeviceNames(const std::vector<std::string>& names) {
  for (std::size_t i = 0; i < names.size(); ++i) {
    const std::string& n = names[i];
    if (n.empty() || n == "--" || n == "unknown") continue;
    const std::string placeholder = "GPU" + std::to_string(i);
    if (n != placeholder) return true;
  }
  return false;
}

[[maybe_unused]] static void drawFooter(int rows, int cols, Screen screen, std::uint32_t refreshMs) {
  (void)screen;
  const int y = rows - 1;
  int x = 0;

  // Avoid blanking the full footer row; let ncurses diff the previous frame.
  move(y, 0);

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

  clrtoeol();
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
  std::optional<double> memUtilPct;
  std::optional<double> vramUsedGiB;
  std::optional<double> vramTotalGiB;
  std::optional<double> watts;
  std::optional<double> tempC;
  std::string pstate;
  std::optional<unsigned int> pcieLinkWidth;
  std::optional<unsigned int> pcieLinkGen;
  std::string source;
};

static std::optional<GpuTelemetry> readGpuTelemetryPreferNvml(unsigned int index) {
  GpuTelemetry t;
  bool any = false;

  if (const auto nv = readNvmlTelemetryForGpu(index)) {
    t.utilPct = nv->gpuUtilPct;
    t.memUtilPct = nv->memUtilPct;
    t.vramUsedGiB = nv->memUsedGiB;
    t.vramTotalGiB = nv->memTotalGiB;
    t.watts = nv->powerWatts;
    t.tempC = nv->tempC;
    t.pstate = nv->pstate;
    t.source = "nvml";
    any = true;
  }

  // Query PCIe link info independently: telemetry calls can fail while link queries still work.
  if (const auto link = readNvmlPcieLinkForGpu(index)) {
    t.pcieLinkWidth = link->width;
    t.pcieLinkGen = link->generation;
    any = true;
  }

  if (any) return t;

  // AMD/Intel via Linux sysfs (best-effort).
  if (const auto lt = readLinuxGpuTelemetry(index)) {
    t.utilPct = lt->utilPct;
    t.vramUsedGiB = lt->vramUsedGiB;
    t.vramTotalGiB = lt->vramTotalGiB;
    t.watts = lt->watts;
    t.tempC = lt->tempC;
    t.pstate = lt->pstate;
    t.source = lt->source;
    return t;
  }

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

  // Arrow keys are typically encoded as escape sequences. The default ESCDELAY can
  // make them feel sluggish and can also contribute to perceived input backlog.
  // Keep it small so navigation is snappy.
#if defined(NCURSES_VERSION)
  set_escdelay(1);
#endif
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

  TuiState state;
  state.screen = Screen::Timelines;

  // Draw something immediately so we don't appear to hang on a cleared screen.
  {
    int rows = 0, cols = 0;
    getmaxyx(stdscr, rows, cols);

    Frame frame;
    Viewport vp{cols, rows};
    state.tick = 0;
    state.hardwareLines = {"Initializing..."};
    renderFrame(frame, vp, state, cfg, debugMode);

    int lastAttr = INT32_MIN;
    for (int y = 0; y < rows; ++y) {
      for (int x = 0; x < cols; ++x) {
        const auto& c = frame.at(x, y);
        const int attr = styleToAttr(c.style);
        if (attr != lastAttr) {
          attrset(attr);
          lastAttr = attr;
        }
        mvaddch(y, x, cellToChtype(c.ch));
      }
    }
    attrset(A_NORMAL);
    refresh();
  }

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
  bool hasNvml = false;
  if (!debugMode) {
    int rows = 0, cols = 0;
    getmaxyx(stdscr, rows, cols);
    (void)rows;
    erase();
    drawHeader(cols, "AI-Z - Starting");
    mvhline(2, 0, ' ', cols);
    mvaddnstr(2, 0, "Initializing NVML...", cols);
    refresh();

    const auto n = nvmlGpuCount();
    if (n && *n > 0) {
      gpuCount = *n;
      hasNvml = true;
    } else {
      const unsigned int nSys = linuxGpuCount();
      if (nSys > 0) gpuCount = nSys;
    }
  }

  // Resolve device names early (before starting background threads) so Timelines titles
  // can show real CPU/GPU names without requiring the Hardware screen.
  const std::string cpuNameFast = probeCpuNameFast();
  std::vector<std::string> gpuNamesInit = probeGpuNamesFast(gpuCount, hasNvml);
  if (!hasRealDeviceNames(gpuNamesInit)) {
    std::vector<std::string> cudaNames = probeGpuNamesCudaFast(gpuCount);
    if (hasRealDeviceNames(cudaNames)) gpuNamesInit = std::move(cudaNames);
  }

  if (!cpuNameFast.empty()) state.cpuDevice = cpuNameFast;
  if (hasRealDeviceNames(gpuNamesInit)) state.gpuDeviceNames = std::move(gpuNamesInit);

  std::mutex nvmlMu;
  std::vector<std::optional<GpuTelemetry>> cachedGpuTel;
  cachedGpuTel.resize(gpuCount);
  std::optional<NvmlPcieThroughput> cachedPcie;
  std::atomic<bool> nvmlStop{false};
  std::thread nvmlThread;
  if (!debugMode) {
    nvmlThread = std::thread([&]() {
      while (!nvmlStop.load()) {
        std::vector<std::optional<GpuTelemetry>> nextGpu;
        nextGpu.resize(gpuCount);

        for (unsigned int i = 0; i < gpuCount; ++i) {
          if (hasNvml) {
            nextGpu[static_cast<std::size_t>(i)] = readGpuTelemetryPreferNvml(i);
          } else {
            // Avoid NVML wrapper overhead on non-NVIDIA systems.
            if (const auto lt = readLinuxGpuTelemetry(i)) {
              GpuTelemetry t;
              t.utilPct = lt->utilPct;
              t.vramUsedGiB = lt->vramUsedGiB;
              t.vramTotalGiB = lt->vramTotalGiB;
              t.watts = lt->watts;
              t.tempC = lt->tempC;
              t.pstate = lt->pstate;
              t.source = lt->source;
              nextGpu[static_cast<std::size_t>(i)] = t;
            }
          }
        }

        const auto nextPcie = hasNvml ? readNvmlPcieThroughput() : std::nullopt;

        {
          std::lock_guard<std::mutex> lk(nvmlMu);
          cachedGpuTel = std::move(nextGpu);
          cachedPcie = nextPcie;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
    });
  }

  // Use shared-core timelines so the Frame renderer can display them.
  ensureTimelineCapacity(state.cpuTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.gpuMemUtilTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.ramTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.vramTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.diskTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.diskReadTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.diskWriteTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.netRxTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.netTxTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.pcieRxTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.pcieTxTl, cfg.timelineSamples);

  // Defer heavy probing until the user opens Hardware/Benchmarks.
  HardwareInfo hwCache;
  std::vector<std::string> hwLines = {"(Press W / F2 to probe hardware)", "(Press B / F3 to load benchmarks)"};
  bool hwReady = false;

  // Boot-time hardware probe (background): this is primarily to get CPU/GPU names
  // for Timelines titles reliably, without requiring the user to open the Hardware screen.
  std::mutex bootHwMu;
  std::atomic<bool> bootHwReady{false};
  HardwareInfo bootHwCache;
  std::vector<std::string> bootHwLines;
  std::thread bootHwThread;
  if (!debugMode) {
    bootHwThread = std::thread([&]() {
      HardwareInfo hw = HardwareInfo::probe();
      std::vector<std::string> lines = hw.toLines();
      {
        std::lock_guard<std::mutex> lk(bootHwMu);
        bootHwCache = std::move(hw);
        bootHwLines = std::move(lines);
      }
      bootHwReady.store(true);
    });
  }

  // Benchmark runner state: keep UI responsive while benchmarks execute.
  std::thread benchThread;

  auto rebuildBenchRows = [&]() {
    state.benches.clear();
    state.benchRowTitles.clear();
    state.benchRowIsHeader.clear();
    state.benchResults.clear();

    const std::vector<std::string> gpuNames = parseGpuNames(hwCache, gpuCount);

    auto addHeader = [&](const std::string& title) {
      state.benches.push_back(nullptr);
      state.benchRowTitles.push_back(title);
      state.benchRowIsHeader.push_back(true);
      state.benchResults.emplace_back();
    };

    auto addBench = [&](std::unique_ptr<IBenchmark> b) {
      const std::string title = b ? b->name() : std::string("(null)");
      state.benches.push_back(std::move(b));
      state.benchRowTitles.push_back(title);
      state.benchRowIsHeader.push_back(false);
      state.benchResults.emplace_back();
    };

    for (unsigned int gi = 0; gi < gpuCount; ++gi) {
      addHeader("GPU" + std::to_string(gi) + " - " + gpuNames[static_cast<std::size_t>(gi)]);
      addBench(makeGpuCudaPcieBandwidthBenchmark(gi));
      addBench(makeGpuVulkanPcieBandwidthBenchmark(gi));
      addBench(makeGpuOpenclPcieBandwidthBenchmark(gi));
      addBench(makeGpuFp32BenchmarkVulkan(gi));
      addBench(makeGpuFp32BenchmarkOpencl(gi));
      addBench(makeGpuFp16Benchmark(gi));
      addBench(makeGpuFp32Benchmark(gi));
      addBench(makeGpuFp64Benchmark(gi));
      addBench(makeGpuInt4Benchmark(gi));
      addBench(makeGpuInt8Benchmark(gi));

      // Inference benchmarks: currently device-0 only.
      if (gi == 0) {
        addBench(makeOrtCudaMatMulBenchmark());
        addBench(makeOrtCudaMemoryBandwidthBenchmark());
      }
    }

    // If no GPUs are detected, still group GPU inference benches under a GPU header.
    if (gpuCount == 0) {
      addHeader("GPU0 - (no GPU detected)");
      addBench(makeOrtCudaMatMulBenchmark());
      addBench(makeOrtCudaMemoryBandwidthBenchmark());
    }

    addHeader("CPU0 - " + (hwCache.cpuName.empty() ? std::string("unknown") : hwCache.cpuName));
    addBench(makeCpuFp16FlopsBenchmark());
    addBench(makeCpuFp32FlopsBenchmark());
    addBench(makeOrtCpuMatMulBenchmark());
    addBench(makeOrtCpuMemoryBandwidthBenchmark());
  };

  bool benchReady = false;

  auto ensureHardwareAndBenches = [&]() {
    {
      std::lock_guard<std::mutex> lk(state.benchMutex);
      if (state.benchmarksRunning) return;
    }
    if (hwReady && benchReady) return;
    int rows = 0, cols = 0;
    getmaxyx(stdscr, rows, cols);
    (void)rows;
    erase();
    drawHeader(cols, "AI-Z - Initializing");
    mvhline(2, 0, ' ', cols);
    mvaddnstr(2, 0, "Probing hardware...", cols);
    refresh();

    hwCache = HardwareInfo::probe();
    hwLines = hwCache.toLines();
    state.hardwareLines = hwLines;
    state.hardwareDirty = false;
    hwReady = true;
    rebuildBenchRows();
    benchReady = true;
  };

  state.benchmarksSel = 0;
  state.configSel = 0;

  std::uint64_t uiTick = 0;

  int lastRows = -1;
  int lastCols = -1;
  Screen lastScreen = state.screen;

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
    ++uiTick;
    // Join completed benchmark worker (if any) so we can start another run.
    if (benchThread.joinable()) {
      bool runningNow = false;
      {
        std::lock_guard<std::mutex> lk(state.benchMutex);
        runningNow = state.benchmarksRunning;
      }
      if (!runningNow) benchThread.join();
    }

    // Input first: if a key is pressed, apply it and render the updated state
    // immediately, instead of waiting for the next refresh tick.
    {
      bool runningNow = false;
      {
        std::lock_guard<std::mutex> lk(state.benchMutex);
        runningNow = state.benchmarksRunning;
      }
      const std::uint32_t waitMs = runningNow ? std::min<std::uint32_t>(cfg.refreshMs, 100u) : cfg.refreshMs;
      timeout(static_cast<int>(waitMs));
      const int ch = getch();

      // If the UI is rendering/sampling slowly, key-repeat can build up in the input
      // buffer. Drain any queued input immediately so navigation doesn't "coast"
      // for seconds after releasing a key.
      std::vector<int> queuedKeys;
      if (ch != ERR) {
        queuedKeys.push_back(ch);
        timeout(0);
        for (;;) {
          const int next = getch();
          if (next == ERR) break;
          queuedKeys.push_back(next);
        }
      }

      for (const int k : queuedKeys) {
        // Sampling / scroll speed stays backend-local for now.
        if (k == '+' || k == '=') {
          adjustRefreshMs(true);
          continue;
        }
        if (k == '-' || k == '_') {
          adjustRefreshMs(false);
          continue;
        }

        const auto cmdOpt = keyToCommand(k, state.screen);
        if (!cmdOpt) continue;
        const Command cmd = *cmdOpt;

        if (cmd == Command::Quit) {
          running = false;
          continue;
        }

        // Navigation + selection changes are centralized.
        if (cmd == Command::NavHelp || cmd == Command::NavHardware || cmd == Command::NavBenchmarks || cmd == Command::NavConfig ||
            cmd == Command::NavTimelines || cmd == Command::Back || cmd == Command::Up || cmd == Command::Down) {
          applyCommand(state, cfg, cmd);
          if (state.screen == Screen::Hardware || state.screen == Screen::Benchmarks) {
            ensureHardwareAndBenches();
          }
          continue;
        }

        // Config screen actions are centralized in the core.
        if (state.screen == Screen::Config &&
            (cmd == Command::Toggle || cmd == Command::Defaults || cmd == Command::Save || cmd == Command::Activate)) {
          applyCommand(state, cfg, cmd);
          continue;
        }

        // Screen-local actions.
        if (cmd == Command::Refresh && state.screen == Screen::Hardware) {
          ensureHardwareAndBenches();
          continue;
        }

        if (cmd == Command::Activate && state.screen == Screen::Benchmarks) {
          // Run benchmarks on a worker thread so the UI can keep updating.
          bool runningNow = false;
          {
            std::lock_guard<std::mutex> lk(state.benchMutex);
            runningNow = state.benchmarksRunning;
          }
          if (benchThread.joinable() && runningNow) {
            // Ignore activation while a run is in progress.
            continue;
          }
          if (benchThread.joinable() && !runningNow) benchThread.join();

          if (state.benchmarksSel == 0) {
            {
              std::lock_guard<std::mutex> lk(state.benchMutex);
              state.lastBenchResult = "Running all...";
              state.benchmarksRunning = true;
              state.runningBenchIndex = -1;
            }
            benchThread = std::thread([&]() {
              for (int row = 0; row < static_cast<int>(state.benches.size()); ++row) {
                if (row >= 0 && row < static_cast<int>(state.benchRowIsHeader.size()) &&
                    state.benchRowIsHeader[static_cast<std::size_t>(row)]) {
                  continue;
                }
                {
                  std::lock_guard<std::mutex> lk(state.benchMutex);
                  state.runningBenchIndex = row;
                }

                auto& b = state.benches[static_cast<std::size_t>(row)];
                const BenchResult r = b ? b->run() : BenchResult{false, "(null benchmark)"};

                {
                  std::lock_guard<std::mutex> lk(state.benchMutex);
                  if (static_cast<std::size_t>(row) < state.benchResults.size()) {
                    state.benchResults[static_cast<std::size_t>(row)] = r.ok ? r.summary : ("FAIL: " + r.summary);
                  }
                }
              }
              {
                std::lock_guard<std::mutex> lk(state.benchMutex);
                state.runningBenchIndex = -1;
                state.benchmarksRunning = false;
              }
            });
          } else {
            const int row = state.benchmarksSel - 1;
            if (row >= 0 && row < static_cast<int>(state.benches.size()) &&
                row < static_cast<int>(state.benchRowIsHeader.size()) &&
                !state.benchRowIsHeader[static_cast<std::size_t>(row)]) {
              {
                std::lock_guard<std::mutex> lk(state.benchMutex);
                state.lastBenchResult = "Running...";
                state.benchmarksRunning = true;
                state.runningBenchIndex = row;
              }
              benchThread = std::thread([&, row]() {
                auto& b = state.benches[static_cast<std::size_t>(row)];
                const BenchResult r = b ? b->run() : BenchResult{false, "(null benchmark)"};

                {
                  std::lock_guard<std::mutex> lk(state.benchMutex);
                  if (static_cast<std::size_t>(row) < state.benchResults.size()) {
                    state.benchResults[static_cast<std::size_t>(row)] = r.ok ? r.summary : ("FAIL: " + r.summary);
                  }
                  state.runningBenchIndex = -1;
                  state.benchmarksRunning = false;
                }
              });
            } else {
              std::lock_guard<std::mutex> lk(state.benchMutex);
              state.lastBenchResult = "(not runnable)";
            }
          }
          continue;
        }
      }
    }

    int rows = 0, cols = 0;
    getmaxyx(stdscr, rows, cols);

    // Ensure we can render a full-width scrolling graph without trailing empty columns.
    const std::size_t desiredSamples =
      std::max<std::size_t>(cfg.timelineSamples, static_cast<std::size_t>(std::max(0, cols)));
    ensureTimelineCapacity(state.cpuTl, desiredSamples);
    ensureTimelineCapacity(state.gpuMemUtilTl, desiredSamples);
    ensureTimelineCapacity(state.ramTl, desiredSamples);
    ensureTimelineCapacity(state.vramTl, desiredSamples);
    ensureTimelineCapacity(state.diskTl, desiredSamples);
    ensureTimelineCapacity(state.diskReadTl, desiredSamples);
    ensureTimelineCapacity(state.diskWriteTl, desiredSamples);
    ensureTimelineCapacity(state.netRxTl, desiredSamples);
    ensureTimelineCapacity(state.netTxTl, desiredSamples);
    ensureTimelineCapacity(state.pcieRxTl, desiredSamples);
    ensureTimelineCapacity(state.pcieTxTl, desiredSamples);

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

      // Copy cached NVML/sysfs telemetry without doing any expensive work on the UI thread.
      {
        std::lock_guard<std::mutex> lk(nvmlMu);
        gpuTel = cachedGpuTel;
        if (cachedPcie) {
          pcieRx = Sample{cachedPcie->rxMBps, "MB/s", "nvml"};
          pcieTx = Sample{cachedPcie->txMBps, "MB/s", "nvml"};
        }
      }

      // Aggregate GPU memory-controller utilization (when available).
      double maxMemUtil = -1.0;
      std::string memLabel;
      for (const auto& gt : gpuTel) {
        if (!gt || !gt->memUtilPct) continue;
        if (*gt->memUtilPct > maxMemUtil) {
          maxMemUtil = *gt->memUtilPct;
          memLabel = gt->source;
        }
      }
      if (maxMemUtil >= 0.0) {
        if (memLabel.empty()) memLabel = "gpu";
        gpuMemUtil = Sample{maxMemUtil, "%", memLabel};
      }

      // Prefer per-GPU telemetry when available (NVML or sysfs).
      for (unsigned int i = 0; i < gpuCount; ++i) {
        const auto& gt = gpuTel[static_cast<std::size_t>(i)];
        if (!gt || !gt->utilPct) continue;
        const std::string label = !gt->source.empty() ? gt->source : std::string("gpu");
        gpuSamples[static_cast<std::size_t>(i)] = Sample{*gt->utilPct, "%", label};
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
        std::string label;
        for (const auto& gt : gpuTel) {
          if (!gt || !gt->vramUsedGiB || !gt->vramTotalGiB) continue;
          if (label.empty() && !gt->source.empty()) label = gt->source;
          else if (!gt->source.empty() && gt->source != label) {
            label = "gpu";
            break;
          }
        }
        if (label.empty()) label = "gpu";
        vramPct = Sample{(100.0 * sumUsed / sumTotal), "%", label};
      }
    }

    // Update shared-core snapshot + timelines.
    state.tick = uiTick;
    state.latest.cpu = cpu;
    state.latest.disk = disk;
    state.latest.diskRead = diskRead;
    state.latest.diskWrite = diskWrite;
    state.latest.netRx = netRx;
    state.latest.netTx = netTx;
    // GPU0 as a Sample is still used by some panels.
    if (!gpuSamples.empty()) state.latest.gpu = gpuSamples[0];
    state.latest.gpuMemUtil = gpuMemUtil;
    state.latest.pcieRx = pcieRx;
    state.latest.pcieTx = pcieTx;
    state.latest.ramPct = ramPct;
    state.latest.vramPct = vramPct;
    state.latest.ramText = formatRamText(ram);

    // Populate per-GPU details for the multi-line header.
    state.latest.gpus.clear();
    state.latest.gpus.reserve(cachedGpuTel.size());
    for (std::size_t i = 0; i < cachedGpuTel.size(); ++i) {
      GpuTelemetrySnapshot gs;
      if (i < gpuSamples.size() && gpuSamples[i]) {
        gs.utilPct = gpuSamples[i]->value;
      }
      if (cachedGpuTel[i]) {
        const auto& gt = *cachedGpuTel[i];
        if (gt.utilPct) gs.utilPct = *gt.utilPct;
        gs.memUtilPct = gt.memUtilPct;
        gs.vramUsedGiB = gt.vramUsedGiB;
        gs.vramTotalGiB = gt.vramTotalGiB;
        gs.watts = gt.watts;
        gs.tempC = gt.tempC;
        gs.pstate = gt.pstate;
        if (gt.pcieLinkWidth) gs.pcieLinkWidth = static_cast<int>(*gt.pcieLinkWidth);
        if (gt.pcieLinkGen) gs.pcieLinkGen = static_cast<int>(*gt.pcieLinkGen);
      }
      state.latest.gpus.push_back(std::move(gs));
    }

    state.cpuTl.push(cpu ? cpu->value : 0.0);
    state.gpuMemUtilTl.push((gpuMemUtil && cfg.showGpuMem) ? gpuMemUtil->value : 0.0);
    state.ramTl.push(ramPct ? ramPct->value : 0.0);
    state.vramTl.push(vramPct ? vramPct->value : 0.0);
    state.diskReadTl.push(diskRead ? diskRead->value : 0.0);
    state.diskWriteTl.push(diskWrite ? diskWrite->value : 0.0);
    state.netRxTl.push(netRx ? netRx->value : 0.0);
    state.netTxTl.push(netTx ? netTx->value : 0.0);
    state.pcieRxTl.push(pcieRx ? pcieRx->value : 0.0);
    state.pcieTxTl.push(pcieTx ? pcieTx->value : 0.0);

    // Keep hardware lines available for the core renderer.
    state.hardwareLines = hwLines;

    // If the background boot probe finished, apply it once.
    // This makes device names available for Timelines section titles.
    static bool bootApplied = false;
    if (!bootApplied && bootHwReady.load()) {
      {
        std::lock_guard<std::mutex> lk(bootHwMu);
        hwCache = std::move(bootHwCache);
        hwLines = std::move(bootHwLines);
      }

      state.hardwareLines = hwLines;
      state.hardwareDirty = false;
      hwReady = true;

      // Initialize benchmark rows too so we don't re-probe later.
      rebuildBenchRows();
      benchReady = true;

      // Best-effort device names for timeline section titles.
      if (!hwCache.cpuName.empty() && hwCache.cpuName != "--" && hwCache.cpuName != "unknown") {
        state.cpuDevice = hwCache.cpuName;
      }
      if (!hasRealDeviceNames(state.gpuDeviceNames)) {
        std::vector<std::string> fromHw = parseGpuNames(hwCache, gpuCount);
        if (hasRealDeviceNames(fromHw)) state.gpuDeviceNames = std::move(fromHw);
      }

      bootApplied = true;
    }

    // Best-effort device names for timeline section titles.
    // Only overwrite when we have *better* information.
    if (!hwCache.cpuName.empty() && hwCache.cpuName != "--" && hwCache.cpuName != "unknown") {
      state.cpuDevice = hwCache.cpuName;
    }

    if (!hasRealDeviceNames(state.gpuDeviceNames)) {
      std::vector<std::string> fromHw = parseGpuNames(hwCache, gpuCount);
      if (hasRealDeviceNames(fromHw)) {
        state.gpuDeviceNames = std::move(fromHw);
      }
    }

    // Per-GPU utilization history.
    if (state.gpuTls.size() != gpuCount) {
      state.gpuTls.clear();
      state.gpuTls.reserve(gpuCount);
      for (unsigned int i = 0; i < gpuCount; ++i) state.gpuTls.emplace_back(desiredSamples);
    }
    for (unsigned int i = 0; i < gpuCount; ++i) {
      ensureTimelineCapacity(state.gpuTls[static_cast<std::size_t>(i)], desiredSamples);
      const auto& s = (i < gpuSamples.size()) ? gpuSamples[static_cast<std::size_t>(i)] : std::optional<Sample>{};
      state.gpuTls[static_cast<std::size_t>(i)].push((s && cfg.showGpu) ? s->value : 0.0);
    }

    // Render via shared core and blit to ncurses.
    const bool fullRedraw = (rows != lastRows || cols != lastCols || state.screen != lastScreen);
    if (fullRedraw) {
      erase();
      lastRows = rows;
      lastCols = cols;
      lastScreen = state.screen;
    }

    Frame frame;
    Viewport vp{cols, rows};
    renderFrame(frame, vp, state, cfg, debugMode);

    int lastAttr = INT32_MIN;
    for (int y = 0; y < rows; ++y) {
      for (int x = 0; x < cols; ++x) {
        const auto& c = frame.at(x, y);
        const int attr = styleToAttr(c.style);
        if (attr != lastAttr) {
          attrset(attr);
          lastAttr = attr;
        }
        mvaddch(y, x, cellToChtype(c.ch));
      }
    }
    attrset(A_NORMAL);
    refresh();
  }

  nvmlStop.store(true);
  if (nvmlThread.joinable()) nvmlThread.join();
  if (bootHwThread.joinable()) bootHwThread.join();
  if (benchThread.joinable()) {
    // Benchmarks are best-effort; allow them to finish before tearing down ncurses.
    benchThread.join();
  }
  endwin();
  return 0;
}

}  // namespace aiz
