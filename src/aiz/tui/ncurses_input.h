#pragma once

#include <aiz/tui/tui_core.h>

#include <optional>
#include <vector>

namespace aiz::ncurses {

std::optional<Command> keyToCommand(int key, Screen screen);

// Reads up to one blocking key (timeout-controlled) and drains any queued keys
// immediately (to avoid input backlog when the UI is slow).
std::vector<int> readAndDrainKeys(std::uint32_t waitMs);

}  // namespace aiz::ncurses
