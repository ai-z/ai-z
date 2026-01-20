#include <aiz/tui/ncurses_ui.h>

#include <aiz/tui/tui_core.h>

#include <aiz/bench/bench.h>
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

#include "ncurses_render.h"
#include "ncurses_probe.h"
#include "ncurses_telemetry.h"
#include "ncurses_bench.h"
#include "ncurses_sampler.h"
#include "ncurses_bootprobe.h"
#include "ncurses_bench_rows.h"

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

static void ensureTimelineCapacity(Timeline& tl, std::size_t desiredCapacity) {
  if (tl.capacity() >= desiredCapacity) return;
  Timeline resized(desiredCapacity);
  for (double v : tl.values()) {
    resized.push(v);
  }
  tl = std::move(resized);
}

// RAM/GPU telemetry formatting helpers live in ncurses_telemetry.{h,cpp}.

// Device-name probing helpers live in ncurses_probe.{h,cpp}.

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

  // Frame diffing: keep a previous frame and only update changed cells.
  Frame prevFrame;
  bool havePrevFrame = false;

  int lastRows = -1;
  int lastCols = -1;
  Screen lastScreen = state.screen;

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
        const int attr = ncurses::styleToAttr(c.style);
        if (attr != lastAttr) {
          attrset(attr);
          lastAttr = attr;
        }
        mvaddch(y, x, ncurses::cellToChtype(c.ch));
      }
    }
    attrset(A_NORMAL);
    refresh();

    // Seed the diff cache and last-known geometry so the first loop iteration
    // doesn't immediately erase + redraw the entire screen.
    prevFrame = std::move(frame);
    havePrevFrame = true;
    lastRows = rows;
    lastCols = cols;
    lastScreen = state.screen;
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
    ncurses::drawHeader(cols, "AI-Z - Starting");
    mvhline(2, 0, ' ', cols);
    mvaddnstr(2, 0, "Initializing NVML...", cols);
    refresh();
    // We drew directly via ncurses, so the terminal no longer matches prevFrame.
    havePrevFrame = false;

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
  const std::string cpuNameFast = ncurses::probeCpuNameFast();
  std::vector<std::string> gpuNamesInit = ncurses::probeGpuNamesFast(gpuCount, hasNvml);
  if (!ncurses::hasRealDeviceNames(gpuNamesInit)) {
    std::vector<std::string> cudaNames = ncurses::probeGpuNamesCudaFast(gpuCount);
    if (ncurses::hasRealDeviceNames(cudaNames)) gpuNamesInit = std::move(cudaNames);
  }

  if (!cpuNameFast.empty()) state.cpuDevice = cpuNameFast;
  if (ncurses::hasRealDeviceNames(gpuNamesInit)) state.gpuDeviceNames = std::move(gpuNamesInit);

  ncurses::GpuTelemetrySampler gpuSampler(gpuCount, hasNvml);
  if (!debugMode) gpuSampler.start();

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
  ncurses::BootHardwareProbe bootProbe;
  if (!debugMode) bootProbe.start();

  // Benchmark runner state: keep UI responsive while benchmarks execute.
  std::thread benchThread;

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
    ncurses::drawHeader(cols, "AI-Z - Initializing");
    mvhline(2, 0, ' ', cols);
    mvaddnstr(2, 0, "Probing hardware...", cols);
    refresh();
    // We drew directly via ncurses, so the terminal no longer matches prevFrame.
    havePrevFrame = false;

    hwCache = HardwareInfo::probe();
    hwLines = hwCache.toLines();
    state.hardwareLines = hwLines;
    state.hardwareDirty = false;
    hwReady = true;
    ncurses::rebuildBenchRows(state, hwCache, gpuCount);
    benchReady = true;
  };

  state.benchmarksSel = 0;
  state.configSel = 0;

  std::uint64_t uiTick = 0;

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
    ncurses::benchJoinIfDone(benchThread, state);

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
          ncurses::benchHandleActivate(benchThread, state);
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
    std::vector<std::optional<ncurses::GpuTelemetry>> gpuTel;
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

      ncurses::GpuTelemetry gt;
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
      std::optional<NvmlPcieThroughput> cachedPcie;
      gpuSampler.snapshot(gpuTel, cachedPcie);
      if (cachedPcie) {
        pcieRx = Sample{cachedPcie->rxMBps, "MB/s", "nvml"};
        pcieTx = Sample{cachedPcie->txMBps, "MB/s", "nvml"};
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
    state.latest.ramText = ncurses::formatRamText(ram);

    // Populate per-GPU details for the multi-line header.
    state.latest.gpus.clear();
    state.latest.gpus.reserve(gpuTel.size());
    for (std::size_t i = 0; i < gpuTel.size(); ++i) {
      GpuTelemetrySnapshot gs;
      if (i < gpuSamples.size() && gpuSamples[i]) {
        gs.utilPct = gpuSamples[i]->value;
      }
      if (gpuTel[i]) {
        const auto& gt = *gpuTel[i];
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
    if (!bootApplied && bootProbe.tryConsume(hwCache, hwLines)) {

      state.hardwareLines = hwLines;
      state.hardwareDirty = false;
      hwReady = true;

      // Initialize benchmark rows too so we don't re-probe later.
      ncurses::rebuildBenchRows(state, hwCache, gpuCount);
      benchReady = true;

      // Best-effort device names for timeline section titles.
      if (!hwCache.cpuName.empty() && hwCache.cpuName != "--" && hwCache.cpuName != "unknown") {
        state.cpuDevice = hwCache.cpuName;
      }
      if (!ncurses::hasRealDeviceNames(state.gpuDeviceNames)) {
        std::vector<std::string> fromHw = ncurses::parseGpuNames(hwCache, gpuCount);
        if (ncurses::hasRealDeviceNames(fromHw)) state.gpuDeviceNames = std::move(fromHw);
      }

      bootApplied = true;
    }

    // Best-effort device names for timeline section titles.
    // Only overwrite when we have *better* information.
    if (!hwCache.cpuName.empty() && hwCache.cpuName != "--" && hwCache.cpuName != "unknown") {
      state.cpuDevice = hwCache.cpuName;
    }

    if (!ncurses::hasRealDeviceNames(state.gpuDeviceNames)) {
      std::vector<std::string> fromHw = ncurses::parseGpuNames(hwCache, gpuCount);
      if (ncurses::hasRealDeviceNames(fromHw)) {
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
      havePrevFrame = false;
    }

    Frame frame;
    Viewport vp{cols, rows};
    renderFrame(frame, vp, state, cfg, debugMode);

    const bool canDiff = havePrevFrame && prevFrame.width == frame.width && prevFrame.height == frame.height;

    int lastAttr = INT32_MIN;
    if (!canDiff) {
      for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
          const auto& c = frame.at(x, y);
          const int attr = ncurses::styleToAttr(c.style);
          if (attr != lastAttr) {
            attrset(attr);
            lastAttr = attr;
          }
          mvaddch(y, x, ncurses::cellToChtype(c.ch));
        }
      }
    } else {
      const std::size_t n = frame.cells.size();
      for (std::size_t i = 0; i < n; ++i) {
        const auto& cur = frame.cells[i];
        const auto& prev = prevFrame.cells[i];
        if (cur.ch == prev.ch && cur.style == prev.style) continue;

        const int y = static_cast<int>(i / static_cast<std::size_t>(frame.width));
        const int x = static_cast<int>(i % static_cast<std::size_t>(frame.width));

        const int attr = ncurses::styleToAttr(cur.style);
        if (attr != lastAttr) {
          attrset(attr);
          lastAttr = attr;
        }
        mvaddch(y, x, ncurses::cellToChtype(cur.ch));
      }
    }

    prevFrame = std::move(frame);
    havePrevFrame = true;
    attrset(A_NORMAL);
    refresh();
  }

  gpuSampler.stop();
  bootProbe.stop();
  // Benchmarks are best-effort; allow them to finish before tearing down ncurses.
  ncurses::benchShutdown(benchThread);
  endwin();
  return 0;
}

}  // namespace aiz
