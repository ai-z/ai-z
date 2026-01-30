#pragma once

#include <aiz/tui/tui_core.h>

#include <cstdint>
#include <string>

struct notcurses;
struct ncplane;

namespace aiz::notcurses_impl {

// Convert Style enum to notcurses RGB channel values.
// Returns a packed 64-bit channel pair (fg | bg).
std::uint64_t styleToChannels(std::uint16_t style);

// Render a Frame cell to a notcurses ncplane at the given position.
void putCell(ncplane* plane, int y, int x, const Cell& cell);

// Draw the header bar at the top of the screen.
void drawHeader(ncplane* plane, int cols, const std::string& title);

// Blit an entire Frame to the standard plane.
void blitFrame(ncplane* stdplane, const Frame& frame, const Frame* prev);

}  // namespace aiz::notcurses_impl
