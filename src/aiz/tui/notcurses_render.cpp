#include "notcurses_render.h"

#include <notcurses/notcurses.h>

namespace aiz::notcurses_impl {

// Color scheme (24-bit RGB).
namespace colors {
  // Header/title bar.
  constexpr std::uint32_t headerFg = 0x000000;  // black
  constexpr std::uint32_t headerBg = 0x00ff00;  // green

  // Footer block (F-key labels).
  constexpr std::uint32_t footerBlockFg = 0xffffff;  // white
  constexpr std::uint32_t footerBlockBg = 0x0000ff;  // blue

  // Footer hot letter.
  constexpr std::uint32_t footerHotFg = 0xffff00;  // yellow
  constexpr std::uint32_t footerHotBg = 0x0000ff;  // blue

  // Footer active page.
  constexpr std::uint32_t footerActiveFg = 0xffffff;  // white
  constexpr std::uint32_t footerActiveBg = 0xff0000;  // red

  // Footer key labels (outside block).
  constexpr std::uint32_t footerKeyFg = 0x00ffff;  // cyan

  // Hot/highlight.
  constexpr std::uint32_t hotFg = 0xffff00;  // yellow

  // Section titles.
  constexpr std::uint32_t sectionFg = 0x00ffff;  // cyan

  // Values.
  constexpr std::uint32_t valueFg = 0x00ff00;  // green

  // Warning.
  constexpr std::uint32_t warningFg = 0xff0000;  // red

  // Default.
  constexpr std::uint32_t defaultFg = 0xc0c0c0;  // light gray
}

std::uint64_t styleToChannels(std::uint16_t style) {
  std::uint64_t channels = 0;

  switch (static_cast<Style>(style)) {
    case Style::Header:
      ncchannels_set_fg_rgb(&channels, colors::headerFg);
      ncchannels_set_bg_rgb(&channels, colors::headerBg);
      break;
    case Style::FooterBlock:
      ncchannels_set_fg_rgb(&channels, colors::footerBlockFg);
      ncchannels_set_bg_rgb(&channels, colors::footerBlockBg);
      break;
    case Style::FooterHot:
      ncchannels_set_fg_rgb(&channels, colors::footerHotFg);
      ncchannels_set_bg_rgb(&channels, colors::footerHotBg);
      break;
    case Style::FooterActive:
      ncchannels_set_fg_rgb(&channels, colors::footerActiveFg);
      ncchannels_set_bg_rgb(&channels, colors::footerActiveBg);
      break;
    case Style::FooterKey:
      ncchannels_set_fg_rgb(&channels, colors::footerKeyFg);
      ncchannels_set_bg_default(&channels);
      break;
    case Style::Hot:
      ncchannels_set_fg_rgb(&channels, colors::hotFg);
      ncchannels_set_bg_default(&channels);
      break;
    case Style::Section:
      ncchannels_set_fg_rgb(&channels, colors::sectionFg);
      ncchannels_set_bg_default(&channels);
      break;
    case Style::Value:
      ncchannels_set_fg_rgb(&channels, colors::valueFg);
      ncchannels_set_bg_default(&channels);
      break;
    case Style::Warning:
      ncchannels_set_fg_rgb(&channels, colors::warningFg);
      ncchannels_set_bg_default(&channels);
      break;
    case Style::Default:
    default:
      ncchannels_set_fg_rgb(&channels, colors::defaultFg);
      ncchannels_set_bg_default(&channels);
      break;
  }

  return channels;
}

void putCell(ncplane* plane, int y, int x, const Cell& cell) {
  if (cell.ch == kWideContinuation) return;

  nccell c = NCCELL_TRIVIAL_INITIALIZER;
  c.channels = styleToChannels(cell.style);

  if (cell.ch == 0 || cell.ch == L' ') {
    nccell_load(plane, &c, " ");
  } else if (cell.ch <= 0x7f) {
    char buf[2] = {static_cast<char>(cell.ch), '\0'};
    nccell_load(plane, &c, buf);
  } else {
    // Unicode character - encode as UTF-8.
    char buf[8] = {0};
    wchar_t wbuf[2] = {cell.ch, 0};
    
    // Simple UTF-8 encoding for BMP characters.
    if (cell.ch < 0x80) {
      buf[0] = static_cast<char>(cell.ch);
    } else if (cell.ch < 0x800) {
      buf[0] = static_cast<char>(0xc0 | (cell.ch >> 6));
      buf[1] = static_cast<char>(0x80 | (cell.ch & 0x3f));
    } else if (cell.ch < 0x10000) {
      buf[0] = static_cast<char>(0xe0 | (cell.ch >> 12));
      buf[1] = static_cast<char>(0x80 | ((cell.ch >> 6) & 0x3f));
      buf[2] = static_cast<char>(0x80 | (cell.ch & 0x3f));
    } else {
      buf[0] = static_cast<char>(0xf0 | (cell.ch >> 18));
      buf[1] = static_cast<char>(0x80 | ((cell.ch >> 12) & 0x3f));
      buf[2] = static_cast<char>(0x80 | ((cell.ch >> 6) & 0x3f));
      buf[3] = static_cast<char>(0x80 | (cell.ch & 0x3f));
    }
    nccell_load(plane, &c, buf);
  }

  ncplane_putc_yx(plane, y, x, &c);
  nccell_release(plane, &c);
}

void drawHeader(ncplane* plane, int cols, const std::string& title) {
  // Clear the header row.
  std::uint64_t clearChannels = 0;
  ncchannels_set_bg_default(&clearChannels);
  ncchannels_set_fg_default(&clearChannels);

  for (int x = 0; x < cols; ++x) {
    ncplane_putchar_yx(plane, 0, x, ' ');
  }

  // Check if title starts with "AI-Z".
  constexpr const char* kPrefix = "AI-Z";
  const bool startsWithAiz = title.rfind(kPrefix, 0) == 0;

  int x = 0;
  if (startsWithAiz) {
    // Draw " AI-Z " with green background.
    const std::string block = " AI-Z ";
    std::uint64_t blockChannels = 0;
    ncchannels_set_fg_rgb(&blockChannels, colors::headerFg);
    ncchannels_set_bg_rgb(&blockChannels, colors::headerBg);

    ncplane_set_channels(plane, blockChannels);
    ncplane_putstr_yx(plane, 0, 0, block.c_str());
    x = static_cast<int>(block.size());

    // Draw the rest with default background.
    std::string rest = title.substr(4);
    if (rest.empty() || rest.front() != ' ') rest = " " + rest;

    ncplane_set_channels(plane, clearChannels);
    ncplane_putstr_yx(plane, 0, x, rest.c_str());
  } else {
    std::uint64_t blockChannels = 0;
    ncchannels_set_fg_rgb(&blockChannels, colors::headerFg);
    ncchannels_set_bg_rgb(&blockChannels, colors::headerBg);

    ncplane_set_channels(plane, blockChannels);
    ncplane_putstr_yx(plane, 0, 0, title.c_str());
  }

  ncplane_set_channels(plane, clearChannels);
}

void blitFrame(ncplane* stdplane, const Frame& frame, const Frame* prev) {
  const bool canDiff = prev && prev->width == frame.width && prev->height == frame.height;

  if (!canDiff) {
    // Full redraw.
    for (int y = 0; y < frame.height; ++y) {
      for (int x = 0; x < frame.width; ++x) {
        putCell(stdplane, y, x, frame.at(x, y));
      }
    }
  } else {
    // Differential update.
    for (int y = 0; y < frame.height; ++y) {
      for (int x = 0; x < frame.width; ++x) {
        const auto& cur = frame.at(x, y);
        const auto& prv = prev->at(x, y);
        if (cur.ch != prv.ch || cur.style != prv.style) {
          putCell(stdplane, y, x, cur);
        }
      }
    }
  }
}

}  // namespace aiz::notcurses_impl
