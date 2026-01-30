#pragma once

#include <aiz/tui/tui_core.h>

#include <cstdint>
#include <optional>
#include <vector>

struct notcurses;
struct ncinput;

namespace aiz::notcurses_impl {

// Input event result.
struct InputEvent {
  std::optional<Command> cmd;
  bool isResize = false;
  std::uint32_t keyId = 0;  // Raw key ID for debugging.
};

// Read input with a timeout. Returns the parsed event (if any).
InputEvent readInput(struct notcurses* nc, std::uint32_t timeoutMs, Screen screen);

// Convert a notcurses input event to a command.
std::optional<Command> inputToCommand(const struct ncinput& ni, Screen screen);

// Drain any queued input events to avoid backlog.
std::vector<InputEvent> drainInput(struct notcurses* nc, Screen screen);

}  // namespace aiz::notcurses_impl
