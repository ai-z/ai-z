#pragma once

#include <aiz/config/config.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <aiz/tui/telemetry.h>

#include <aiz/metrics/timeline.h>

namespace aiz {

class IBenchmark;

enum class Screen {
  Timelines,
  Minimal,
  Help,
  Benchmarks,
  Config,
  Hardware,
  Processes,
};

enum class TimelineView {
  Timelines,
  Bars,
};

enum class Command {
  None,
  Quit,
  Back,

  NavHelp,
  NavHardware,
  NavBenchmarks,
  NavConfig,
  NavMinimal,
  NavProcesses,

  Up,
  Down,
  Left,
  Right,
  Activate,
  Toggle,
  Save,
  Defaults,
  Refresh,

  SortProcessName,
  SortCpu,
  SortGpu,
  SortRam,
  SortVram,
  ToggleGpuOnly,

  BenchRunAll,
  BenchReport,

  ViewTimelines,
  ViewBars,
  ViewMinimal,
};

struct KeyEvent {
  Command cmd = Command::None;
};

struct TuiState {
  enum class ProcessSort {
    Name,
    Cpu,
    Gpu,
    Ram,
    Vram,
  };

  Screen screen = Screen::Timelines;
  TimelineView timelineView = TimelineView::Timelines;

  // Simple per-screen cursors.
  int benchmarksSel = 0;
  int configSel = 0;
  int configCol = 0;  // 0 = Timelines, 1 = H. Bars

  // Benchmarks screen.
  std::vector<std::unique_ptr<IBenchmark>> benches;
  // Benchmarks UI rows mirror `benches` 1:1 (including header rows represented by nullptr benches).
  std::vector<std::string> benchRowTitles;
  std::vector<bool> benchRowIsHeader;
  std::vector<std::string> benchResults;
  std::string lastBenchResult;

  // Benchmarks execution state (updated from a worker thread in some backends).
  // Protected by benchMutex.
  bool benchmarksRunning = false;
  int runningBenchIndex = -1;  // 0-based index into benches, or -1 when idle.
  mutable std::mutex benchMutex;

  // UI messages.
  std::optional<std::string> statusLine;

  // Processes screen: latest snapshot of top processes.
  ProcessSort processSort = ProcessSort::Cpu;
  bool processesGpuOnly = true;
  struct ProcessEntry {
    int pid = 0;
    std::string name;
    std::string cmdline;
    double cpuPct = 0.0;
    std::uint64_t ramBytes = 0;
    std::optional<double> gpuUtilPct;
    std::optional<double> vramUsedGiB;
    std::optional<int> gpuIndex;
  };
  std::vector<ProcessEntry> processes;

  // Timelines screen: latest telemetry snapshot.
  TelemetrySnapshot latest;

  // Device names for section titles (best-effort; may be empty until probed).
  std::string cpuDevice;
  std::vector<std::string> gpuDeviceNames;
  std::string ramDevice;
  std::string diskDevice;
  std::string netDevice;

  // Timelines history (oldest -> newest) for scrolling bars.
  Timeline cpuTl{240};
  Timeline cpuMaxTl{240};
  std::vector<Timeline> gpuTls;
  Timeline gpuMemUtilTl{240};
  Timeline gpuClockTl{240};
  Timeline gpuMemClockTl{240};
  Timeline gpuEncTl{240};
  Timeline gpuDecTl{240};
  Timeline ramTl{240};
  Timeline vramTl{240};
  Timeline diskTl{240};
  Timeline diskReadTl{240};
  Timeline diskWriteTl{240};
  Timeline netRxTl{240};
  Timeline netTxTl{240};
  Timeline pcieRxTl{240};
  Timeline pcieTxTl{240};

  // Debug: increments each rendered frame.
  std::uint64_t tick = 0;

  // Hardware screen: cache probe output and refresh on demand.
  std::vector<std::string> hardwareLines;
  bool hardwareDirty = true;
};

struct Cell {
  wchar_t ch = L' ';
  std::uint16_t style = 0;
};

struct Frame {
  int width = 0;
  int height = 0;
  std::vector<Cell> cells;

  void resize(int w, int h);
  void clear(Cell fill = {});

  Cell& at(int x, int y);
  const Cell& at(int x, int y) const;
};

// Sentinel used by the shared renderer to mark columns that are covered by the
// previous cell's wide glyph (e.g., CJK characters). Backends must avoid
// drawing into these cells.
constexpr wchar_t kWideContinuation = static_cast<wchar_t>(0xFFFF);

// All styles are backend-defined mappings.
enum class Style : std::uint16_t {
  Default = 0,
  Header = 1,
  FooterKey = 2,
  FooterBlock = 7,
  FooterHot = 8,
  FooterActive = 9,
  Hot = 3,
  Section = 4,
  Value = 5,
  Warning = 6,
};

struct Viewport {
  int width = 0;
  int height = 0;
};

// Drives shared state transitions.
// Note: this may mutate cfg for screen-local actions (e.g., Config toggles).
void applyCommand(TuiState& state, Config& cfg, Command cmd);

// Renders the current screen into a full frame.
void renderFrame(Frame& out, const Viewport& vp, const TuiState& state, const Config& cfg, bool debugMode);

}  // namespace aiz
