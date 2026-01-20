#pragma once

#include <aiz/tui/tui_core.h>

#include <cstdint>
#include <string>

#include <curses.h>

namespace aiz::ncurses {

int styleToAttr(std::uint16_t style);
chtype cellToChtype(wchar_t ch);
void drawHeader(int cols, const std::string& title);

}  // namespace aiz::ncurses
