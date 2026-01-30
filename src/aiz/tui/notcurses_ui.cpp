#include <aiz/tui/notcurses_ui.h>

#include <aiz/tui/tui_core.h>

#include <aiz/bench/bench.h>
#include <aiz/hw/hardware_info.h>
#include <aiz/metrics/cpu_usage.h>
#include <aiz/metrics/disk_bandwidth.h>
#include <aiz/metrics/network_bandwidth.h>
#if defined(AI_Z_PLATFORM_LINUX)
#include <aiz/metrics/linux_gpu_sysfs.h>
#endif
#include <aiz/metrics/nvidia_nvml.h>
#include <aiz/metrics/process_list.h>
#include <aiz/metrics/ram_usage.h>
#include <aiz/metrics/timeline.h>
#include <aiz/version.h>

#include <notcurses/notcurses.h>

#include <clocale>
#include <cctype>
#include <iostream>

#if defined(AI_Z_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "notcurses_render.h"
#include "notcurses_input.h"

// Reuse the existing helpers from the ncurses implementation.
// They don't actually depend on ncurses - they're just telemetry/probe utilities.
#include "ncurses_probe.h"
#include "ncurses_telemetry.h"
#include "ncurses_bench.h"
#include "ncurses_sampler.h"
#include "ncurses_bootprobe.h"
#include "ncurses_bench_rows.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <random>

namespace aiz {

namespace {

static void ensureTimelineCapacity(Timeline& tl, std::size_t desiredCapacity) {
  if (tl.capacity() >= desiredCapacity) return;
  Timeline resized(desiredCapacity);
  for (double v : tl.values()) {
    resized.push(v);
  }
  tl = std::move(resized);
}

static std::string trimAscii(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

static std::string afterColonSpace(const std::string& s) {
  const std::size_t pos = s.find(": ");
  if (pos == std::string::npos) return s;
  return s.substr(pos + 2);
}

static int parseFirstInt(const std::string& s) {
  int cur = 0;
  bool inNum = false;
  for (char ch : s) {
    if (std::isdigit(static_cast<unsigned char>(ch))) {
      inNum = true;
      cur = cur * 10 + (ch - '0');
    } else if (inNum) {
      break;
    }
  }
  return inNum ? cur : 0;
}

static std::string formatTimelineRamDevice(const HardwareInfo& hw) {
  const std::string& s = hw.ramSummary;
  if (s.empty() || s == "--" || s == "unknown") return {};

  std::string type;
  {
    const char* kTypes[] = {"DDR5", "DDR4", "DDR3", "DDR2", "LPDDR5", "LPDDR4", "LPDDR3"};
    for (const char* t : kTypes) {
      if (s.find(t) != std::string::npos) {
        type = t;
        break;
      }
    }
  }

  int speed = 0;
  {
    const std::size_t sp = s.find("speed:");
    if (sp != std::string::npos) {
      speed = parseFirstInt(s.substr(sp));
    }
  }

  if (!type.empty() && speed > 0) return type + " " + std::to_string(speed) + " MT/s";
  if (speed > 0) return std::to_string(speed) + " MT/s";
  if (!type.empty()) return type;
  return {};
}

static std::string formatTimelineDiskDevice(const HardwareInfo& hw) {
  if (hw.perDiskLines.empty()) return {};
  std::string first = afterColonSpace(hw.perDiskLines.front());
  first = trimAscii(first);
  if (hw.perDiskLines.size() > 1) {
    first += " +" + std::to_string(hw.perDiskLines.size() - 1);
  }
  return first;
}

static std::string formatTimelineNetDevice(const HardwareInfo& hw) {
  if (hw.perNicLines.empty()) return {};
  std::string first = afterColonSpace(hw.perNicLines.front());
  first = trimAscii(first);
  const std::size_t open = first.find(" (");
  const std::size_t close = first.rfind(")");
  if (open != std::string::npos && close != std::string::npos && close > open) {
    std::string inParens = first.substr(open + 2, close - (open + 2));
    first.erase(open);
    first = trimAscii(first);
    inParens = trimAscii(inParens);
    if (!inParens.empty()) first += " " + inParens;
  }
  if (hw.perNicLines.size() > 1) {
    first += " +" + std::to_string(hw.perNicLines.size() - 1);
  }
  return first;
}

static void applyTimelineDevicesFromHw(TuiState& state, const HardwareInfo& hw) {
  const std::string ramDev = formatTimelineRamDevice(hw);
  const std::string diskDev = formatTimelineDiskDevice(hw);
  const std::string netDev = formatTimelineNetDevice(hw);

  if (!ramDev.empty() && state.ramDevice.empty()) state.ramDevice = ramDev;
  if (!diskDev.empty() && state.diskDevice.empty()) state.diskDevice = diskDev;
  if (!netDev.empty() && state.netDevice.empty()) state.netDevice = netDev;
}

static bool windowsEnvFlagSet(const char* name) {
  if (!name) return false;
  if (const char* v = std::getenv(name)) {
    if (*v == '\0') return false;
    if (v[0] == '0' && v[1] == '\0') return false;
    return true;
  }
  return false;
}

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

int NotcursesUi::run(Config& cfg, bool debugMode) {
  // Required for correct CJK width handling and wide-character output.
  std::setlocale(LC_ALL, "");

  // Initialize notcurses.
  struct notcurses_options opts{};
  opts.flags = NCOPTION_SUPPRESS_BANNERS;

  struct notcurses* nc = notcurses_init(&opts, nullptr);
  if (!nc) {
    std::cerr << "ai-z: failed to initialize notcurses (unsupported terminal).\n";
    return 1;
  }

  struct ncplane* stdplane = notcurses_stdplane(nc);

  TuiState state;
  state.screen = Screen::Timelines;

  // Frame diffing: keep a previous frame and only update changed cells.
  Frame prevFrame;
  bool havePrevFrame = false;

  unsigned int lastRows = 0;
  unsigned int lastCols = 0;
  Screen lastScreen = state.screen;

  // Get initial dimensions.
  ncplane_dim_yx(stdplane, &lastRows, &lastCols);

  // Draw initial frame.
  {
    Frame frame;
    Viewport vp{static_cast<int>(lastCols), static_cast<int>(lastRows)};
    state.tick = 0;
    state.hardwareLines = {"Initializing..."};
    renderFrame(frame, vp, state, cfg, debugMode);
    notcurses_impl::blitFrame(stdplane, frame, nullptr);
    notcurses_render(nc);
    prevFrame = std::move(frame);
    havePrevFrame = true;
  }

  CpuUsageCollector cpuCol;
  CpuMaxCoreUsageCollector cpuMaxCol;
  DiskBandwidthCollector diskReadCol(DiskBandwidthMode::Read);
  DiskBandwidthCollector diskWriteCol(DiskBandwidthMode::Write);
  NetworkBandwidthCollector netRxCol(NetworkBandwidthMode::Rx);
  NetworkBandwidthCollector netTxCol(NetworkBandwidthMode::Tx);
  ProcessSampler procSampler;

  // Debug generators.
  RandomWalk dbgCpu(0.0, 100.0, 10.0);
  RandomWalk dbgCpuMax(0.0, 100.0, 12.0);
  RandomWalk dbgGpu(0.0, 100.0, 12.0);
  RandomWalk dbgDisk(0.0, 3000.0, 250.0);
  RandomWalk dbgDiskR(0.0, 3000.0, 250.0);
  RandomWalk dbgDiskW(0.0, 3000.0, 250.0);
  RandomWalk dbgNetRx(0.0, 5000.0, 350.0);
  RandomWalk dbgNetTx(0.0, 5000.0, 350.0);
  RandomWalk dbgPcieRx(0.0, 32000.0, 2500.0);
  RandomWalk dbgPcieTx(0.0, 32000.0, 2500.0);
  RandomWalk dbgRamPct(0.0, 100.0, 6.0);
  RandomWalk dbgVramPct(0.0, 100.0, 8.0);
  RandomWalk dbgGpuW(5.0, 350.0, 20.0);
  RandomWalk dbgGpuTemp(25.0, 92.0, 6.0);

  unsigned int gpuCount = 1;
  bool hasNvml = false;
  if (!debugMode) {
    notcurses_impl::drawHeader(stdplane, static_cast<int>(lastCols), "AI-Z - Starting");
    ncplane_putstr_yx(stdplane, 2, 0, "Initializing NVML...");
    notcurses_render(nc);
    havePrevFrame = false;

    const auto n = nvmlGpuCount();
    if (n && *n > 0) {
      gpuCount = *n;
      hasNvml = true;
    } else {
#if defined(AI_Z_PLATFORM_LINUX)
      const unsigned int nSys = linuxGpuCount();
      if (nSys > 0) gpuCount = nSys;
#elif defined(_WIN32)
      const unsigned int nSys = ncurses::probeGpuCountFast();
      if (nSys > 0) gpuCount = nSys;
#endif
    }
  }

  // Resolve device names early.
  const std::string cpuNameFast = ncurses::probeCpuNameFast();
  std::vector<std::string> gpuNamesInit = ncurses::probeGpuNamesFast(gpuCount, hasNvml);
  if (!ncurses::hasRealDeviceNames(gpuNamesInit)) {
    std::vector<std::string> cudaNames = ncurses::probeGpuNamesCudaFast(gpuCount);
    if (ncurses::hasRealDeviceNames(cudaNames)) gpuNamesInit = std::move(cudaNames);
  }

  if (!cpuNameFast.empty()) state.cpuDevice = cpuNameFast;
  if (ncurses::hasRealDeviceNames(gpuNamesInit)) state.gpuDeviceNames = std::move(gpuNamesInit);

  ncurses::GpuTelemetrySampler gpuSampler(gpuCount, hasNvml);
  const bool disableGpuSampler = windowsEnvFlagSet("AI_Z_DISABLE_GPU_SAMPLER");
  if (!debugMode && !disableGpuSampler) {
    gpuSampler.start();
  }

#if defined(_WIN32)
  if (!debugMode && !hasNvml && !ncurses::GpuTelemetrySampler::isPcieThroughputSupported()) {
    cfg.showPcieRx = false;
    cfg.showPcieTx = false;
  }
#endif

  // Ensure timeline capacity.
  ensureTimelineCapacity(state.cpuTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.cpuMaxTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.gpuMemUtilTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.gpuClockTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.gpuMemClockTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.gpuEncTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.gpuDecTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.ramTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.vramTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.diskTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.diskReadTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.diskWriteTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.netRxTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.netTxTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.pcieRxTl, cfg.timelineSamples);
  ensureTimelineCapacity(state.pcieTxTl, cfg.timelineSamples);

  // Hardware probe cache.
  HardwareInfo hwCache;
  std::vector<std::string> hwLines = {"(Press W / F2 to probe hardware)", "(Press B / F3 to load benchmarks)"};
  bool hwReady = false;

  // Boot-time hardware probe.
  ncurses::BootHardwareProbe bootProbe;
  const bool disableBootProbe = windowsEnvFlagSet("AI_Z_DISABLE_BOOT_PROBE");
#if defined(_WIN32)
  const bool enableBootProbeThread = windowsEnvFlagSet("AI_Z_ENABLE_BOOT_PROBE");
#else
  const bool enableBootProbeThread = true;
#endif

  if (!debugMode && !disableBootProbe && enableBootProbeThread) {
    bootProbe.start();
  }

  // Benchmark runner state.
  std::thread benchThread;
  bool benchReady = false;

  auto ensureHardwareAndBenches = [&]() {
    {
      std::lock_guard<std::mutex> lk(state.benchMutex);
      if (state.benchmarksRunning) return;
    }
    if (hwReady && benchReady) return;

    unsigned int rows = 0, cols = 0;
    ncplane_dim_yx(stdplane, &rows, &cols);
    ncplane_erase(stdplane);
    notcurses_impl::drawHeader(stdplane, static_cast<int>(cols), "AI-Z - Initializing");
    ncplane_putstr_yx(stdplane, 2, 0, "Probing hardware...");
    notcurses_render(nc);
    havePrevFrame = false;

    hwCache = HardwareInfo::probe();
    hwLines = hwCache.toLines();
    state.hardwareLines = hwLines;
    state.hardwareDirty = false;
    applyTimelineDevicesFromHw(state, hwCache);
    hwReady = true;
    ncurses::rebuildBenchRows(state, hwCache, gpuCount);
    benchReady = true;
  };

  state.benchmarksSel = 0;
  state.configSel = 0;

  std::uint64_t uiTick = 0;
  bool running = true;

  std::optional<std::uint32_t> smokeMs;
  if (const char* v = std::getenv("AI_Z_TUI_SMOKE_MS")) {
    const long ms = std::strtol(v, nullptr, 10);
    if (ms > 0) smokeMs = static_cast<std::uint32_t>(ms);
  }
  const auto smokeStart = std::chrono::steady_clock::now();

  constexpr std::uint32_t kRefreshMinMs = 200;
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

  // Frame timing to prevent busy-looping.
  auto lastFrameTime = std::chrono::steady_clock::now();

  while (running) {
    bool sawResize = false;

    if (smokeMs) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - smokeStart);
      if (elapsed.count() >= static_cast<long long>(*smokeMs)) {
        break;
      }
    }

    // Enforce minimum frame time to prevent CPU spinning.
    {
      const auto now = std::chrono::steady_clock::now();
      const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameTime).count();
      const long targetMs = static_cast<long>(cfg.refreshMs);
      if (elapsedMs < targetMs) {
        const long remainingMs = targetMs - elapsedMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(remainingMs));
      }
      lastFrameTime = std::chrono::steady_clock::now();
    }

    ++uiTick;
    ncurses::benchJoinIfDone(benchThread, state);

    // Input handling.
    {
      bool runningNow = false;
      {
        std::lock_guard<std::mutex> lk(state.benchMutex);
        runningNow = state.benchmarksRunning;
      }
      // Use a short timeout since frame pacing is handled by the sleep above.
      // This allows responsive input while not blocking for the full refresh interval.
      constexpr std::uint32_t kInputPollMs = 10;

      // Read first input with timeout.
      auto ev = notcurses_impl::readInput(nc, kInputPollMs, state.screen);

      // Collect this and any queued events.
      std::vector<notcurses_impl::InputEvent> events;
      if (ev.keyId != 0) {
        events.push_back(ev);
      }
      auto drained = notcurses_impl::drainInput(nc, state.screen);
      events.insert(events.end(), drained.begin(), drained.end());

      for (const auto& evt : events) {
        if (evt.isResize) {
          sawResize = true;
          continue;
        }

        if (!evt.cmd) continue;
        const Command cmd = *evt.cmd;

        // Speed controls.
        if (evt.keyId == '+' || evt.keyId == '=') {
          adjustRefreshMs(true);
          continue;
        }
        if (evt.keyId == '-' || evt.keyId == '_') {
          adjustRefreshMs(false);
          continue;
        }

        if (cmd == Command::Quit) {
          running = false;
          continue;
        }

        // Navigation and selection.
        if (cmd == Command::NavHelp || cmd == Command::NavHardware || cmd == Command::NavBenchmarks ||
            cmd == Command::NavConfig || cmd == Command::NavMinimal || cmd == Command::NavProcesses ||
            cmd == Command::Back || cmd == Command::Up || cmd == Command::Down ||
            cmd == Command::Left || cmd == Command::Right) {
          applyCommand(state, cfg, cmd);
          if (state.screen == Screen::Hardware || state.screen == Screen::Benchmarks) {
            ensureHardwareAndBenches();
          }
          continue;
        }

        if ((state.screen == Screen::Timelines || state.screen == Screen::Minimal) &&
            (cmd == Command::ViewTimelines || cmd == Command::ViewBars || cmd == Command::ViewMinimal)) {
          applyCommand(state, cfg, cmd);
          continue;
        }

        if (state.screen == Screen::Config &&
            (cmd == Command::Toggle || cmd == Command::Defaults || cmd == Command::Save ||
             cmd == Command::Activate || cmd == Command::Left || cmd == Command::Right)) {
          applyCommand(state, cfg, cmd);
          continue;
        }

        if (cmd == Command::Refresh && state.screen == Screen::Hardware) {
          ensureHardwareAndBenches();
          continue;
        }

        if (cmd == Command::Activate && state.screen == Screen::Benchmarks) {
          ncurses::benchHandleActivate(benchThread, state);
          continue;
        }

        if (state.screen == Screen::Benchmarks && cmd == Command::BenchRunAll) {
          state.benchmarksSel = 0;
          ncurses::benchHandleActivate(benchThread, state);
          continue;
        }

        if (state.screen == Screen::Benchmarks && cmd == Command::BenchReport) {
          ensureHardwareAndBenches();
          (void)ncurses::benchGenerateHtmlReport(benchThread, state);
          continue;
        }

        if (state.screen == Screen::Processes &&
            (cmd == Command::SortProcessName || cmd == Command::SortCpu || cmd == Command::SortGpu ||
             cmd == Command::SortRam || cmd == Command::SortVram || cmd == Command::ToggleGpuOnly)) {
          applyCommand(state, cfg, cmd);
          continue;
        }
      }
    }

    // Handle resize.
    unsigned int rows = 0, cols = 0;
    ncplane_dim_yx(stdplane, &rows, &cols);

    bool forceFullRedraw = sawResize || (rows != lastRows || cols != lastCols || state.screen != lastScreen);
    if (forceFullRedraw) {
      ncplane_erase(stdplane);
      lastRows = rows;
      lastCols = cols;
      lastScreen = state.screen;
      havePrevFrame = false;
    }

    // Ensure timeline capacity for current width.
    const std::size_t desiredSamples =
        std::max<std::size_t>(cfg.timelineSamples, static_cast<std::size_t>(std::max(0u, cols)));
    ensureTimelineCapacity(state.cpuTl, desiredSamples);
    ensureTimelineCapacity(state.cpuMaxTl, desiredSamples);
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

    // Sample telemetry.
    std::optional<Sample> cpu, cpuMax, disk, diskRead, diskWrite, netRx, netTx;
    std::optional<RamUsage> ram;
    std::optional<Sample> ramPct, vramPct, gpuMemUtil, pcieRx, pcieTx;
    std::vector<std::optional<Sample>> gpuSamples(gpuCount);
    std::vector<std::optional<ncurses::GpuTelemetry>> gpuTel(gpuCount);

    if (debugMode) {
      const double cpuVal = dbgCpu.next();
      cpu = Sample{cpuVal, "%", "debug"};
      cpuMax = Sample{std::max(cpuVal, dbgCpuMax.next()), "%", "debug"};
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

      const double util = dbgGpu.next();
      gpuSamples[0] = Sample{util, "%", "debug"};
      gpuMemUtil = Sample{util, "%", "debug"};

      ncurses::GpuTelemetry gt;
      gt.utilPct = util;
      const double totalVram = 10.0;
      const double vramPctV = dbgVramPct.next();
      const double usedVram = (vramPctV / 100.0) * totalVram;
      gt.vramUsedGiB = usedVram;
      gt.vramTotalGiB = totalVram;
      vramPct = Sample{vramPctV, "%", "debug"};
      gt.watts = dbgGpuW.next();
      gt.tempC = dbgGpuTemp.next();
      gt.gpuClockMHz = static_cast<unsigned int>(1200 + util * 10.0);
      gt.memClockMHz = static_cast<unsigned int>(7000 + util * 5.0);
      gt.encoderUtilPct = util * 0.6;
      gt.decoderUtilPct = util * 0.4;
      std::string ps = "P8";
      if (util > 80.0) ps = "P0";
      else if (util > 50.0) ps = "P2";
      else if (util > 20.0) ps = "P5";
      gt.pstate = ps;

      if (const auto link = readNvmlPcieLinkForGpu(0)) {
        gt.pcieLinkWidth = link->width;
        gt.pcieLinkGen = link->generation;
      }
      gpuTel[0] = gt;
    } else {
      cpu = cpuCol.sample();
      cpuMax = cpuMaxCol.sample();
      diskRead = diskReadCol.sample();
      diskWrite = diskWriteCol.sample();
      netRx = netRxCol.sample();
      netTx = netTxCol.sample();
      ram = readRamUsage();

      std::optional<NvmlPcieThroughput> cachedPcie;
      gpuSampler.snapshot(gpuTel, cachedPcie);
      if (cachedPcie) {
        pcieRx = Sample{cachedPcie->rxMBps, "MB/s", "nvml"};
        pcieTx = Sample{cachedPcie->txMBps, "MB/s", "nvml"};
      }

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

      for (unsigned int i = 0; i < gpuCount; ++i) {
        const auto& gt = gpuTel[i];
        if (!gt || !gt->utilPct) continue;
        const std::string label = !gt->source.empty() ? gt->source : std::string("gpu");
        gpuSamples[i] = Sample{*gt->utilPct, "%", label};
      }

      if (ram) {
        ramPct = Sample{ram->usedPct, "%", "os"};
      }

      double sumUsed = 0.0, sumTotal = 0.0;
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

    std::optional<Sample> gpuClock, gpuMemClock, gpuEnc, gpuDec;
    if (!gpuTel.empty() && gpuTel[0]) {
      const auto& gt0 = *gpuTel[0];
      const std::string label = !gt0.source.empty() ? gt0.source : std::string("gpu");
      if (gt0.gpuClockMHz) gpuClock = Sample{static_cast<double>(*gt0.gpuClockMHz), "MHz", label};
      if (gt0.memClockMHz) gpuMemClock = Sample{static_cast<double>(*gt0.memClockMHz), "MHz", label};
      if (gt0.encoderUtilPct) gpuEnc = Sample{*gt0.encoderUtilPct, "%", label};
      if (gt0.decoderUtilPct) gpuDec = Sample{*gt0.decoderUtilPct, "%", label};
    }

    // Update state.
    state.tick = uiTick;
    state.latest.cpu = cpu;
    state.latest.cpuMax = cpuMax;
    state.latest.disk = disk;
    state.latest.diskRead = diskRead;
    state.latest.diskWrite = diskWrite;
    state.latest.netRx = netRx;
    state.latest.netTx = netTx;
    if (!gpuSamples.empty()) state.latest.gpu = gpuSamples[0];
    state.latest.gpuMemUtil = gpuMemUtil;
    state.latest.gpuClock = gpuClock;
    state.latest.gpuMemClock = gpuMemClock;
    state.latest.gpuEnc = gpuEnc;
    state.latest.gpuDec = gpuDec;
    state.latest.pcieRx = pcieRx;
    state.latest.pcieTx = pcieTx;
    state.latest.ramPct = ramPct;
    state.latest.vramPct = vramPct;
    state.latest.ramText = ncurses::formatRamText(ram);

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
        gs.gpuClockMHz = gt.gpuClockMHz;
        gs.memClockMHz = gt.memClockMHz;
        gs.encoderUtilPct = gt.encoderUtilPct;
        gs.decoderUtilPct = gt.decoderUtilPct;
        if (gt.pcieLinkWidth) gs.pcieLinkWidth = static_cast<int>(*gt.pcieLinkWidth);
        if (gt.pcieLinkGen) gs.pcieLinkGen = static_cast<int>(*gt.pcieLinkGen);
        gs.pcieLinkNote = gt.pcieLinkNote;
      }
      state.latest.gpus.push_back(std::move(gs));
    }

    // Update timelines.
    state.cpuTl.push(cpu ? cpu->value : 0.0);
    state.cpuMaxTl.push(cpuMax ? cpuMax->value : 0.0);
    state.gpuMemUtilTl.push((gpuMemUtil && (cfg.showGpuMem || cfg.showGpuMemBars)) ? gpuMemUtil->value : 0.0);
    state.gpuClockTl.push((gpuClock && (cfg.showGpuClock || cfg.showGpuClockBars)) ? gpuClock->value : 0.0);
    state.gpuMemClockTl.push((gpuMemClock && (cfg.showGpuMemClock || cfg.showGpuMemClockBars)) ? gpuMemClock->value : 0.0);
    state.gpuEncTl.push((gpuEnc && (cfg.showGpuEnc || cfg.showGpuEncBars)) ? gpuEnc->value : 0.0);
    state.gpuDecTl.push((gpuDec && (cfg.showGpuDec || cfg.showGpuDecBars)) ? gpuDec->value : 0.0);
    state.ramTl.push(ramPct ? ramPct->value : 0.0);
    state.vramTl.push(vramPct ? vramPct->value : 0.0);
    state.diskReadTl.push(diskRead ? diskRead->value : 0.0);
    state.diskWriteTl.push(diskWrite ? diskWrite->value : 0.0);
    state.netRxTl.push(netRx ? netRx->value : 0.0);
    state.netTxTl.push(netTx ? netTx->value : 0.0);
    state.pcieRxTl.push(pcieRx ? pcieRx->value : 0.0);
    state.pcieTxTl.push(pcieTx ? pcieTx->value : 0.0);

    state.hardwareLines = hwLines;

    // Process list (if on that screen).
    if (state.screen == Screen::Processes) {
      const int availRows = std::max(0, static_cast<int>(rows) - 4);
      const std::size_t maxCount = (availRows > 0) ? static_cast<std::size_t>(availRows) : 0u;
      const std::size_t cpuCount = (maxCount > 0) ? (maxCount * 2) : 0u;

      std::vector<CpuProcessInfo> cpuTop = procSampler.sampleTop(cpuCount);
      std::vector<NvmlProcessInfo> gpuTop = readNvmlProcessInfo();

      std::unordered_map<int, TuiState::ProcessEntry> merged;
      merged.reserve(cpuTop.size() + gpuTop.size());

      for (const auto& p : cpuTop) {
        TuiState::ProcessEntry entry;
        entry.pid = p.pid;
        entry.name = p.name;
        entry.cmdline = p.cmdline;
        entry.cpuPct = p.cpuPct;
        entry.ramBytes = p.ramBytes;
        merged.emplace(p.pid, std::move(entry));
      }

      for (const auto& gp : gpuTop) {
        const int pid = static_cast<int>(gp.pid);
        if (!isUserProcess(pid)) continue;
        auto it = merged.find(pid);
        if (it == merged.end()) {
          TuiState::ProcessEntry entry;
          entry.pid = pid;
          if (const auto id = readProcessIdentity(pid)) {
            entry.name = id->name;
            entry.cmdline = id->cmdline;
            entry.ramBytes = id->ramBytes;
          }
          entry.gpuIndex = static_cast<int>(gp.gpuIndex);
          if (gp.vramUsedGiB > 0.0) entry.vramUsedGiB = gp.vramUsedGiB;
          if (gp.gpuUtilPct) entry.gpuUtilPct = gp.gpuUtilPct;
          merged.emplace(pid, std::move(entry));
        } else {
          it->second.gpuIndex = static_cast<int>(gp.gpuIndex);
          if (gp.vramUsedGiB > 0.0) it->second.vramUsedGiB = gp.vramUsedGiB;
          if (gp.gpuUtilPct) it->second.gpuUtilPct = gp.gpuUtilPct;
        }
      }

      std::vector<TuiState::ProcessEntry> procRows;
      procRows.reserve(merged.size());
      for (auto& kv : merged) {
        if (state.processesGpuOnly) {
          const auto& e = kv.second;
          const bool onGpu = e.gpuIndex.has_value() || e.gpuUtilPct.has_value() || e.vramUsedGiB.has_value();
          if (!onGpu) continue;
        }
        procRows.push_back(std::move(kv.second));
      }

      auto toLower = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char ch : s) out.push_back(static_cast<char>(std::tolower(ch)));
        return out;
      };

      std::sort(procRows.begin(), procRows.end(), [&](const TuiState::ProcessEntry& a, const TuiState::ProcessEntry& b) {
        switch (state.processSort) {
          case TuiState::ProcessSort::Name:
            return toLower(a.name) < toLower(b.name);
          case TuiState::ProcessSort::Cpu:
            return a.cpuPct > b.cpuPct;
          case TuiState::ProcessSort::Gpu:
            return a.gpuUtilPct.value_or(0.0) > b.gpuUtilPct.value_or(0.0);
          case TuiState::ProcessSort::Ram:
            return a.ramBytes > b.ramBytes;
          case TuiState::ProcessSort::Vram:
            return a.vramUsedGiB.value_or(0.0) > b.vramUsedGiB.value_or(0.0);
          default:
            return a.pid < b.pid;
        }
      });

      if (maxCount > 0 && procRows.size() > maxCount) procRows.resize(maxCount);
      state.processes = std::move(procRows);
    }

    // Boot probe.
    static bool bootApplied = false;
    if (!bootApplied && bootProbe.tryConsume(hwCache, hwLines)) {
      state.hardwareLines = hwLines;
      state.hardwareDirty = false;
      hwReady = true;
      ncurses::rebuildBenchRows(state, hwCache, gpuCount);
      benchReady = true;

      if (!hwCache.cpuName.empty() && hwCache.cpuName != "--" && hwCache.cpuName != "unknown") {
        state.cpuDevice = hwCache.cpuName;
      }
      if (!ncurses::hasRealDeviceNames(state.gpuDeviceNames)) {
        std::vector<std::string> fromHw = ncurses::parseGpuNames(hwCache, gpuCount);
        if (ncurses::hasRealDeviceNames(fromHw)) state.gpuDeviceNames = std::move(fromHw);
      }
      applyTimelineDevicesFromHw(state, hwCache);
      bootApplied = true;
    }

    // Per-GPU timelines.
    if (state.gpuTls.size() != gpuCount) {
      state.gpuTls.clear();
      state.gpuTls.reserve(gpuCount);
      for (unsigned int i = 0; i < gpuCount; ++i) state.gpuTls.emplace_back(desiredSamples);
    }
    for (unsigned int i = 0; i < gpuCount; ++i) {
      ensureTimelineCapacity(state.gpuTls[i], desiredSamples);
      const auto& s = (i < gpuSamples.size()) ? gpuSamples[i] : std::optional<Sample>{};
      state.gpuTls[i].push((s && (cfg.showGpu || cfg.showGpuBars)) ? s->value : 0.0);
    }

    // Render.
    Frame frame;
    Viewport vp{static_cast<int>(cols), static_cast<int>(rows)};
    renderFrame(frame, vp, state, cfg, debugMode);

    const Frame* prevPtr = havePrevFrame ? &prevFrame : nullptr;
    notcurses_impl::blitFrame(stdplane, frame, prevPtr);
    notcurses_render(nc);

    prevFrame = std::move(frame);
    havePrevFrame = true;
  }

  gpuSampler.stop();
  bootProbe.stop();
  ncurses::benchShutdown(benchThread);
  notcurses_stop(nc);
  return 0;
}

}  // namespace aiz
