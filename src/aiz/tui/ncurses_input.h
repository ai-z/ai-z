#pragma once

#include <aiz/tui/tui_core.h>

#include <optional>

namespace aiz::ncurses {

std::optional<Command> keyToCommand(int key, Screen screen);

}  // namespace aiz::ncurses
