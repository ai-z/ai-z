#include <aiz/tui/ncurses_ui.h>

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

#include <curses.h>

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

#include "ncurses_render.h"
#include "ncurses_probe.h"
#include "ncurses_telemetry.h"
#include "ncurses_bench.h"
#include "ncurses_sampler.h"
#include "ncurses_bootprobe.h"
#include "ncurses_bench_rows.h"
#include "ncurses_input.h"

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
  // Example: "31.9 GiB DDR4 (speed: 3200 MT/s, channels: likely >=2)"
  const std::string& s = hw.ramSummary;
  if (s.empty() || s == "--" || s == "unknown") return {};

  std::string type;
  {
    // Look for common DDR tokens.
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
  // Convert "Realtek ... (1000 Mb/s)" -> "Realtek ... 1000 Mb/s"
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

// RAM/GPU telemetry formatting helpers live in ncurses_telemetry.{h,cpp}.

// Device-name probing helpers live in ncurses_probe.{h,cpp}.

}  // namespace

namespace {

static bool windowsEnvFlagSet(const char* name) {
  if (!name) return false;
  if (const char* v = std::getenv(name)) {
    if (*v == '\0') return false;
    if (v[0] == '0' && v[1] == '\0') return false;
    return true;
  }
  return false;
}

}  // namespace

#if defined(AI_Z_PLATFORM_WINDOWS)
namespace {

static void windowsAppendTuiLog(const std::string& msg) {
  char tempPath[MAX_PATH] = {0};
  const DWORD n = GetTempPathA(MAX_PATH, tempPath);
  if (n == 0 || n >= MAX_PATH) return;

  std::string path(tempPath);
  if (!path.empty() && (path.back() != '\\' && path.back() != '/')) path.push_back('\\');
  path += "ai-z-tui.log";

  HANDLE h = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;

  DWORD written = 0;
  (void)WriteFile(h, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr);
  CloseHandle(h);
}

static bool windowsGetConsoleWindowSize(int* cols, int* rows) {
  if (!cols || !rows) return false;
  const HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut == INVALID_HANDLE_VALUE) return false;
  CONSOLE_SCREEN_BUFFER_INFO csbi{};
  if (!GetConsoleScreenBufferInfo(hOut, &csbi)) return false;
  *cols = (csbi.srWindow.Right - csbi.srWindow.Left) + 1;
  *rows = (csbi.srWindow.Bottom - csbi.srWindow.Top) + 1;
  return true;
}

static void windowsEnsureConsoleBufferAtLeast(int cols, int rows) {
  const HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut == INVALID_HANDLE_VALUE) return;

  CONSOLE_SCREEN_BUFFER_INFO csbi{};
  if (!GetConsoleScreenBufferInfo(hOut, &csbi)) return;

  const int curCols = static_cast<int>(csbi.dwSize.X);
  const int curRows = static_cast<int>(csbi.dwSize.Y);
  if (curCols >= cols && curRows >= rows) return;

  // Windows console limits are 16-bit signed for COORD.
  const int targetCols = std::clamp(cols, 1, 32767);
  const int targetRows = std::clamp(rows, 1, 32767);

  COORD newSize;
  newSize.X = static_cast<SHORT>(std::max(curCols, targetCols));
  newSize.Y = static_cast<SHORT>(std::max(curRows, targetRows));
  (void)SetConsoleScreenBufferSize(hOut, newSize);
}

static void windowsFixCursesEnvFromConsole() {
  // Some terminals/PTY layers end up with bogus LINES/COLUMNS values (even negative),
  // which can cause PDCurses to refuse to initialize.
  int cols = 0, rows = 0;
  if (!windowsGetConsoleWindowSize(&cols, &rows)) return;
  if (cols <= 0 || rows <= 0) return;

  char buf[32];
  (void)snprintf(buf, sizeof(buf), "%d", rows);
  SetEnvironmentVariableA("LINES", buf);
  (void)snprintf(buf, sizeof(buf), "%d", cols);
  SetEnvironmentVariableA("COLUMNS", buf);
}

extern "C" int aiz_windows_safe_refresh(void);

static bool windowsConsoleOkForCurses(std::string* reason) {
  const HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  const HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hIn == INVALID_HANDLE_VALUE || hOut == INVALID_HANDLE_VALUE) {
    if (reason) *reason = "invalid std handles";
    return false;
  }

  DWORD mode = 0;
  if (!GetConsoleMode(hOut, &mode)) {
    if (reason) *reason = "stdout is not a Windows console (output redirected or unsupported terminal)";
    return false;
  }
  if (!GetConsoleMode(hIn, &mode)) {
    if (reason) *reason = "stdin is not a Windows console (input redirected or unsupported terminal)";
    return false;
  }

  CONSOLE_SCREEN_BUFFER_INFO csbi{};
  if (!GetConsoleScreenBufferInfo(hOut, &csbi)) {
    if (reason) *reason = "failed to query console screen buffer";
    return false;
  }

  const int cols = (csbi.srWindow.Right - csbi.srWindow.Left) + 1;
  const int rows = (csbi.srWindow.Bottom - csbi.srWindow.Top) + 1;
  if (cols < 10 || rows < 2) {
    if (reason) *reason = "console window is too small";
    return false;
  }

  return true;
}

}  // namespace
#endif

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
  // Required for correct CJK width handling (wcwidth) and wide-character output.
  // Falls back gracefully if the environment isn't UTF-8.
  std::setlocale(LC_ALL, "");

#if defined(AI_Z_PLATFORM_WINDOWS)
  // PDCurses/ncurses builds on Windows can fail (or even crash) when stdout/stdin
  // are not connected to a real Windows console (e.g., redirected output, some PTYs).
  // Detect that early and provide a clear error instead of calling initscr().
  std::string consoleReason;
  if (!windowsConsoleOkForCurses(&consoleReason)) {
    const std::string msg1 = std::string("ai-z: TUI unavailable: ") + consoleReason + "\n";
    const std::string msg2 =
        "ai-z: try running from Windows Terminal/cmd.exe (not the VS Code integrated terminal), and avoid output redirection\n";
    windowsAppendTuiLog(msg1 + msg2);
    std::cerr << msg1 << msg2 << std::flush;
    return 1;
  }

  // If we're in a console but environment variables are bogus, override them.
  // This helps PDCurses initialize in some PTY/ConPTY setups.
  windowsAppendTuiLog("stage: console ok\n");
  windowsFixCursesEnvFromConsole();
  windowsAppendTuiLog("stage: env fixed (LINES/COLUMNS)\n");
#endif

  #if defined(AI_Z_PLATFORM_WINDOWS)
  windowsAppendTuiLog("stage: calling initscr()\n");
  #endif
  WINDOW* w = initscr();
  if (!w) {
    const std::string msg =
        "ai-z: failed to initialize curses (unsupported terminal).\n"
        "ai-z: try Windows Terminal/cmd.exe; the VS Code integrated terminal may not support curses on Windows.\n";
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog(msg);
    #endif
    std::cerr << msg << std::flush;
    return 1;
  }

  #if defined(AI_Z_PLATFORM_WINDOWS)
  windowsAppendTuiLog("stage: initscr ok\n");
  #endif
  cbreak();
  noecho();
  keypad(stdscr, TRUE);

  #if defined(AI_Z_PLATFORM_WINDOWS)
  windowsAppendTuiLog("stage: input modes set\n");
  #endif

  // Arrow keys are typically encoded as escape sequences. The default ESCDELAY can
  // make them feel sluggish and can also contribute to perceived input backlog.
  // Keep it small so navigation is snappy.
#if defined(NCURSES_VERSION)
  set_escdelay(1);
#elif defined(PDC_BUILD)
  // PDCurses handles escape timing internally.
#endif

  #if defined(AI_Z_PLATFORM_WINDOWS)
  windowsAppendTuiLog("stage: escdelay/cursor\n");
  #endif
  curs_set(0);

  #if defined(AI_Z_PLATFORM_WINDOWS)
  windowsAppendTuiLog("stage: colors begin\n");
  #endif
  if (has_colors()) {
    start_color();
    use_default_colors();
    // Pair 1: F-key blocks with background color.
    init_pair(1, COLOR_WHITE, COLOR_BLUE);
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
    // Pair 7: highlighted hot letter inside footer labels (yellow on blue).
    init_pair(7, COLOR_YELLOW, COLOR_BLUE);
    // Pair 8: active footer page (white on red).
    init_pair(8, COLOR_WHITE, COLOR_RED);
  }

  #if defined(AI_Z_PLATFORM_WINDOWS)
  windowsAppendTuiLog("stage: colors done\n");
  #endif

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
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog("stage: initial frame begin\n");
    #endif
    int rows = 0, cols = 0;
    getmaxyx(stdscr, rows, cols);

    Frame frame;
    Viewport vp{cols, rows};
    state.tick = 0;
    state.hardwareLines = {"Initializing..."};
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog("stage: initial renderFrame begin\n");
    #endif
    renderFrame(frame, vp, state, cfg, debugMode);
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog("stage: initial renderFrame done\n");
    #endif

    int lastAttr = INT32_MIN;
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog("stage: initial draw loop begin\n");
    #endif
    for (int y = 0; y < rows; ++y) {
      for (int x = 0; x < cols; ++x) {
        const auto& c = frame.at(x, y);
        if (c.ch == kWideContinuation) continue;
        const int attr = ncurses::styleToAttr(c.style);
        if (attr != lastAttr) {
          attrset(attr);
          lastAttr = attr;
        }

        // Prefer ASCII/ACS for conservative rendering, but allow wide glyphs
        // for localized UI strings.
        if (c.ch == 0x2593 || c.ch == 0x2588) {
          mvaddch(y, x, ncurses::cellToChtype(c.ch));
        } else if (c.ch >= 0 && c.ch <= 0x7f) {
          mvaddch(y, x, ncurses::cellToChtype(c.ch));
        } else if (c.ch == 0 || c.ch == L' ') {
          mvaddch(y, x, ' ');
        } else {
          #if defined(_WIN32)
          // Some Windows curses backends (notably under ConPTY/VS Code terminal)
          // can crash when rendering wide characters.
          mvaddch(y, x, '?');
          #else
          wchar_t buf[2] = {c.ch, 0};
          mvaddnwstr(y, x, buf, 1);
          #endif
        }
      }
    }
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog("stage: initial draw loop done\n");
    #endif
    attrset(A_NORMAL);
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog("stage: initial refresh begin\n");
    if (!aiz_windows_safe_refresh()) {
      windowsAppendTuiLog("ai-z: EXCEPTION during refresh()\n");
      const std::string msg =
          "ai-z: TUI crashed during screen refresh in this terminal.\n"
          "ai-z: try Windows Terminal/cmd.exe, or run --hardware.\n";
      std::cerr << msg;
      return 1;
    }
    windowsAppendTuiLog("stage: initial refresh done\n");
    #else
    refresh();
    #endif

    // Seed the diff cache and last-known geometry so the first loop iteration
    // doesn't immediately erase + redraw the entire screen.
    prevFrame = std::move(frame);
    havePrevFrame = true;
    lastRows = rows;
    lastCols = cols;
    lastScreen = state.screen;
  }

  CpuUsageCollector cpuCol;
  CpuMaxCoreUsageCollector cpuMaxCol;
  DiskBandwidthCollector diskReadCol(DiskBandwidthMode::Read);
  DiskBandwidthCollector diskWriteCol(DiskBandwidthMode::Write);
  NetworkBandwidthCollector netRxCol(NetworkBandwidthMode::Rx);
  NetworkBandwidthCollector netTxCol(NetworkBandwidthMode::Tx);
  ProcessSampler procSampler;

  // Debug generators (used only when --debug is passed)
  RandomWalk dbgCpu(0.0, 100.0, 10.0);
  RandomWalk dbgCpuMax(0.0, 100.0, 12.0);
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
    clearok(stdscr, TRUE);
    clear();
    ncurses::drawHeader(cols, "AI-Z - Starting");
    mvhline(2, 0, ' ', cols);
    mvaddnstr(2, 0, "Initializing NVML...", cols);
    #if defined(AI_Z_PLATFORM_WINDOWS)
    if (!aiz_windows_safe_refresh()) {
      windowsAppendTuiLog("ai-z: EXCEPTION during refresh()\n");
      const std::string msg =
          "ai-z: TUI crashed during screen refresh in this terminal.\n"
          "ai-z: try Windows Terminal/cmd.exe, or run --hardware.\n";
      std::cerr << msg;
      return 1;
    }
    #else
    refresh();
    #endif
    // We drew directly via ncurses, so the terminal no longer matches prevFrame.
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
  const bool disableGpuSampler = windowsEnvFlagSet("AI_Z_DISABLE_GPU_SAMPLER");
  if (!debugMode && !disableGpuSampler) {
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog("stage: starting gpu sampler\n");
    #endif
    gpuSampler.start();
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog("stage: gpu sampler started\n");
    #endif
  } else if (!debugMode && disableGpuSampler) {
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog("stage: gpu sampler disabled by AI_Z_DISABLE_GPU_SAMPLER\n");
    #endif
  }
#if defined(_WIN32)
  if (!debugMode && !hasNvml && !ncurses::GpuTelemetrySampler::isPcieThroughputSupported()) {
    cfg.showPcieRx = false;
    cfg.showPcieTx = false;
  }
#endif

  // Use shared-core timelines so the Frame renderer can display them.
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

  // Defer heavy probing until the user opens Hardware/Benchmarks.
  HardwareInfo hwCache;
  std::vector<std::string> hwLines = {"(Press W / F2 to probe hardware)", "(Press B / F3 to load benchmarks)"};
  bool hwReady = false;

  // Boot-time hardware probe (background): this is primarily to get CPU/GPU names
  // for Timelines titles reliably, without requiring the user to open the Hardware screen.
  ncurses::BootHardwareProbe bootProbe;
  const bool disableBootProbe = windowsEnvFlagSet("AI_Z_DISABLE_BOOT_PROBE");
  #if defined(_WIN32)
  // On some Windows systems, HardwareInfo::probe() is not safe to run from a
  // background thread (can crash via COM/WMI/WDDM calls). Keep it opt-in.
  const bool enableBootProbeThread = windowsEnvFlagSet("AI_Z_ENABLE_BOOT_PROBE");
  #else
  const bool enableBootProbeThread = true;
  #endif

  if (!debugMode && !disableBootProbe && enableBootProbeThread) {
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog("stage: starting boot probe\n");
    #endif
    bootProbe.start();
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog("stage: boot probe started\n");
    #endif
  } else if (!debugMode && disableBootProbe) {
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog("stage: boot probe disabled by AI_Z_DISABLE_BOOT_PROBE\n");
    #endif
  } else if (!debugMode && !enableBootProbeThread) {
    #if defined(AI_Z_PLATFORM_WINDOWS)
    windowsAppendTuiLog("stage: boot probe thread disabled by default on Windows (set AI_Z_ENABLE_BOOT_PROBE=1 to enable)\n");
    #endif
  }

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
    clearok(stdscr, TRUE);
    clear();
    ncurses::drawHeader(cols, "AI-Z - Initializing");
    mvhline(2, 0, ' ', cols);
    mvaddnstr(2, 0, "Probing hardware...", cols);
    #if defined(AI_Z_PLATFORM_WINDOWS)
    if (!aiz_windows_safe_refresh()) {
      windowsAppendTuiLog("ai-z: EXCEPTION during refresh()\n");
      const std::string msg =
          "ai-z: TUI crashed during screen refresh in this terminal.\n"
          "ai-z: try Windows Terminal/cmd.exe, or run --hardware.\n";
      std::cerr << msg;
      return;
    }
    #else
    refresh();
    #endif
    // We drew directly via ncurses, so the terminal no longer matches prevFrame.
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
    bool sawResizeKey = false;
    if (smokeMs) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - smokeStart);
      if (elapsed.count() >= static_cast<long long>(*smokeMs)) {
        break;
      }
    }
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
      // If the UI is rendering/sampling slowly, key-repeat can build up in the input
      // buffer. Drain any queued input immediately so navigation doesn't "coast"
      // for seconds after releasing a key.
      const std::vector<int> queuedKeys = ncurses::readAndDrainKeys(waitMs);

      for (const int k : queuedKeys) {
        #if defined(KEY_RESIZE)
        if (k == KEY_RESIZE) {
          sawResizeKey = true;
          continue;
        }
        #endif

        // Sampling / scroll speed stays backend-local for now.
        if (k == '+' || k == '=') {
          adjustRefreshMs(true);
          continue;
        }
        if (k == '-' || k == '_') {
          adjustRefreshMs(false);
          continue;
        }

        const auto cmdOpt = ncurses::keyToCommand(k, state.screen);
        if (!cmdOpt) continue;
        const Command cmd = *cmdOpt;

        if (cmd == Command::Quit) {
          running = false;
          continue;
        }

        // Navigation + selection changes are centralized.
        if (cmd == Command::NavHelp || cmd == Command::NavHardware || cmd == Command::NavBenchmarks || cmd == Command::NavConfig ||
          cmd == Command::NavMinimal || cmd == Command::NavProcesses || cmd == Command::Back || cmd == Command::Up || cmd == Command::Down ||
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

        // Config screen actions are centralized in the core.
        if (state.screen == Screen::Config &&
            (cmd == Command::Toggle || cmd == Command::Defaults || cmd == Command::Save || cmd == Command::Activate ||
             cmd == Command::Left || cmd == Command::Right)) {
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

    // Handle terminal resize events (or missed resize notifications).
    // On ncurses/PDCurses the application must explicitly resize the stdscr window.
    bool forceFullRedraw = false;
    {
      int rows = 0, cols = 0;
      getmaxyx(stdscr, rows, cols);

      #if defined(AI_Z_PLATFORM_WINDOWS)
      // Windows Terminal / ConPTY may not reliably send KEY_RESIZE.
      // Poll the real console size every frame and resize if it changed.
      int winCols = 0, winRows = 0;
      if (windowsGetConsoleWindowSize(&winCols, &winRows) && winCols > 0 && winRows > 0) {
        const bool sizeChanged = (winCols != cols || winRows != rows);
        const bool needResize = sawResizeKey || sizeChanged;
        if (needResize) {
          // Grow the console screen buffer if needed so the window can expand.
          windowsEnsureConsoleBufferAtLeast(winCols, winRows);
          windowsFixCursesEnvFromConsole();

          #if defined(PDC_BUILD)
          // PDCurses: resize_term resizes stdscr and internal state.
          // Pass 0,0 first to force a full internal reset, then real size.
          resize_term(0, 0);
          resize_term(winRows, winCols);
          #elif defined(NCURSES_VERSION)
          resizeterm(winRows, winCols);
          #endif

          // Force curses to completely repaint on next refresh.
          wclear(stdscr);
          clearok(stdscr, TRUE);
          forceFullRedraw = true;
        }
      } else if (sawResizeKey) {
        // Best-effort: if we saw a resize key but can't query the console size,
        // still force a full redraw.
        wclear(stdscr);
        clearok(stdscr, TRUE);
        forceFullRedraw = true;
      }
      #else
      if (sawResizeKey) {
        #if defined(PDC_BUILD)
        // PDCurses expects resize_term to be called after KEY_RESIZE.
        resize_term(0, 0);
        resize_term(rows, cols);
        #elif defined(NCURSES_VERSION)
        // ncurses expects resizeterm to be called after KEY_RESIZE.
        resizeterm(rows, cols);
        #endif
        wclear(stdscr);
        clearok(stdscr, TRUE);
        forceFullRedraw = true;
      }
      #endif

      if (forceFullRedraw) {
        havePrevFrame = false;
      }
    }

    int rows = 0, cols = 0;
    getmaxyx(stdscr, rows, cols);

    // Ensure we can render a full-width scrolling graph without trailing empty columns.
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

    std::optional<Sample> cpu;
    std::optional<Sample> cpuMax;
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
      const double cpuVal = dbgCpu.next();
      cpu = Sample{cpuVal, "%", "debug"};
      cpuMax = Sample{std::max(cpuVal, dbgCpuMax.next()), "%", "debug"};
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
      gt.gpuClockMHz = static_cast<unsigned int>(1200 + util * 10.0);
      gt.memClockMHz = static_cast<unsigned int>(7000 + util * 5.0);
      gt.encoderUtilPct = util * 0.6;
      gt.decoderUtilPct = util * 0.4;
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
      cpuMax = cpuMaxCol.sample();
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

    std::optional<Sample> gpuClock;
    std::optional<Sample> gpuMemClock;
    std::optional<Sample> gpuEnc;
    std::optional<Sample> gpuDec;
    if (!gpuTel.empty() && gpuTel[0]) {
      const auto& gt0 = *gpuTel[0];
      const std::string label = !gt0.source.empty() ? gt0.source : std::string("gpu");
      if (gt0.gpuClockMHz) gpuClock = Sample{static_cast<double>(*gt0.gpuClockMHz), "MHz", label};
      if (gt0.memClockMHz) gpuMemClock = Sample{static_cast<double>(*gt0.memClockMHz), "MHz", label};
      if (gt0.encoderUtilPct) gpuEnc = Sample{*gt0.encoderUtilPct, "%", label};
      if (gt0.decoderUtilPct) gpuDec = Sample{*gt0.decoderUtilPct, "%", label};
    }

    // Update shared-core snapshot + timelines.
    state.tick = uiTick;
    state.latest.cpu = cpu;
    state.latest.cpuMax = cpuMax;
    state.latest.disk = disk;
    state.latest.diskRead = diskRead;
    state.latest.diskWrite = diskWrite;
    state.latest.netRx = netRx;
    state.latest.netTx = netTx;
    // GPU0 as a Sample is still used by some panels.
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

    // Keep hardware lines available for the core renderer.
    state.hardwareLines = hwLines;

    // Processes screen (best-effort snapshot).
    if (state.screen == Screen::Processes) {
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
          if (it->second.cmdline.empty()) {
            if (const auto id = readProcessIdentity(pid)) {
              if (!id->cmdline.empty()) it->second.cmdline = id->cmdline;
              if (it->second.name.empty()) it->second.name = id->name;
              if (it->second.ramBytes == 0) it->second.ramBytes = id->ramBytes;
            }
          }
        }
      }

      std::vector<TuiState::ProcessEntry> rows;
      rows.reserve(merged.size());
      for (auto& kv : merged) {
        if (state.processesGpuOnly) {
          const auto& e = kv.second;
          const bool onGpu = e.gpuIndex.has_value() || e.gpuUtilPct.has_value() || e.vramUsedGiB.has_value();
          if (!onGpu) continue;
        }
        rows.push_back(std::move(kv.second));
      }

      auto toLower = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char ch : s) out.push_back(static_cast<char>(std::tolower(ch)));
        return out;
      };

      std::sort(rows.begin(), rows.end(), [&](const TuiState::ProcessEntry& a, const TuiState::ProcessEntry& b) {
        switch (state.processSort) {
          case TuiState::ProcessSort::Name: {
            const std::string aName = toLower(a.name);
            const std::string bName = toLower(b.name);
            if (aName != bName) return aName < bName;
            return a.pid < b.pid;
          }
          case TuiState::ProcessSort::Cpu:
            if (a.cpuPct != b.cpuPct) return a.cpuPct > b.cpuPct;
            return a.pid < b.pid;
          case TuiState::ProcessSort::Gpu: {
            const double aGpu = a.gpuUtilPct.value_or(0.0);
            const double bGpu = b.gpuUtilPct.value_or(0.0);
            if (aGpu != bGpu) return aGpu > bGpu;
            return a.pid < b.pid;
          }
          case TuiState::ProcessSort::Ram:
            if (a.ramBytes != b.ramBytes) return a.ramBytes > b.ramBytes;
            return a.pid < b.pid;
          case TuiState::ProcessSort::Vram: {
            const double aVram = a.vramUsedGiB.value_or(0.0);
            const double bVram = b.vramUsedGiB.value_or(0.0);
            if (aVram != bVram) return aVram > bVram;
            return a.pid < b.pid;
          }
          default:
            return a.pid < b.pid;
        }
      });

      if (maxCount > 0 && rows.size() > maxCount) rows.resize(maxCount);
      state.processes = std::move(rows);
    }

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
        std::vector<std::string> fromHw;
      #if defined(_WIN32)
        fromHw.resize(gpuCount);
        for (unsigned int i = 0; i < gpuCount; ++i) fromHw[i] = "GPU" + std::to_string(i);
      #else
        fromHw = ncurses::parseGpuNames(hwCache, gpuCount);
      #endif
        if (ncurses::hasRealDeviceNames(fromHw)) state.gpuDeviceNames = std::move(fromHw);
      }
      applyTimelineDevicesFromHw(state, hwCache);

      bootApplied = true;
    }

    // Best-effort device names for timeline section titles.
    // Only overwrite when we have *better* information.
    if (!hwCache.cpuName.empty() && hwCache.cpuName != "--" && hwCache.cpuName != "unknown") {
      state.cpuDevice = hwCache.cpuName;
    }

    if (!ncurses::hasRealDeviceNames(state.gpuDeviceNames)) {
      std::vector<std::string> fromHw;
    #if defined(_WIN32)
      fromHw.resize(gpuCount);
      for (unsigned int i = 0; i < gpuCount; ++i) fromHw[i] = "GPU" + std::to_string(i);
    #else
      fromHw = ncurses::parseGpuNames(hwCache, gpuCount);
    #endif
      if (ncurses::hasRealDeviceNames(fromHw)) {
        state.gpuDeviceNames = std::move(fromHw);
      }
    }

    applyTimelineDevicesFromHw(state, hwCache);

    // Per-GPU utilization history.
    if (state.gpuTls.size() != gpuCount) {
      state.gpuTls.clear();
      state.gpuTls.reserve(gpuCount);
      for (unsigned int i = 0; i < gpuCount; ++i) state.gpuTls.emplace_back(desiredSamples);
    }
    for (unsigned int i = 0; i < gpuCount; ++i) {
      ensureTimelineCapacity(state.gpuTls[static_cast<std::size_t>(i)], desiredSamples);
      const auto& s = (i < gpuSamples.size()) ? gpuSamples[static_cast<std::size_t>(i)] : std::optional<Sample>{};
      state.gpuTls[static_cast<std::size_t>(i)].push((s && (cfg.showGpu || cfg.showGpuBars)) ? s->value : 0.0);
    }

    // Render via shared core and blit to ncurses.
    const bool fullRedraw = forceFullRedraw || (rows != lastRows || cols != lastCols || state.screen != lastScreen);
    if (fullRedraw) {
      wclear(stdscr);
      clearok(stdscr, TRUE);
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
          if (c.ch == kWideContinuation) continue;
          const int attr = ncurses::styleToAttr(c.style);
          if (attr != lastAttr) {
            attrset(attr);
            lastAttr = attr;
          }

          if (c.ch == 0x2593 || c.ch == 0x2588) {
            mvaddch(y, x, ncurses::cellToChtype(c.ch));
          } else if (c.ch >= 0 && c.ch <= 0x7f) {
            mvaddch(y, x, ncurses::cellToChtype(c.ch));
          } else if (c.ch == 0 || c.ch == L' ') {
            mvaddch(y, x, ' ');
          } else {
            #if defined(_WIN32)
            mvaddch(y, x, '?');
            #else
            wchar_t buf[2] = {c.ch, 0};
            mvaddnwstr(y, x, buf, 1);
            #endif
          }
        }
      }
    } else {
      const std::size_t n = frame.cells.size();
      for (std::size_t i = 0; i < n; ++i) {
        const auto& cur = frame.cells[i];
        const auto& prev = prevFrame.cells[i];
        if (cur.ch == prev.ch && cur.style == prev.style) continue;

        if (cur.ch == kWideContinuation) continue;

        const int y = static_cast<int>(i / static_cast<std::size_t>(frame.width));
        const int x = static_cast<int>(i % static_cast<std::size_t>(frame.width));

        const int attr = ncurses::styleToAttr(cur.style);
        if (attr != lastAttr) {
          attrset(attr);
          lastAttr = attr;
        }

        if (cur.ch == 0x2593 || cur.ch == 0x2588) {
          mvaddch(y, x, ncurses::cellToChtype(cur.ch));
        } else if (cur.ch >= 0 && cur.ch <= 0x7f) {
          mvaddch(y, x, ncurses::cellToChtype(cur.ch));
        } else if (cur.ch == 0 || cur.ch == L' ') {
          mvaddch(y, x, ' ');
        } else {
          #if defined(_WIN32)
          mvaddch(y, x, '?');
          #else
          wchar_t buf[2] = {cur.ch, 0};
          mvaddnwstr(y, x, buf, 1);
          #endif
        }
      }
    }

    prevFrame = std::move(frame);
    havePrevFrame = true;
    attrset(A_NORMAL);

    #if defined(AI_Z_PLATFORM_WINDOWS)
    if (!aiz_windows_safe_refresh()) {
      windowsAppendTuiLog("ai-z: EXCEPTION during refresh()\n");
      const std::string msg =
          "ai-z: TUI crashed during screen refresh in this terminal.\n"
          "ai-z: try Windows Terminal/cmd.exe, or run --hardware.\n";
      std::cerr << msg;
      return 1;
    }
    #else
    refresh();
    #endif

  }

  gpuSampler.stop();
  bootProbe.stop();
  // Benchmarks are best-effort; allow them to finish before tearing down ncurses.
  ncurses::benchShutdown(benchThread);
  endwin();
  return 0;
}

}  // namespace aiz
