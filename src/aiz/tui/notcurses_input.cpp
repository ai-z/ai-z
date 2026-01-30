#include "notcurses_input.h"

#include <notcurses/notcurses.h>

namespace aiz::notcurses_impl {

std::optional<Command> inputToCommand(const struct ncinput& ni, Screen screen) {
  const std::uint32_t id = ni.id;

  // Function keys (htop-style).
  if (id == NCKEY_F01) return Command::NavHelp;
  if (id == NCKEY_F02) return Command::NavHardware;
  if (id == NCKEY_F03) return Command::NavBenchmarks;
  if (id == NCKEY_F04) return Command::NavConfig;
  if (id == NCKEY_F05) return Command::NavMinimal;
  if (id == NCKEY_F06) return Command::NavProcesses;
  if (id == NCKEY_F10) return Command::Quit;

  // Letter shortcuts (case insensitive).
  if (id == 'h' || id == 'H') return Command::NavHelp;
  if (id == 'w' || id == 'W') return Command::NavHardware;
  if (id == 'b' || id == 'B') return Command::NavBenchmarks;
  if (id == 'c' || id == 'C') return Command::NavConfig;
  if (id == 'm' || id == 'M') return Command::NavMinimal;
  if (id == 'p' || id == 'P') return Command::NavProcesses;
  if (id == 'q' || id == 'Q') return Command::Quit;

  // Global back (ESC).
  if (id == NCKEY_ESC) return Command::Back;

  // Arrow keys.
  if (id == NCKEY_UP) return Command::Up;
  if (id == NCKEY_DOWN) return Command::Down;

  // Screen-specific commands.
  if (screen == Screen::Benchmarks) {
    if (id == '1') return Command::BenchRunAll;
    if (id == '2') return Command::BenchReport;
    if (id == NCKEY_ENTER || id == '\n') return Command::Activate;
  }

  if (screen == Screen::Timelines || screen == Screen::Minimal) {
    if (id == '1') return Command::ViewTimelines;
    if (id == '2') return Command::ViewBars;
    if (id == '3') return Command::ViewMinimal;
  }

  if (screen == Screen::Config) {
    if (id == NCKEY_LEFT) return Command::Left;
    if (id == NCKEY_RIGHT) return Command::Right;
    if (id == NCKEY_TAB || id == '\t') return Command::Right;
    if (id == ' ') return Command::Toggle;
    if (id == NCKEY_ENTER || id == '\n') return Command::Activate;
    if (id == 's' || id == 'S') return Command::Save;
    if (id == 'r' || id == 'R' || id == 'd' || id == 'D') return Command::Defaults;
  }

  if (screen == Screen::Hardware) {
    if (id == 'r' || id == 'R') return Command::Refresh;
  }

  if (screen == Screen::Processes) {
    if (id == '1') return Command::SortProcessName;
    if (id == '2') return Command::SortCpu;
    if (id == '3') return Command::SortGpu;
    if (id == '4') return Command::SortRam;
    if (id == '5') return Command::SortVram;
    if (id == 'g' || id == 'G') return Command::ToggleGpuOnly;
  }

  return std::nullopt;
}

InputEvent readInput(struct notcurses* nc, std::uint32_t timeoutMs, Screen screen) {
  InputEvent result;

  struct timespec ts;
  ts.tv_sec = timeoutMs / 1000;
  ts.tv_nsec = static_cast<long>((timeoutMs % 1000) * 1000000);

  struct ncinput ni;
  std::uint32_t id = notcurses_get(nc, &ts, &ni);

  if (id == 0) {
    // Timeout, no input.
    return result;
  }

  result.keyId = id;

  // Handle resize event.
  if (id == NCKEY_RESIZE) {
    result.isResize = true;
    return result;
  }

  // Convert to command.
  result.cmd = inputToCommand(ni, screen);
  return result;
}

std::vector<InputEvent> drainInput(struct notcurses* nc, Screen screen) {
  std::vector<InputEvent> events;

  // First, read with zero timeout to drain any queued input.
  struct timespec ts = {0, 0};
  struct ncinput ni;

  while (true) {
    std::uint32_t id = notcurses_get(nc, &ts, &ni);
    if (id == 0) break;

    InputEvent ev;
    ev.keyId = id;

    if (id == NCKEY_RESIZE) {
      ev.isResize = true;
    } else {
      ev.cmd = inputToCommand(ni, screen);
    }

    events.push_back(ev);
  }

  return events;
}

}  // namespace aiz::notcurses_impl
