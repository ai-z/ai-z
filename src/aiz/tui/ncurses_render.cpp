#include "ncurses_render.h"

namespace aiz::ncurses {

int styleToAttr(std::uint16_t style) {
  // Ncurses color pairs are configured in NcursesUi::run().
  const bool colors = has_colors() != 0;
  if (!colors) return A_NORMAL;

  switch (static_cast<Style>(style)) {
    case Style::Header:
      // Prefer colored text on default background (avoid full-row backgrounds).
      return COLOR_PAIR(5) | A_BOLD;
    case Style::FooterBlock:
      // Colored block behind F-key tokens (htop-style).
      return COLOR_PAIR(1) | A_BOLD;
    case Style::FooterHot:
      // Hot letter inside the footer block.
      return COLOR_PAIR(7) | A_BOLD;
    case Style::FooterActive:
      // Current page highlight in footer.
      return COLOR_PAIR(8) | A_BOLD;
    case Style::FooterKey:
      return COLOR_PAIR(4) | A_BOLD;
    case Style::Hot:
      return COLOR_PAIR(2) | A_BOLD;
    case Style::Section:
      return COLOR_PAIR(4) | A_BOLD;
    case Style::Value:
      return COLOR_PAIR(5) | A_BOLD;
    case Style::Warning:
      return COLOR_PAIR(6) | A_BOLD;
    case Style::Default:
    default:
      return A_NORMAL;
  }
}

chtype cellToChtype(wchar_t ch) {
  // Keep ncurses rendering conservative: prefer ASCII and ACS glyphs.
  // The core renderer uses Unicode block elements; map those to ncurses ACS.
  if (ch == 0x2593) return ACS_CKBOARD;  // htop-esque solid block
  if (ch == 0 || ch == L' ') return static_cast<chtype>(' ');
  if (ch >= 0 && ch <= 0x7f) return static_cast<chtype>(static_cast<unsigned char>(ch));
  return static_cast<chtype>('?');
}

void drawHeader(int cols, const std::string& title) {
  const bool colors = has_colors() != 0;

  // Clear the whole title bar with default background.
  mvhline(0, 0, ' ', cols);

  // If the title starts with "AI-Z", draw that prefix with green background,
  // and draw the remaining metrics text with default (black) background.
  constexpr const char* kPrefix = "AI-Z";
  const bool startsWithAiz = title.rfind(kPrefix, 0) == 0;

  int x = 0;
  if (colors && startsWithAiz) {
    const std::string block = " AI-Z ";
    attron(COLOR_PAIR(3) | A_BOLD);
    mvaddnstr(0, 0, block.c_str(), cols);
    attroff(COLOR_PAIR(3) | A_BOLD);
    x = static_cast<int>(block.size());

    // Skip the literal "AI-Z" from the incoming title.
    std::string rest = title.substr(4);
    // Ensure at least one space after the green block.
    if (rest.empty() || rest.front() != ' ') rest = " " + rest;
    const int avail = cols - x;
    mvaddnstr(0, x, rest.c_str(), (avail > 0) ? avail : 0);
  } else {
    if (colors) attron(COLOR_PAIR(3) | A_BOLD);
    mvaddnstr(0, 0, title.c_str(), cols);
    if (colors) attroff(COLOR_PAIR(3) | A_BOLD);
  }
}

}  // namespace aiz::ncurses
