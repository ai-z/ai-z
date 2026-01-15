#pragma once

#include <aiz/config/config.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <aiz/tui/telemetry.h>

#include <aiz/metrics/timeline.h>

namespace aiz {

class IBenchmark;

enum class Screen {
  Timelines,
  Help,
  Benchmarks,
  Config,
  Hardware,
};

enum class Command {
  None,
  Quit,
  Back,

  NavHelp,
  NavHardware,
  NavBenchmarks,
  NavConfig,
  NavTimelines,

  Up,
  Down,
  Activate,
  Toggle,
  Save,
  Refresh,
};

struct KeyEvent {
  Command cmd = Command::None;
};

struct TuiState {
  Screen screen = Screen::Timelines;

  // Simple per-screen cursors.
  int benchmarksSel = 0;
  int configSel = 0;

  // Benchmarks screen.
  std::vector<std::unique_ptr<IBenchmark>> benches;
  std::vector<std::string> benchResults;
  std::string lastBenchResult;

  // UI messages.
  std::optional<std::string> statusLine;

  // Timelines screen: latest telemetry snapshot.
  TelemetrySnapshot latest;

  // Timelines history (oldest -> newest) for scrolling bars.
  Timeline cpuTl{240};
  Timeline ramTl{240};
  Timeline vramTl{240};
  Timeline diskTl{240};
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

// All styles are backend-defined mappings.
enum class Style : std::uint16_t {
  Default = 0,
  Header = 1,
  FooterKey = 2,
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
void applyCommand(TuiState& state, const Config& cfg, Command cmd);

// Renders the current screen into a full frame.
void renderFrame(Frame& out, const Viewport& vp, const TuiState& state, const Config& cfg, bool debugMode);

}  // namespace aiz
