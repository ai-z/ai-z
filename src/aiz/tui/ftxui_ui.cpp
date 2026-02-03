#include <aiz/tui/ftxui_ui.h>

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

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/canvas.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(AI_Z_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif
#endif

// Reuse the existing helpers from the ncurses implementation.
#include "ncurses_probe.h"
#include "ncurses_telemetry.h"
#include "ncurses_bench.h"
#include "ncurses_sampler.h"
#include "ncurses_bootprobe.h"
#include "ncurses_bench_rows.h"

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

#if defined(AI_Z_PLATFORM_WINDOWS)
static bool enableWindowsVirtualTerminal() {
  // Set console code page to UTF-8 so Unicode block characters (█▄▀ etc.)
  // display correctly even without the Windows "Beta: Use Unicode UTF-8"
  // option enabled.
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut == INVALID_HANDLE_VALUE) return false;
  DWORD mode = 0;
  if (!GetConsoleMode(hOut, &mode)) return false;
  mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
  if (!SetConsoleMode(hOut, mode)) return false;

  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  if (hIn == INVALID_HANDLE_VALUE) return false;
  if (!GetConsoleMode(hIn, &mode)) return false;
  mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
  SetConsoleMode(hIn, mode);
  return true;
}
#endif

// Convert Frame/Cell-based rendering to FTXUI Element
struct FtxuiStyle {
  std::optional<ftxui::Color> fg;
  std::optional<ftxui::Color> bg;
  bool bold = false;
};

static FtxuiStyle styleToFtxuiStyle(Style style) {
  using namespace ftxui;

  // Light-blue footer label background (requested).
  const Color footerBg = Color(173, 216, 230);  // "light blue"
  const Color footerBgActive = Color(100, 170, 255);

  switch (style) {
    case Style::Header:
      return {.fg = Color::Cyan};
    case Style::FooterKey:
      return {.fg = Color::Yellow, .bold = true};
    case Style::FooterBlock:
      return {.fg = Color::Black, .bg = footerBg};
    case Style::FooterHot:
      return {.fg = Color::Red, .bg = footerBg, .bold = true};
    case Style::FooterActive:
      return {.fg = Color::White, .bg = footerBgActive, .bold = true};
    case Style::Hot:
      return {.fg = Color::Yellow, .bold = true};
    case Style::Section:
      return {.fg = Color::Cyan};
    case Style::Value:
      return {.fg = Color::Green};
    case Style::Warning:
      return {.fg = Color::Red, .bold = true};
    case Style::Default:
    default:
      return {};
  }
}

// Convert Frame to FTXUI Elements
static ftxui::Element frameToElement(const Frame& frame) {
  using namespace ftxui;
  
  std::vector<Element> lines;
  lines.reserve(static_cast<std::size_t>(frame.height));
  
  for (int y = 0; y < frame.height; ++y) {
    std::vector<Element> lineElements;
    
    int x = 0;
    while (x < frame.width) {
      const Cell& cell = frame.at(x, y);
      
      // Skip wide-char continuation markers
      if (cell.ch == kWideContinuation) {
        ++x;
        continue;
      }
      
      // Collect consecutive cells with the same style
      Style currentStyle = static_cast<Style>(cell.style);
      std::wstring run;
      run.push_back(cell.ch);
      ++x;
      
      while (x < frame.width) {
        const Cell& next = frame.at(x, y);
        if (next.ch == kWideContinuation) {
          ++x;
          continue;
        }
        if (static_cast<Style>(next.style) != currentStyle) {
          break;
        }
        run.push_back(next.ch);
        ++x;
      }
      
      // Convert wstring to string (UTF-8)
      std::string utf8;
      for (wchar_t wc : run) {
        if (wc < 0x80) {
          utf8.push_back(static_cast<char>(wc));
        } else if (wc < 0x800) {
          utf8.push_back(static_cast<char>(0xC0 | (wc >> 6)));
          utf8.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else if (wc < 0x10000) {
          utf8.push_back(static_cast<char>(0xE0 | (wc >> 12)));
          utf8.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
          utf8.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else {
          utf8.push_back(static_cast<char>(0xF0 | (wc >> 18)));
          utf8.push_back(static_cast<char>(0x80 | ((wc >> 12) & 0x3F)));
          utf8.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
          utf8.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        }
      }
      
      Element elem = text(utf8);
      const FtxuiStyle style = styleToFtxuiStyle(currentStyle);
      if (style.fg) elem = elem | color(*style.fg);
      if (style.bg) elem = elem | bgcolor(*style.bg);
      if (style.bold) elem = elem | bold;
      lineElements.push_back(elem);
    }
    
    lines.push_back(hbox(std::move(lineElements)));
  }
  
  return vbox(std::move(lines));
}

// Map FTXUI events to our Command enum
static Command eventToCommand(const ftxui::Event& event, Screen currentScreen) {
  using namespace ftxui;
  
  // Quit (only q/Q, not ESC - ESC is for back navigation)
  if (event == Event::Character('q') || event == Event::Character('Q')) {
    return Command::Quit;
  }
  
  // ESC goes back (or quits if on main screen)
  if (event == Event::Escape) {
    if (currentScreen == Screen::Timelines) {
      return Command::Quit;
    }
    return Command::Back;
  }
  
  // Navigation keys
  if (event == Event::ArrowUp || event == Event::Character('k')) {
    return Command::Up;
  }
  if (event == Event::ArrowDown || event == Event::Character('j')) {
    return Command::Down;
  }
  if (event == Event::ArrowLeft || event == Event::Character('h')) {
    return Command::Left;
  }
  if (event == Event::ArrowRight || event == Event::Character('l')) {
    return Command::Right;
  }
  
  // Enter/Space for activation
  if (event == Event::Return) {
    return Command::Activate;
  }
  if (event == Event::Character(' ')) {
    return Command::Toggle;
  }
  
  // Function keys and shortcuts
  if (event == Event::F1 || event == Event::Character('?')) {
    return Command::NavHelp;
  }
  if (event == Event::F2 || event == Event::Character('w') || event == Event::Character('W')) {
    return Command::NavHardware;
  }
  if (event == Event::F3 || event == Event::Character('b') || event == Event::Character('B')) {
    return Command::NavBenchmarks;
  }
  if (event == Event::F4 || event == Event::Character('c') || event == Event::Character('C')) {
    return Command::NavConfig;
  }
  if (event == Event::F5) {
    return Command::NavProcesses;
  }
  if (event == Event::F10) {
    return Command::Quit;
  }
  if (event == Event::Character('p') || event == Event::Character('P')) {
    return Command::NavProcesses;
  }
  
  // Screen-specific number keys
  if (currentScreen == Screen::Benchmarks) {
    // Benchmark screen: 1=Run All, 2=Report
    if (event == Event::Character('1')) {
      return Command::BenchRunAll;
    }
    if (event == Event::Character('2')) {
      return Command::BenchReport;
    }
  } else if (currentScreen == Screen::Timelines || currentScreen == Screen::Minimal) {
    // View modes on timeline screens
    if (event == Event::Character('1')) {
      return Command::ViewTimelines;
    }
    if (event == Event::Character('2')) {
      return Command::ViewBars;
    }
    if (event == Event::Character('3')) {
      return Command::ViewMinimal;
    }
  }
  
  // Back to timelines
  if (event == Event::Backspace) {
    return Command::Back;
  }
  
  // Config screen actions
  if (currentScreen == Screen::Config) {
    if (event == Event::Character('s') || event == Event::Character('S')) {
      return Command::Save;
    }
    if (event == Event::Character('d') || event == Event::Character('D')) {
      return Command::Defaults;
    }
  }
  
  // Refresh (Hardware screen)
  if (currentScreen == Screen::Hardware) {
    if (event == Event::Character('r') || event == Event::Character('R')) {
      return Command::Refresh;
    }
  }
  
  // Process sort keys
  if (currentScreen == Screen::Processes) {
    if (event == Event::Character('n') || event == Event::Character('N')) {
      return Command::SortProcessName;
    }
    if (event == Event::Character('c') || event == Event::Character('C')) {
      return Command::SortCpu;
    }
    if (event == Event::Character('g') || event == Event::Character('G')) {
      return Command::SortGpu;
    }
    if (event == Event::Character('m') || event == Event::Character('M')) {
      return Command::SortRam;
    }
    if (event == Event::Character('v') || event == Event::Character('V')) {
      return Command::SortVram;
    }
    if (event == Event::Character('o') || event == Event::Character('O')) {
      return Command::ToggleGpuOnly;
    }
  }
  
  return Command::None;
}

}  // namespace

int FtxuiUi::run(Config& cfg, bool debugMode) {
  if (debugMode) {
    std::cerr << "ai-z: entering FtxuiUi::run()\n";
    std::cerr.flush();
  }

  std::setlocale(LC_ALL, "");

#if defined(AI_Z_PLATFORM_WINDOWS)
  enableWindowsVirtualTerminal();
#endif

  TuiState state;
  state.screen = Screen::Timelines;

  CpuUsageCollector cpuCol;
  CpuMaxCoreUsageCollector cpuMaxCol;
  DiskBandwidthCollector diskReadCol(DiskBandwidthMode::Read);
  DiskBandwidthCollector diskWriteCol(DiskBandwidthMode::Write);
  NetworkBandwidthCollector netRxCol(NetworkBandwidthMode::Rx);
  NetworkBandwidthCollector netTxCol(NetworkBandwidthMode::Tx);
  ProcessSampler procSampler;

  // Debug generators
  RandomWalk dbgCpu(0.0, 100.0, 10.0);
  RandomWalk dbgCpuMax(0.0, 100.0, 12.0);
  RandomWalk dbgGpu(0.0, 100.0, 12.0);
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

  // Device names
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

  // Ensure timeline capacity
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

  // Hardware probe cache
  HardwareInfo hwCache;
  std::vector<std::string> hwLines = {"(Press W / F2 to probe hardware)", "(Press B / F3 to load benchmarks)"};
  bool hwReady = false;

  // Boot-time hardware probe
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

  // Benchmark runner state
  std::thread benchThread;
  bool benchReady = false;

  auto ensureHardwareAndBenches = [&]() {
    {
      std::lock_guard<std::mutex> lk(state.benchMutex);
      if (state.benchmarksRunning) return;
    }
    if (hwReady && benchReady) return;

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

  // FTXUI screen
  auto screen = ftxui::ScreenInteractive::Fullscreen();
  
  // Renderer component
  auto renderer = ftxui::Renderer([&] {
    // Get terminal dimensions
    int cols = ftxui::Terminal::Size().dimx;
    int rows = ftxui::Terminal::Size().dimy;
    
    // Ensure timeline capacity for current width
    const std::size_t desiredSamples =
        std::max<std::size_t>(cfg.timelineSamples, static_cast<std::size_t>(std::max(0, cols)));
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

    // Per-GPU timelines
    if (state.gpuTls.size() != gpuCount) {
      state.gpuTls.clear();
      state.gpuTls.reserve(gpuCount);
      for (unsigned int i = 0; i < gpuCount; ++i) state.gpuTls.emplace_back(desiredSamples);
    }
    for (unsigned int i = 0; i < gpuCount; ++i) {
      ensureTimelineCapacity(state.gpuTls[i], desiredSamples);
    }
    
    // Render to Frame
    Frame frame;
    Viewport vp{cols, rows};
    renderFrame(frame, vp, state, cfg, debugMode);
    
    // Convert to FTXUI element and force it to fill the terminal
    using namespace ftxui;
    return frameToElement(frame) | size(WIDTH, EQUAL, cols) | size(HEIGHT, EQUAL, rows);
  });

  // Event handler component
  auto component = ftxui::CatchEvent(renderer, [&](ftxui::Event event) {
    // Handle resize
    if (event == ftxui::Event::Custom) {
      return false;
    }
    
    // Speed controls
    if (event == ftxui::Event::Character('+') || event == ftxui::Event::Character('=')) {
      adjustRefreshMs(true);
      return true;
    }
    if (event == ftxui::Event::Character('-') || event == ftxui::Event::Character('_')) {
      adjustRefreshMs(false);
      return true;
    }
    
    Command cmd = eventToCommand(event, state.screen);
    
    if (cmd == Command::None) {
      return false;
    }
    
    if (cmd == Command::Quit) {
      running = false;
      screen.Exit();
      return true;
    }
    
    // Navigation
    if (cmd == Command::NavHelp || cmd == Command::NavHardware || cmd == Command::NavBenchmarks ||
        cmd == Command::NavConfig || cmd == Command::NavMinimal || cmd == Command::NavProcesses ||
        cmd == Command::Back || cmd == Command::Up || cmd == Command::Down ||
        cmd == Command::Left || cmd == Command::Right) {
      applyCommand(state, cfg, cmd);
      if (state.screen == Screen::Hardware || state.screen == Screen::Benchmarks) {
        ensureHardwareAndBenches();
      }
      return true;
    }
    
    if ((state.screen == Screen::Timelines || state.screen == Screen::Minimal) &&
        (cmd == Command::ViewTimelines || cmd == Command::ViewBars || cmd == Command::ViewMinimal)) {
      applyCommand(state, cfg, cmd);
      return true;
    }
    
    if (state.screen == Screen::Config &&
        (cmd == Command::Toggle || cmd == Command::Defaults || cmd == Command::Save ||
         cmd == Command::Activate || cmd == Command::Left || cmd == Command::Right)) {
      applyCommand(state, cfg, cmd);
      return true;
    }
    
    if (cmd == Command::Refresh && state.screen == Screen::Hardware) {
      ensureHardwareAndBenches();
      return true;
    }
    
    if (cmd == Command::Activate && state.screen == Screen::Benchmarks) {
      ncurses::benchHandleActivate(benchThread, state);
      return true;
    }
    
    if (state.screen == Screen::Benchmarks && cmd == Command::BenchRunAll) {
      state.benchmarksSel = 0;
      ncurses::benchHandleActivate(benchThread, state);
      return true;
    }
    
    if (state.screen == Screen::Benchmarks && cmd == Command::BenchReport) {
      ensureHardwareAndBenches();
      (void)ncurses::benchGenerateHtmlReport(benchThread, state);
      return true;
    }
    
    if (state.screen == Screen::Processes &&
        (cmd == Command::SortProcessName || cmd == Command::SortCpu || cmd == Command::SortGpu ||
         cmd == Command::SortRam || cmd == Command::SortVram || cmd == Command::ToggleGpuOnly)) {
      applyCommand(state, cfg, cmd);
      return true;
    }
    
    return false;
  });

  // Background update loop
  std::atomic<bool> stopUpdate{false};
  std::thread updateThread([&]() {
    while (!stopUpdate && running) {
      auto frameStart = std::chrono::steady_clock::now();
      
      if (smokeMs) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - smokeStart);
        if (elapsed.count() >= static_cast<long long>(*smokeMs)) {
          running = false;
          screen.Exit();
          break;
        }
      }
      
      ++uiTick;
      ncurses::benchJoinIfDone(benchThread, state);
      
      // Sample telemetry
      std::optional<Sample> cpu, cpuMax, disk, diskRead, diskWrite, netRx, netTx;
      std::optional<RamUsage> ram;
      std::optional<Sample> ramPct, vramPct, gpuMemUtil, pcieRx, pcieTx;
      std::optional<Sample> gpuClock, gpuMemClock, gpuEnc, gpuDec;
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
        vramPct = Sample{dbgVramPct.next(), "%", "debug"};
        gpuMemUtil = Sample{util * 0.8, "%", "debug"};
        gpuClock = Sample{1500.0 + util * 5.0, "MHz", "debug"};
        gpuMemClock = Sample{8000.0 + util * 10.0, "MHz", "debug"};
        gpuEnc = Sample{util * 0.3, "%", "debug"};
        gpuDec = Sample{util * 0.2, "%", "debug"};

        ncurses::GpuTelemetry gt;
        gt.utilPct = util;
        gt.memUtilPct = util * 0.8;
        gt.vramUsedGiB = 4.0 + (dbgVramPct.next() / 100.0) * 20.0;
        gt.vramTotalGiB = 24.0;
        gt.watts = dbgGpuW.next();
        gt.tempC = dbgGpuTemp.next();
        gt.gpuClockMHz = static_cast<unsigned int>(1500 + util * 5);
        gt.memClockMHz = 8000u;
        gpuTel[0] = gt;
      } else {
        cpu = cpuCol.sample();
        cpuMax = cpuMaxCol.sample();
        diskRead = diskReadCol.sample();
        diskWrite = diskWriteCol.sample();
        netRx = netRxCol.sample();
        netTx = netTxCol.sample();
        ram = readRamUsage();
        if (ram) ramPct = Sample{ram->usedPct, "%", "os"};

        std::optional<NvmlPcieThroughput> pcie;
        gpuSampler.snapshot(gpuTel, pcie);

        for (unsigned int i = 0; i < gpuCount; ++i) {
          if (i < gpuTel.size() && gpuTel[i] && gpuTel[i]->utilPct) {
            gpuSamples[i] = Sample{*gpuTel[i]->utilPct, "%", gpuTel[i]->source};
          }
        }

        if (!gpuTel.empty() && gpuTel[0]) {
          const auto& g = *gpuTel[0];
          if (g.memUtilPct) gpuMemUtil = Sample{*g.memUtilPct, "%", g.source};
          if (g.vramUsedGiB && g.vramTotalGiB && *g.vramTotalGiB > 0.0) {
            vramPct = Sample{100.0 * (*g.vramUsedGiB / *g.vramTotalGiB), "%", g.source};
          }
          if (g.gpuClockMHz) gpuClock = Sample{static_cast<double>(*g.gpuClockMHz), "MHz", g.source};
          if (g.memClockMHz) gpuMemClock = Sample{static_cast<double>(*g.memClockMHz), "MHz", g.source};
          if (g.encoderUtilPct) gpuEnc = Sample{*g.encoderUtilPct, "%", g.source};
          if (g.decoderUtilPct) gpuDec = Sample{*g.decoderUtilPct, "%", g.source};
        }

        if (pcie) {
          pcieRx = Sample{pcie->rxMBps, "MB/s", "nvml"};
          pcieTx = Sample{pcie->txMBps, "MB/s", "nvml"};
        }
      }
      
      // Update state
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
      
      // Update timelines
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
      
      // Process list
      if (state.screen == Screen::Processes) {
        int rows = ftxui::Terminal::Size().dimy;
        const int availRows = std::max(0, rows - 4);
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
      
      // Boot probe
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
      
      // Per-GPU timelines
      for (unsigned int i = 0; i < gpuCount; ++i) {
        const auto& s = (i < gpuSamples.size()) ? gpuSamples[i] : std::optional<Sample>{};
        if (i < state.gpuTls.size()) {
          state.gpuTls[i].push((s && (cfg.showGpu || cfg.showGpuBars)) ? s->value : 0.0);
        }
      }
      
      // Request screen refresh
      screen.PostEvent(ftxui::Event::Custom);
      
      // Frame timing
      auto frameEnd = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frameEnd - frameStart);
      long targetMs = static_cast<long>(cfg.refreshMs);
      if (elapsed.count() < targetMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(targetMs - elapsed.count()));
      }
    }
  });

  screen.Loop(component);
  
  // Cleanup
  stopUpdate = true;
  if (updateThread.joinable()) {
    updateThread.join();
  }
  
  gpuSampler.stop();
  bootProbe.stop();
  ncurses::benchShutdown(benchThread);
  
  return 0;
}

// Factory function - this replaces makeUi()
std::unique_ptr<Ui> makeUi() {
  return std::make_unique<FtxuiUi>();
}

}  // namespace aiz
