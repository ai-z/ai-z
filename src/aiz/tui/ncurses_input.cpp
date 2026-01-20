#include "ncurses_input.h"

#include <curses.h>

namespace aiz::ncurses {

std::vector<int> readAndDrainKeys(std::uint32_t waitMs) {
  std::vector<int> queuedKeys;
  timeout(static_cast<int>(waitMs));
  const int ch = getch();

  if (ch == ERR) return queuedKeys;

  queuedKeys.push_back(ch);

  // Drain queued keys without waiting.
  timeout(0);
  for (;;) {
    const int next = getch();
    if (next == ERR) break;
    queuedKeys.push_back(next);
  }

  return queuedKeys;
}

std::optional<Command> keyToCommand(int key, Screen screen) {
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

}  // namespace aiz::ncurses
