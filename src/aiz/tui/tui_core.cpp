#include <aiz/tui/tui_core.h>

#include <aiz/bench/bench.h>
#include <aiz/i18n.h>
#include <aiz/version.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <limits>
#include <mutex>
#include <wchar.h>

#if defined(_WIN32)
static int wcwidth_compat(wchar_t ch) {
  if (ch == 0) return 0;
  if (iswcntrl(ch)) return -1;
  return 1;
}
#define wcwidth wcwidth_compat
#endif

namespace aiz {

struct ConfigToggleItem {
  i18n::MsgId label;
  bool Config::*field;
};

static constexpr std::array<ConfigToggleItem, 16> kConfigToggleItems = {{
  {i18n::MsgId::ConfigToggleCpuUsage, &Config::showCpu},
  {i18n::MsgId::ConfigToggleCpuHotCoreUsage, &Config::showCpuHot},
  {i18n::MsgId::ConfigToggleRamUsage, &Config::showRam},
  {i18n::MsgId::ConfigToggleGpuUsage, &Config::showGpu},
  {i18n::MsgId::ConfigToggleGpuMemCtrl, &Config::showGpuMem},
  {i18n::MsgId::ConfigToggleVramUsage, &Config::showVram},
  {i18n::MsgId::ConfigToggleGpuClock, &Config::showGpuClock},
  {i18n::MsgId::ConfigToggleGpuMemClock, &Config::showGpuMemClock},
  {i18n::MsgId::ConfigToggleGpuEnc, &Config::showGpuEnc},
  {i18n::MsgId::ConfigToggleGpuDec, &Config::showGpuDec},
  {i18n::MsgId::ConfigTogglePcieRx, &Config::showPcieRx},
  {i18n::MsgId::ConfigTogglePcieTx, &Config::showPcieTx},
  {i18n::MsgId::ConfigToggleDiskRead, &Config::showDiskRead},
  {i18n::MsgId::ConfigToggleDiskWrite, &Config::showDiskWrite},
  {i18n::MsgId::ConfigToggleNetRx, &Config::showNetRx},
  {i18n::MsgId::ConfigToggleNetTx, &Config::showNetTx},
}};
static constexpr std::array<ConfigToggleItem, 16> kConfigToggleItemsBars = {{
  {i18n::MsgId::ConfigToggleCpuUsage, &Config::showCpuBars},
  {i18n::MsgId::ConfigToggleCpuHotCoreUsage, &Config::showCpuHotBars},
  {i18n::MsgId::ConfigToggleRamUsage, &Config::showRamBars},
  {i18n::MsgId::ConfigToggleGpuUsage, &Config::showGpuBars},
  {i18n::MsgId::ConfigToggleGpuMemCtrl, &Config::showGpuMemBars},
  {i18n::MsgId::ConfigToggleVramUsage, &Config::showVramBars},
  {i18n::MsgId::ConfigToggleGpuClock, &Config::showGpuClockBars},
  {i18n::MsgId::ConfigToggleGpuMemClock, &Config::showGpuMemClockBars},
  {i18n::MsgId::ConfigToggleGpuEnc, &Config::showGpuEncBars},
  {i18n::MsgId::ConfigToggleGpuDec, &Config::showGpuDecBars},
  {i18n::MsgId::ConfigTogglePcieRx, &Config::showPcieRxBars},
  {i18n::MsgId::ConfigTogglePcieTx, &Config::showPcieTxBars},
  {i18n::MsgId::ConfigToggleDiskRead, &Config::showDiskReadBars},
  {i18n::MsgId::ConfigToggleDiskWrite, &Config::showDiskWriteBars},
  {i18n::MsgId::ConfigToggleNetRx, &Config::showNetRxBars},
  {i18n::MsgId::ConfigToggleNetTx, &Config::showNetTxBars},
 }};

static constexpr int configToggleCount() {
  return static_cast<int>(kConfigToggleItems.size());
}

static constexpr int configItemCount() {
  // read-only misc rows: samples/bucket, rate, peak toggle, peak window, value color, metric color, graph style
  return configToggleCount() + 7;
}

static constexpr int configPeakToggleRowIndex() {
  // Read-only misc rows follow toggles in order:
  // Samples per bucket, Sampling rate, Peak toggle, Peak window, Value color, Metric name color, Graph style.
  return configToggleCount() + 2;
}

static constexpr int configPeakWindowRowIndex() {
  return configToggleCount() + 3;
}

static constexpr int configMetricNameColorRowIndex() {
  // Read-only misc rows follow toggles in order:
  // Samples per bucket, Sampling rate, Peak toggle, Peak window, Value color, Metric name color, Graph style.
  return configToggleCount() + 5;
}

static constexpr int configGraphStyleRowIndex() {
  return configToggleCount() + 6;
}

void Frame::resize(int w, int h) {
  width = std::max(0, w);
  height = std::max(0, h);
  cells.assign(static_cast<std::size_t>(width * height), Cell{});
}

void Frame::clear(Cell fill) {
  std::fill(cells.begin(), cells.end(), fill);
}

Cell& Frame::at(int x, int y) {
  return cells[static_cast<std::size_t>(y * width + x)];
}

const Cell& Frame::at(int x, int y) const {
  return cells[static_cast<std::size_t>(y * width + x)];
}

static void drawText(Frame& f, int x, int y, const std::wstring& s, Style style) {
  if (y < 0 || y >= f.height) return;
  int cx = x;
  for (wchar_t ch : s) {
    if (cx < 0) {
      ++cx;
      continue;
    }
    if (cx >= f.width) break;
    auto& c = f.at(cx, y);
    c.ch = ch;
    c.style = static_cast<std::uint16_t>(style);

    int w = 1;
    if (ch != 0 && ch != L' ' && ch != kWideContinuation) {
      const int ww = ::wcwidth(ch);
      if (ww > 0) w = ww;
    }

    // Mark any extra columns covered by this glyph so backends avoid drawing
    // into them (CJK characters are typically width 2).
    for (int i = 1; i < w; ++i) {
      if (cx + i >= f.width) break;
      auto& cc = f.at(cx + i, y);
      cc.ch = kWideContinuation;
      cc.style = static_cast<std::uint16_t>(style);
    }

    cx += w;
  }
}

static void drawTextStyled(Frame& f, int x, int y, std::wstring_view s, const std::vector<Style>& styles) {
  if (y < 0 || y >= f.height) return;
  int cx = x;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const wchar_t ch = s[i];
    if (cx < 0) {
      ++cx;
      continue;
    }
    if (cx >= f.width) break;

    const Style st = (i < styles.size()) ? styles[i] : Style::Default;
    auto& c = f.at(cx, y);
    c.ch = ch;
    c.style = static_cast<std::uint16_t>(st);

    int w = 1;
    if (ch != 0 && ch != L' ' && ch != kWideContinuation) {
      const int ww = ::wcwidth(ch);
      if (ww > 0) w = ww;
    }

    for (int j = 1; j < w; ++j) {
      if (cx + j >= f.width) break;
      auto& cc = f.at(cx + j, y);
      cc.ch = kWideContinuation;
      cc.style = static_cast<std::uint16_t>(st);
    }

    cx += w;
  }
}

static std::size_t textWidth(std::wstring_view s) {
  std::size_t w = 0;
  for (wchar_t ch : s) {
    if (ch == kWideContinuation) continue;
    int ww = 1;
    if (ch != 0 && ch != L' ') {
      const int wcw = ::wcwidth(ch);
      if (wcw > 0) ww = wcw;
    }
    w += static_cast<std::size_t>(ww);
  }
  return w;
}

static std::wstring clipToWidth(std::wstring_view s, std::size_t maxW) {
  std::wstring out;
  out.reserve(s.size());
  std::size_t w = 0;
  for (wchar_t ch : s) {
    if (ch == kWideContinuation) continue;
    int ww = 1;
    if (ch != 0 && ch != L' ') {
      const int wcw = ::wcwidth(ch);
      if (wcw > 0) ww = wcw;
    }
    if (w + static_cast<std::size_t>(ww) > maxW) break;
    out.push_back(ch);
    w += static_cast<std::size_t>(ww);
  }
  return out;
}


void applyCommand(TuiState& state, Config& cfg, Command cmd) {
  switch (cmd) {
    case Command::Quit:
      // handled by backend
      return;
    case Command::NavHelp:
      state.screen = Screen::Help;
      return;
    case Command::NavHardware:
      state.screen = Screen::Hardware;
      return;
    case Command::NavBenchmarks:
      state.screen = Screen::Benchmarks;
      return;
    case Command::NavConfig:
      state.screen = Screen::Config;
      return;
    case Command::NavMinimal:
      state.screen = Screen::Minimal;
      return;
    case Command::NavProcesses:
      state.screen = Screen::Processes;
      return;
    case Command::Back:
      state.screen = Screen::Timelines;
      return;
    default:
      break;
  }

  // Screen-local actions.
  if (state.screen == Screen::Benchmarks) {
    const int maxSel = static_cast<int>(state.benches.size());  // +1 for "run all" (index 0)

    auto selectable = [&](int sel) {
      if (sel == 0) return true;  // run-all
      const int row = sel - 1;
      if (row < 0 || row >= static_cast<int>(state.benches.size())) return false;
      return state.benches[static_cast<std::size_t>(row)] != nullptr;
    };

    auto clampToSelectable = [&](int sel, int dir) {
      sel = std::clamp(sel, 0, maxSel);
      if (selectable(sel)) return sel;
      // Walk in requested direction first (dir = +1 down, -1 up).
      for (int s = sel; s >= 0 && s <= maxSel; s += dir) {
        if (selectable(s)) return s;
      }
      // Fallback: try the other direction.
      for (int s = sel; s >= 0 && s <= maxSel; s -= dir) {
        if (selectable(s)) return s;
      }
      return 0;
    };

    if (cmd == Command::Up) state.benchmarksSel = clampToSelectable(state.benchmarksSel - 1, -1);
    if (cmd == Command::Down) state.benchmarksSel = clampToSelectable(state.benchmarksSel + 1, +1);
    return;
  }

  if (state.screen == Screen::Config) {
    if (cmd == Command::Up) state.configSel = std::max(0, state.configSel - 1);
    if (cmd == Command::Down) state.configSel = std::min(configItemCount() - 1, state.configSel + 1);

    if (cmd == Command::Left) {
      state.configCol = 0;
      return;
    }
    if (cmd == Command::Right) {
      state.configCol = 1;
      return;
    }

    if (cmd == Command::Toggle) {
      if (state.configSel >= 0 && state.configSel < configToggleCount()) {
        const auto& items = (state.configCol == 0) ? kConfigToggleItems : kConfigToggleItemsBars;
        const auto& it = items[static_cast<std::size_t>(state.configSel)];
        cfg.*(it.field) = !(cfg.*(it.field));
      } else if (state.configSel == configPeakToggleRowIndex()) {
        cfg.showPeakValues = !cfg.showPeakValues;
      } else if (state.configSel == configPeakWindowRowIndex()) {
        // Cycle through peak window values: 10s -> 30s -> 60s -> 120s -> 10s
        if (cfg.peakWindowSec <= 10) cfg.peakWindowSec = 30;
        else if (cfg.peakWindowSec <= 30) cfg.peakWindowSec = 60;
        else if (cfg.peakWindowSec <= 60) cfg.peakWindowSec = 120;
        else cfg.peakWindowSec = 10;
      } else if (state.configSel == configMetricNameColorRowIndex()) {
        cfg.metricNameColor = (cfg.metricNameColor == MetricNameColor::Cyan)
            ? MetricNameColor::Green
            : MetricNameColor::Cyan;
      } else if (state.configSel == configGraphStyleRowIndex()) {
        // Cycle through graph styles: Braille -> Smooth -> Block -> Braille
        switch (cfg.timelineGraphStyle) {
          case TimelineGraphStyle::Braille:
            cfg.timelineGraphStyle = TimelineGraphStyle::Smooth;
            break;
          case TimelineGraphStyle::Smooth:
            cfg.timelineGraphStyle = TimelineGraphStyle::Block;
            break;
          case TimelineGraphStyle::Block:
          default:
            cfg.timelineGraphStyle = TimelineGraphStyle::Braille;
            break;
        }
      }
      return;
    }

    if (cmd == Command::Defaults) {
      cfg = Config{};
      return;
    }

    if (cmd == Command::Save) {
      cfg.save();
      state.statusLine = "Config saved to " + Config::path();
      return;
    }

    if (cmd == Command::Activate) {
      // Enter only activates the action row to avoid accidental toggles.
      return;
    }

    return;
  }

  if (state.screen == Screen::Processes) {
    if (cmd == Command::SortProcessName) state.processSort = TuiState::ProcessSort::Name;
    if (cmd == Command::SortCpu) state.processSort = TuiState::ProcessSort::Cpu;
    if (cmd == Command::SortGpu) state.processSort = TuiState::ProcessSort::Gpu;
    if (cmd == Command::SortRam) state.processSort = TuiState::ProcessSort::Ram;
    if (cmd == Command::SortVram) state.processSort = TuiState::ProcessSort::Vram;
    if (cmd == Command::ToggleGpuOnly) state.processesGpuOnly = !state.processesGpuOnly;
    return;
  }

  if (state.screen == Screen::Timelines || state.screen == Screen::Minimal) {
    if (cmd == Command::ViewTimelines) {
      state.screen = Screen::Timelines;
      state.timelineView = TimelineView::Timelines;
      return;
    }
    if (cmd == Command::ViewBars) {
      state.screen = Screen::Timelines;
      state.timelineView = TimelineView::Bars;
      return;
    }
    if (cmd == Command::ViewMinimal) {
      state.screen = Screen::Minimal;
      return;
    }
    return;
  }
}

static void drawBodyLine(Frame& out, int y, const std::wstring& s, Style style) {
  if (y < 0 || y >= out.height) return;
  for (int x = 0; x < out.width; ++x) {
    auto& c = out.at(x, y);
    c.ch = L' ';
    c.style = static_cast<std::uint16_t>(Style::Default);
  }
  drawText(out, 0, y, s, style);
}

static std::wstring widenAscii(const std::string& s) {
  std::wstring w;
  w.reserve(s.size());
  for (unsigned char ch : s) w.push_back(static_cast<wchar_t>(ch));
  return w;
}

enum class Align {
  Left,
  Right,
};

static std::string fit(std::string s, std::size_t w, Align a) {
  if (s.size() > w) s.resize(w);
  if (s.size() < w) {
    const std::size_t pad = w - s.size();
    if (a == Align::Right) s = std::string(pad, ' ') + s;
    else s += std::string(pad, ' ');
  }
  return s;
}

static std::string fmt1(double v);

static std::string fmtSampleOrDash(const std::optional<Sample>& s) {
  if (!s) return std::string("--");
  return fmt1(s->value) + s->unit;
}

static std::string fmtSampleOrDashSpaced(const std::optional<Sample>& s) {
  if (!s) return std::string("--");
  return fmt1(s->value) + " " + s->unit;
}

static std::string fmtSampleOrDashSpacedPcie(const std::optional<Sample>& s) {
  if (!s) return std::string("--");
  if (s->label == "pcie-cap") return fmt1(s->value) + " " + s->unit + " (cap)";
  return fmt1(s->value) + " " + s->unit;
}

static std::optional<double> estimatePcieLinkCapMiBps(unsigned int gen, unsigned int width) {
  // Estimate theoretical one-direction bandwidth from link speed.
  // Values are based on commonly cited effective throughput per lane:
  // Gen1: 250 MB/s, Gen2: 500 MB/s, Gen3: 984.615 MB/s, Gen4: 1969.231 MB/s, Gen5: 3938.462 MB/s.
  // Convert decimal MB/s to MiB/s for consistency with other MB/s counters.
  if (gen == 0 || width == 0) return std::nullopt;
  const double mbPerLane = [&]() -> double {
    switch (gen) {
      case 1: return 250.0;
      case 2: return 500.0;
      case 3: return 984.615;
      case 4: return 1969.231;
      case 5: return 3938.462;
      default: return 0.0;
    }
  }();
  if (mbPerLane <= 0.0) return std::nullopt;
  constexpr double mibPerMb = 1.0 / 1.048576;  // MiB = MB / 1.048576
  const double mibPerLane = mbPerLane * mibPerMb;
  return mibPerLane * static_cast<double>(width);
}

static Style metricNameStyle(const Config& cfg) {
  switch (cfg.metricNameColor) {
    case MetricNameColor::White:
      return Style::Default;
    case MetricNameColor::Green:
      return Style::Value;
    case MetricNameColor::Yellow:
      return Style::Hot;
    case MetricNameColor::Cyan:
    default:
      return Style::Section;
  }
}

static std::wstring metricNameColorLabel(const Config& cfg) {
  switch (cfg.metricNameColor) {
    case MetricNameColor::White:
      return L"white";
    case MetricNameColor::Green:
      return L"light green";
    case MetricNameColor::Yellow:
      return L"yellow";
    case MetricNameColor::Cyan:
    default:
      return L"cyan";
  }
}

static std::wstring graphStyleLabel(const Config& cfg) {
  switch (cfg.timelineGraphStyle) {
    case TimelineGraphStyle::Block:
      return L"block";
    case TimelineGraphStyle::Smooth:
      return L"smooth";
    case TimelineGraphStyle::Braille:
    default:
      return L"braille";
  }
}


static std::string fmt0(double v) {
  char buf[64]{};
  std::snprintf(buf, sizeof(buf), "%.0f", v);
  return std::string(buf);
}

static std::string fmt1(double v) {
  char buf[64]{};
  std::snprintf(buf, sizeof(buf), "%.1f", v);
  return std::string(buf);
}

static std::string fmtBytes(std::uint64_t bytes) {
  const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
  if (gib >= 1.0) return fmt1(gib) + "G";
  const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
  if (mib >= 1.0) return fmt0(mib) + "M";
  const double kib = static_cast<double>(bytes) / 1024.0;
  if (kib >= 1.0) return fmt0(kib) + "K";
  return fmt0(static_cast<double>(bytes)) + "B";
}

static void drawSectionTitleLineSplit(
    Frame& out,
    int y,
    const std::string& left,
    const std::string& value,
  const std::string& right,
  Style nameStyle,
  const std::string& peakStr = {}) {
  if (y < 0 || y >= out.height) return;

  // Clear line.
  for (int x = 0; x < out.width; ++x) {
    auto& c = out.at(x, y);
    c.ch = L' ';
    c.style = static_cast<std::uint16_t>(Style::Default);
  }

  const std::string title = left + value;

  // Left side: section label in Section style, numeric value in Default (white).
  {
    const std::wstring wtitle = widenAscii(title);
    std::vector<Style> styles(wtitle.size(), nameStyle);
    for (std::size_t i = left.size(); i < styles.size(); ++i) styles[i] = Style::Default;
    drawTextStyled(out, 0, y, wtitle, styles);
  }

  // Right side: device/context name right-aligned, no parentheses.
  std::string rightFit;
  int rightStart = out.width;
  if (!right.empty() && out.width > 0) {
    const std::size_t leftW = title.size();
    const std::size_t maxRight = (leftW + 1 < static_cast<std::size_t>(out.width))
        ? (static_cast<std::size_t>(out.width) - (leftW + 1))
        : 0u;
    if (maxRight > 0u) {
      rightFit = right;
      if (rightFit.size() > maxRight) {
        // Keep the tail (usually contains the GPU index like "#0").
        rightFit = rightFit.substr(rightFit.size() - maxRight);
      }
      rightStart = std::max(0, out.width - static_cast<int>(rightFit.size()));
      drawText(out, rightStart, y, widenAscii(rightFit), nameStyle);
    }
  }

  // Fill separator dashes in the gap between left and right, with optional peak label.
  const int dashStart = std::min(out.width, static_cast<int>(title.size()) + 1);
  const int dashEnd = std::min(out.width, rightStart);

  if (!peakStr.empty()) {
    // Format: ---PEAK: xxx---
    const std::string peakLabel = "PEAK: " + peakStr;
    const int gapWidth = dashEnd - dashStart;
    const int peakLen = static_cast<int>(peakLabel.size());

    if (gapWidth >= peakLen + 6) {
      // Enough room for ---PEAK: xxx---
      const int leadingDashes = 3;
      const int peakStart = dashStart + leadingDashes;
      const int trailingDashStart = peakStart + peakLen;

      // Leading dashes
      for (int x = dashStart; x < peakStart; ++x) {
        auto& c = out.at(x, y);
        c.ch = L'-';
        c.style = static_cast<std::uint16_t>(nameStyle);
      }

      // Peak label
      const std::wstring wpeakLabel = widenAscii(peakLabel);
      for (std::size_t i = 0; i < wpeakLabel.size() && peakStart + static_cast<int>(i) < dashEnd; ++i) {
        auto& c = out.at(peakStart + static_cast<int>(i), y);
        c.ch = wpeakLabel[i];
        // "PEAK:" in nameStyle, value in Default (white)
        c.style = static_cast<std::uint16_t>(i < 5 ? nameStyle : Style::Default);
      }

      // Trailing dashes
      for (int x = trailingDashStart; x < dashEnd; ++x) {
        auto& c = out.at(x, y);
        c.ch = L'-';
        c.style = static_cast<std::uint16_t>(nameStyle);
      }
    } else {
      // Not enough room, just fill with dashes
      for (int x = dashStart; x < dashEnd; ++x) {
        auto& c = out.at(x, y);
        c.ch = L'-';
        c.style = static_cast<std::uint16_t>(nameStyle);
      }
    }
  } else {
    // No peak, just fill with dashes
    for (int x = dashStart; x < dashEnd; ++x) {
      auto& c = out.at(x, y);
      c.ch = L'-';
      c.style = static_cast<std::uint16_t>(nameStyle);
    }
  }
}

static void drawSectionTitleLine(Frame& out, int y, const std::string& title) {
  if (y < 0 || y >= out.height) return;

  // Clear line.
  for (int x = 0; x < out.width; ++x) {
    auto& c = out.at(x, y);
    c.ch = L' ';
    c.style = static_cast<std::uint16_t>(Style::Default);
  }

  // Title up to width.
  drawText(out, 0, y, widenAscii(title), Style::Section);

  // Fill remainder with '-' separator.
  const int start = std::min(out.width, static_cast<int>(title.size()) + 1);
  for (int x = start; x < out.width; ++x) {
    auto& c = out.at(x, y);
    c.ch = L'-';
    c.style = static_cast<std::uint16_t>(Style::Section);
  }
}

static void drawCenteredSectionLine(Frame& out, int y, const std::string& title, Style st) {
  if (y < 0 || y >= out.height) return;

  // Clear line.
  for (int x = 0; x < out.width; ++x) {
    auto& c = out.at(x, y);
    c.ch = L' ';
    c.style = static_cast<std::uint16_t>(Style::Default);
  }

  // Fill line with '-' in requested style.
  for (int x = 0; x < out.width; ++x) {
    auto& c = out.at(x, y);
    c.ch = L'-';
    c.style = static_cast<std::uint16_t>(st);
  }

  if (title.empty() || out.width <= 0) return;

  const int textW = static_cast<int>(title.size());
  const int start = std::max(0, (out.width - textW) / 2);
  drawText(out, start, y, widenAscii(title), st);
}

static void drawScrollingBars(
    Frame& out,
    int topY,
    int leftX,
    int height,
    int width,
    const Timeline& tl,
    double maxV,
    int sampleWindow,
    TimelineAgg agg,
    TimelineGraphStyle graphStyle = TimelineGraphStyle::Block) {
  if (height <= 0 || width <= 0) return;

  const auto vals = tl.values();
  const int colsToDraw = width;
  const int available = static_cast<int>(vals.size());

  // Clear region.
  for (int r = 0; r < height; ++r) {
    const int y = topY + r;
    if (y < 0 || y >= out.height) continue;
    for (int x = 0; x < colsToDraw; ++x) {
      const int xx = leftX + x;
      if (xx < 0 || xx >= out.width) continue;
      auto& c = out.at(xx, y);
      c.ch = L' ';
      c.style = static_cast<std::uint16_t>(Style::Default);
    }
  }

  if (available <= 0 || maxV <= 0.0) return;

  // Helper to compute value for a column, handling bucketing and aggregation.
  auto computeColumnValue = [&](int col, int cols, int totalSamples) -> double {
    const int missing = totalSamples - available;
    const bool needsBucketing = totalSamples > cols;

    if (!needsBucketing) {
      // 1:1 mapping for the most recent samples, right-aligned.
      const int count = std::min(cols, available);
      const int xOffset = cols - count;
      const int idx = col - xOffset;
      if (idx < 0 || idx >= available) return 0.0;
      const int sampleIdx = std::max(0, available - count) + idx;
      if (sampleIdx < 0 || sampleIdx >= available) return 0.0;
      double v = vals[static_cast<std::size_t>(sampleIdx)];
      if (!std::isfinite(v)) v = 0.0;
      return std::clamp(v, 0.0, maxV);
    }

    const int start = static_cast<int>((static_cast<long long>(col) * totalSamples) / cols);
    const int end = static_cast<int>((static_cast<long long>(col + 1) * totalSamples) / cols);
    if (start >= end) return 0.0;

    int dataStart = start - missing;
    int dataEnd = end - missing;
    if (dataEnd <= 0 || dataStart >= available) return 0.0;
    dataStart = std::max(0, dataStart);
    dataEnd = std::min(available, dataEnd);
    if (dataStart >= dataEnd) return 0.0;

    double v = 0.0;
    if (agg == TimelineAgg::Avg) {
      double sum = 0.0;
      int n = 0;
      for (int j = dataStart; j < dataEnd; ++j) {
        double s = vals[static_cast<std::size_t>(j)];
        if (!std::isfinite(s)) continue;
        sum += s;
        ++n;
      }
      v = (n > 0) ? (sum / static_cast<double>(n)) : 0.0;
    } else {
      double maxBucket = -std::numeric_limits<double>::infinity();
      for (int j = dataStart; j < dataEnd; ++j) {
        double s = vals[static_cast<std::size_t>(j)];
        if (!std::isfinite(s)) continue;
        maxBucket = std::max(maxBucket, s);
      }
      if (!std::isfinite(maxBucket)) maxBucket = 0.0;
      v = maxBucket;
    }
    if (!std::isfinite(v)) v = 0.0;
    return std::clamp(v, 0.0, maxV);
  };

  const int cols = colsToDraw;
  const int totalSamples = std::max(available, std::max(sampleWindow, width));

  if (graphStyle == TimelineGraphStyle::Braille) {
    // Braille characters give 2x4 resolution per cell (2 columns × 4 rows of dots).
    // Each cell represents 2 horizontal samples and 4 vertical dots.
    // Unicode braille: 0x2800 + dot pattern bits.
    // Dot numbering: 
    //   1 4
    //   2 5
    //   3 6
    //   7 8
    // Bits: 1=0x01, 2=0x02, 3=0x04, 4=0x08, 5=0x10, 6=0x20, 7=0x40, 8=0x80

    // We use 1 sample per column (not 2) for simplicity and better time resolution.
    // Each cell height represents 4 vertical dots.
    const int dotsPerCellV = 4;
    const int totalDotsV = height * dotsPerCellV;

    for (int i = 0; i < cols; ++i) {
      const int x = leftX + i;
      if (x < 0 || x >= out.width) continue;

      double v = computeColumnValue(i, cols, totalSamples);
      const int dotHeight = static_cast<int>(std::round((v / maxV) * static_cast<double>(totalDotsV)));
      const int clampedDotH = std::clamp(dotHeight, 0, totalDotsV);

      // Fill from bottom up.
      for (int cellY = 0; cellY < height; ++cellY) {
        const int y = topY + (height - 1 - cellY);
        if (y < 0 || y >= out.height) continue;

        // Calculate which dots in this cell should be lit.
        const int cellDotStart = cellY * dotsPerCellV;  // bottom dot index of this cell
        const int cellDotEnd = cellDotStart + dotsPerCellV;  // exclusive

        wchar_t pattern = 0x2800;  // empty braille
        // Dot bits: row 0 (bottom) = bits 7,8; row 1 = bits 3,6; row 2 = bits 2,5; row 3 (top) = bits 1,4
        // But we're using single-column, so only left dots: 1,2,3,7 (bits 0x01, 0x02, 0x04, 0x40)
        const int dotBits[4] = {0x40, 0x04, 0x02, 0x01};  // from bottom to top

        for (int d = 0; d < dotsPerCellV; ++d) {
          const int dotIdx = cellDotStart + d;
          if (dotIdx < clampedDotH) {
            pattern |= static_cast<wchar_t>(dotBits[d]);
          }
        }

        if (pattern != 0x2800) {
          auto& c = out.at(x, y);
          c.ch = pattern;
          c.style = static_cast<std::uint16_t>(Style::Value);  // Use value color for graph
        }
      }
    }
    return;
  }

  if (graphStyle == TimelineGraphStyle::Smooth) {
    // Use half-block characters for 2x vertical resolution.
    // ▀ (0x2580) = upper half, ▄ (0x2584) = lower half, █ (0x2588) = full block
    const int halfRows = height * 2;  // 2 half-rows per cell

    for (int i = 0; i < cols; ++i) {
      const int x = leftX + i;
      if (x < 0 || x >= out.width) continue;

      double v = computeColumnValue(i, cols, totalSamples);
      const int halfH = static_cast<int>(std::round((v / maxV) * static_cast<double>(halfRows)));
      const int clampedHalfH = std::clamp(halfH, 0, halfRows);

      // Fill from bottom up in half-cell increments.
      int remainingHalves = clampedHalfH;
      for (int cellY = 0; cellY < height && remainingHalves > 0; ++cellY) {
        const int y = topY + (height - 1 - cellY);
        if (y < 0 || y >= out.height) continue;

        auto& c = out.at(x, y);
        if (remainingHalves >= 2) {
          c.ch = 0x2588;  // full block █
          remainingHalves -= 2;
        } else {
          c.ch = 0x2584;  // lower half ▄
          remainingHalves -= 1;
        }
        c.style = static_cast<std::uint16_t>(Style::Default);
      }
    }
    return;
  }

  // Default: Block style (original implementation)
  for (int i = 0; i < cols; ++i) {
    const int x = leftX + i;
    if (x < 0 || x >= out.width) continue;

    double v = computeColumnValue(i, cols, totalSamples);
    const int barH = static_cast<int>(std::round((v / maxV) * static_cast<double>(height)));
    const int clampedH = std::clamp(barH, 0, height);

    for (int r = 0; r < clampedH; ++r) {
      const int y = topY + (height - 1 - r);
      if (y < 0 || y >= out.height) continue;
      auto& c = out.at(x, y);
      c.ch = 0x2593;  // dark shade block
      c.style = static_cast<std::uint16_t>(Style::Default);
    }
  }
}

static void drawHorizontalBar(Frame& out, int y, int leftX, int width, double value, double maxV) {
  if (width <= 0) return;
  if (y < 0 || y >= out.height) return;

  // Clear line region.
  for (int x = 0; x < width; ++x) {
    const int xx = leftX + x;
    if (xx < 0 || xx >= out.width) continue;
    auto& c = out.at(xx, y);
    c.ch = L' ';
    c.style = static_cast<std::uint16_t>(Style::Default);
  }

  if (!std::isfinite(value) || maxV <= 0.0) return;
  double v = std::clamp(value, 0.0, maxV);
  const int filled = std::clamp(static_cast<int>(std::round((v / maxV) * static_cast<double>(width))), 0, width);

  for (int x = 0; x < filled; ++x) {
    const int xx = leftX + x;
    if (xx < 0 || xx >= out.width) continue;
    auto& c = out.at(xx, y);
    c.ch = 0x2588;  // full block
    c.style = static_cast<std::uint16_t>(Style::Default);
  }
}

static void drawHorizontalBarNoClear(Frame& out, int y, int leftX, int width, double value, double maxV) {
  if (width <= 0) return;
  if (y < 0 || y >= out.height) return;
  if (!std::isfinite(value) || maxV <= 0.0) return;

  double v = std::clamp(value, 0.0, maxV);
  const int filled = std::clamp(static_cast<int>(std::round((v / maxV) * static_cast<double>(width))), 0, width);

  for (int x = 0; x < filled; ++x) {
    const int xx = leftX + x;
    if (xx < 0 || xx >= out.width) continue;
    auto& c = out.at(xx, y);
    c.ch = 0x2588;  // full block
    c.style = static_cast<std::uint16_t>(Style::Default);
  }
}

static void drawBarLineWithDots(
  Frame& out,
  int y,
  const std::string& left,
  const std::string& value,
  const std::string& right,
  int barStart,
  double valueNum,
  double maxV,
  Style nameStyle) {
  if (y < 0 || y >= out.height) return;

  // Clear line.
  for (int x = 0; x < out.width; ++x) {
    auto& c = out.at(x, y);
    c.ch = L' ';
    c.style = static_cast<std::uint16_t>(Style::Default);
  }

  const std::string title = left + value;

  // Left side: label in nameStyle, value in Default.
  {
    const std::wstring wtitle = widenAscii(title);
    std::vector<Style> styles(wtitle.size(), nameStyle);
    for (std::size_t i = left.size(); i < styles.size(); ++i) styles[i] = Style::Default;
    drawTextStyled(out, 0, y, wtitle, styles);
  }

  // Right side: optional device name.
  std::string rightFit;
  int rightStart = out.width;
  if (!right.empty() && out.width > 0) {
    const std::size_t leftW = title.size();
    const std::size_t maxRight = (leftW + 1 < static_cast<std::size_t>(out.width))
        ? (static_cast<std::size_t>(out.width) - (leftW + 1))
        : 0u;
    if (maxRight > 0u) {
      rightFit = right;
      if (rightFit.size() > maxRight) {
        rightFit = rightFit.substr(rightFit.size() - maxRight);
      }
      rightStart = std::max(0, out.width - static_cast<int>(rightFit.size()));
      drawText(out, rightStart, y, widenAscii(rightFit), nameStyle);
    }
  }

  const int minStart = std::min(out.width, static_cast<int>(title.size()) + 1);
  const int barStartAligned = std::max(minStart, std::min(out.width, barStart));
  const int barEnd = std::min(out.width, rightStart);

  // Ensure spacing up to aligned bar start.
  for (int x = minStart; x < barStartAligned; ++x) {
    auto& c = out.at(x, y);
    c.ch = L' ';
    c.style = static_cast<std::uint16_t>(Style::Default);
  }

  // Dotted guide.
  for (int x = barStartAligned; x < barEnd; ++x) {
    auto& c = out.at(x, y);
    c.ch = L'.';
    c.style = static_cast<std::uint16_t>(Style::Default);
  }

  if (barEnd > barStartAligned) {
    drawHorizontalBarNoClear(out, y, barStartAligned, barEnd - barStartAligned, valueNum, maxV);
  }
}

static void renderTimelines(Frame& out, int /*bodyTop*/, const TuiState& state, const Config& cfg) {
  const bool barView = (state.timelineView == TimelineView::Bars);

  const bool showCpu = barView ? cfg.showCpuBars : cfg.showCpu;
  const bool showCpuHot = barView ? cfg.showCpuHotBars : cfg.showCpuHot;
  const bool showRam = barView ? cfg.showRamBars : cfg.showRam;
  const bool showGpu = barView ? cfg.showGpuBars : cfg.showGpu;
  const bool showGpuMem = barView ? cfg.showGpuMemBars : cfg.showGpuMem;
  const bool showVram = barView ? cfg.showVramBars : cfg.showVram;
  const bool showGpuClock = barView ? cfg.showGpuClockBars : cfg.showGpuClock;
  const bool showGpuMemClock = barView ? cfg.showGpuMemClockBars : cfg.showGpuMemClock;
  const bool showGpuEnc = barView ? cfg.showGpuEncBars : cfg.showGpuEnc;
  const bool showGpuDec = barView ? cfg.showGpuDecBars : cfg.showGpuDec;
  const bool showPcieRx = barView ? cfg.showPcieRxBars : cfg.showPcieRx;
  const bool showPcieTx = barView ? cfg.showPcieTxBars : cfg.showPcieTx;
  const bool showDiskRead = barView ? cfg.showDiskReadBars : cfg.showDiskRead;
  const bool showDiskWrite = barView ? cfg.showDiskWriteBars : cfg.showDiskWrite;
  const bool showNetRx = barView ? cfg.showNetRxBars : cfg.showNetRx;
  const bool showNetTx = barView ? cfg.showNetTxBars : cfg.showNetTx;

  auto renderStatsHeader = [&](Frame& f) -> int {
    // Multi-line header matches legacy ncurses layout:
    // row 0: CPU/RAM/DISK/NET
    // row 1..N: one row per GPU
    const int headerRows = 1 + static_cast<int>(state.latest.gpus.size());

    // Row 0
    {
      drawBodyLine(f, 0, L"", Style::Default);

      int x = 0;
      auto addBadge = [&](const std::string& label) {
        if (x >= f.width) return;
        drawText(f, x, 0, widenAscii(label), Style::FooterBlock);
        x += static_cast<int>(label.size());
        if (x < f.width) {
          drawText(f, x, 0, L" ", Style::Default);
          ++x;
        }
      };
      auto addPunct = [&](const std::wstring& s, Style st) {
        if (x >= f.width) return;
        drawText(f, x, 0, s, st);
        x += static_cast<int>(s.size());
        if (x < f.width) {
          drawText(f, x, 0, L" ", Style::Default);
          ++x;
        }
      };
      auto addLabel = [&](const std::string& label) {
        drawText(f, x, 0, widenAscii(label), Style::FooterBlock);
        x += static_cast<int>(label.size());
      };
      auto addValueField = [&](std::string value, std::size_t w, Align a) {
        (void)a;
        if (value.size() > w) value.resize(w);
        drawText(f, x, 0, widenAscii(value), Style::Default);
        x += static_cast<int>(value.size());
        if (x + 1 < f.width) {
          drawText(f, x, 0, L"  ", Style::Default);
          x += 2;
        }
      };

      const std::string cpuStr = state.latest.cpu ? (fmt0(state.latest.cpu->value) + "%") : std::string("--");
      addLabel("CPU0:");
      addValueField(cpuStr, 4, Align::Right);

      addLabel("RAM:");
      addValueField(!state.latest.ramText.empty() ? state.latest.ramText : std::string("--"), 18, Align::Left);

        const std::string diskStr = "R: " + fmtSampleOrDashSpaced(state.latest.diskRead)
          + " W: " + fmtSampleOrDashSpaced(state.latest.diskWrite);
        addLabel("DISK:");
        addValueField(diskStr, 32, Align::Left);

        const std::string netStr = "RX: " + fmtSampleOrDashSpaced(state.latest.netRx)
          + " TX: " + fmtSampleOrDashSpaced(state.latest.netTx);
        addLabel("NET:");
        addValueField(netStr, 32, Align::Left);
    }

    // GPU rows
    for (int i = 0; i < static_cast<int>(state.latest.gpus.size()); ++i) {
      const int row = 1 + i;
      if (row >= f.height) break;
      drawBodyLine(f, row, L"", Style::Default);

      int x = 0;
      auto addBadge = [&](const std::string& label) {
        if (x >= f.width) return;
        drawText(f, x, row, widenAscii(label), Style::FooterBlock);
        x += static_cast<int>(label.size());
        if (x < f.width) {
          drawText(f, x, row, L" ", Style::Default);
          ++x;
        }
      };
      auto addPunct = [&](const std::wstring& s, Style st) {
        if (x >= f.width) return;
        drawText(f, x, row, s, st);
        x += static_cast<int>(s.size());
        if (x < f.width) {
          drawText(f, x, row, L" ", Style::Default);
          ++x;
        }
      };
      auto addLabel = [&](const std::string& label) {
        drawText(f, x, row, widenAscii(label), Style::FooterBlock);
        x += static_cast<int>(label.size());
      };
      auto addValueField = [&](std::string value, std::size_t w, Align a) {
        (void)a;
        if (value.size() > w) value.resize(w);
        drawText(f, x, row, widenAscii(value), Style::Default);
        x += static_cast<int>(value.size());
        if (x + 1 < f.width) {
          drawText(f, x, row, L"  ", Style::Default);
          x += 2;
        }
      };

      const auto& gt = state.latest.gpus[static_cast<std::size_t>(i)];
      // Header should be compact: GPU label is just "GPU <n>" (device name is shown elsewhere).
      const std::string gpuId = "GPU" + std::to_string(i);
      addLabel(gpuId + ":");

      addValueField(gt.utilPct ? (fmt0(*gt.utilPct) + "%") : std::string("--"), 4, Align::Right);

      std::string vramStr = "--";
      if (gt.vramUsedGiB && gt.vramTotalGiB) {
        const double used = *gt.vramUsedGiB;
        const double total = *gt.vramTotalGiB;
        const double pct = total > 0.0 ? (100.0 * used / total) : 0.0;
        vramStr = fmt1(used) + "/" + fmt1(total) + "G(" + fmt0(pct) + "%)";
      }
      addLabel("VRAM:");
      addValueField(vramStr, 18, Align::Left);

      addLabel("W:");
      addValueField(gt.watts ? (fmt0(*gt.watts) + "W") : std::string("--"), 5, Align::Right);

      addLabel("T:");
      addValueField(gt.tempC ? (fmt0(*gt.tempC) + "C") : std::string("--"), 4, Align::Right);

      addLabel("POWER:");
      addValueField(!gt.pstate.empty() ? gt.pstate : std::string("--"), 4, Align::Left);

      std::string linkStr = "--";
      if (gt.pcieLinkWidth && gt.pcieLinkGen) {
        linkStr = "GEN " + std::to_string(*gt.pcieLinkGen) + "@" + std::to_string(*gt.pcieLinkWidth) + "x";
      } else if (!gt.pcieLinkNote.empty()) {
        linkStr = "-- (" + gt.pcieLinkNote + ")";
      }

      std::optional<Sample> pcieRx = state.latest.pcieRx;
      std::optional<Sample> pcieTx = state.latest.pcieTx;
      // If OS/NVML throughput counters are unavailable, still show an estimated link bandwidth cap.
      if ((!pcieRx || !pcieTx) && gt.pcieLinkWidth && gt.pcieLinkGen) {
        if (const auto cap = estimatePcieLinkCapMiBps(*gt.pcieLinkGen, *gt.pcieLinkWidth)) {
          const Sample capS{*cap, "MB/s", "pcie-cap"};
          if (!pcieRx) pcieRx = capS;
          if (!pcieTx) pcieTx = capS;
        }
      }
      const std::string pcieStr = linkStr
          + " RX: " + fmtSampleOrDashSpacedPcie(pcieRx)
          + " TX: " + fmtSampleOrDashSpacedPcie(pcieTx);
      addLabel("PCIE:");
      addValueField(pcieStr, 40, Align::Left);
    }

    return headerRows;
  };

  const int headerRows = renderStatsHeader(out);

  const int top = headerRows;
  const int bottomReserved = 1;
  const int usable = std::max(0, (out.height - bottomReserved) - top);

  struct Panel {
    std::string name;
    bool enabled;
    // Title line uses these.
    const std::optional<Sample>* sample;
    // Graph rendering uses these (either a single TL, or two TLs for PCIe).
    const Timeline* tl;
    const Timeline* tl2;
    double maxV;
    bool twoLine = false;
    std::string device;
    std::string unit;  // Unit for peak value display
  };

  auto gpuContext = [&](std::size_t index) -> std::string {
    if (index >= state.gpuDeviceNames.size()) return "#" + std::to_string(index);
    const std::string& n = state.gpuDeviceNames[index];
    if (n.empty() || n == ("GPU" + std::to_string(index))) return "#" + std::to_string(index);
    return n + " #" + std::to_string(index);
  };

  auto gpuPrefix = [&](std::size_t index) -> std::string {
    return "GPU" + std::to_string(index);
  };

  // Build per-GPU usage samples for title lines (USAGE: xx %).
  std::vector<std::optional<Sample>> gpuUsageSamples;
  gpuUsageSamples.reserve(state.latest.gpus.size());
  for (const auto& gt : state.latest.gpus) {
    if (gt.utilPct) gpuUsageSamples.push_back(Sample{*gt.utilPct, "%", {}});
    else gpuUsageSamples.push_back(std::nullopt);
  }

  std::vector<Panel> panels;
  panels.push_back(Panel{"CPU", showCpu, &state.latest.cpu, &state.cpuTl, nullptr, 100.0, false, state.cpuDevice, "%"});
  panels.push_back(Panel{"Hottest Core", showCpuHot, &state.latest.cpuMax, &state.cpuMaxTl, nullptr, 100.0, false, state.cpuDevice, "%"});

  // CPU/RAM first.
  panels.push_back(Panel{"RAM", showRam, &state.latest.ramPct, &state.ramTl, nullptr, 100.0, false, state.ramDevice, "%"});

  // GPU usage before VRAM and MemCtrl.
  if (showGpu) {
    const std::size_t n = std::min(state.gpuTls.size(), gpuUsageSamples.size());
    for (std::size_t i = 0; i < n; ++i) {
      panels.push_back(Panel{gpuPrefix(i) + " USAGE", true, &gpuUsageSamples[i], &state.gpuTls[i], nullptr, 100.0, false, gpuContext(i), "%"});
    }
  }

  panels.push_back(Panel{gpuPrefix(0) + " VRAM", showVram, &state.latest.vramPct, &state.vramTl, nullptr, 100.0, false, gpuContext(0), "%"});
  panels.push_back(Panel{gpuPrefix(0) + " MemCtrl", showGpuMem, &state.latest.gpuMemUtil, &state.gpuMemUtilTl, nullptr, 100.0, false, gpuContext(0), "%"});

  panels.push_back(Panel{gpuPrefix(0) + " MHz", showGpuClock, &state.latest.gpuClock, &state.gpuClockTl, nullptr, 3000.0, false, gpuContext(0), "MHz"});
  panels.push_back(Panel{gpuPrefix(0) + " Mem MHz", showGpuMemClock, &state.latest.gpuMemClock, &state.gpuMemClockTl, nullptr, 14000.0, false, gpuContext(0), "MHz"});
  panels.push_back(Panel{gpuPrefix(0) + " Enc", showGpuEnc, &state.latest.gpuEnc, &state.gpuEncTl, nullptr, 100.0, false, gpuContext(0), "%"});
  panels.push_back(Panel{gpuPrefix(0) + " Dec", showGpuDec, &state.latest.gpuDec, &state.gpuDecTl, nullptr, 100.0, false, gpuContext(0), "%"});

  // PCIe split RX/TX (tweak restored).
  double pcieMax = 32'000.0;
  if (!state.latest.gpus.empty()) {
    const auto& gt0 = state.latest.gpus[0];
    if (gt0.pcieLinkGen && gt0.pcieLinkWidth) {
      if (const auto cap = estimatePcieLinkCapMiBps(*gt0.pcieLinkGen, *gt0.pcieLinkWidth)) {
        pcieMax = *cap;
      }
    }
  }
  panels.push_back(Panel{gpuPrefix(0) + " PCIe RX", showPcieRx, &state.latest.pcieRx, &state.pcieRxTl, nullptr, pcieMax, false, gpuContext(0), "MB/s"});
  panels.push_back(Panel{gpuPrefix(0) + " PCIe TX", showPcieTx, &state.latest.pcieTx, &state.pcieTxTl, nullptr, pcieMax, false, gpuContext(0), "MB/s"});

  // Disk/network last.
  panels.push_back(Panel{"Disk Read", showDiskRead, &state.latest.diskRead, &state.diskReadTl, nullptr, 5000.0, false, state.diskDevice, "MB/s"});
  panels.push_back(Panel{"Disk Write", showDiskWrite, &state.latest.diskWrite, &state.diskWriteTl, nullptr, 5000.0, false, state.diskDevice, "MB/s"});

  panels.push_back(Panel{"Net RX", showNetRx, &state.latest.netRx, &state.netRxTl, nullptr, 5000.0, false, state.netDevice, "MB/s"});
  panels.push_back(Panel{"Net TX", showNetTx, &state.latest.netTx, &state.netTxTl, nullptr, 5000.0, false, state.netDevice, "MB/s"});

  panels.erase(
      std::remove_if(panels.begin(), panels.end(), [](const Panel& p) { return !p.enabled; }),
      panels.end());

  if (panels.empty()) {
    drawBodyLine(out, 2, std::wstring(i18n::tr(i18n::MsgId::TimelinesNoneEnabled)), Style::Value);
    return;
  }

  const int labelRows = 1;
  const int n = static_cast<int>(panels.size());
  const int minGraphRows = barView ? 0 : 3;
  const int perPanel = std::max(labelRows + minGraphRows, usable / std::max(1, n));

  int y = top;
  int barStartCol = 0;
  if (barView) {
    std::size_t maxTitle = 0;
    for (const auto& p : panels) {
      std::string section = p.name;
      std::string value;
      if (p.sample && p.sample->has_value()) {
        section += ": ";
        value = fmt1((*p.sample)->value) + " " + (*p.sample)->unit;
      } else if (p.sample != nullptr) {
        section += ": ";
        value = "unavailable";
      }
      maxTitle = std::max(maxTitle, section.size() + value.size());
    }
    barStartCol = static_cast<int>(maxTitle) + 1;
  }

  std::string lastDeviceShown;

  // Compute peak samples based on config: peakWindowSec * 1000 / refreshMs
  const std::size_t peakSamples = (cfg.refreshMs > 0)
      ? static_cast<std::size_t>((cfg.peakWindowSec * 1000u) / cfg.refreshMs)
      : 60u;

  for (int i = 0; i < n; ++i) {
    const auto& p = panels[static_cast<std::size_t>(i)];

    if (barView && !p.device.empty() && p.device != lastDeviceShown) {
      if (y >= out.height - bottomReserved) break;
      drawCenteredSectionLine(out, y, p.device, Style::Hot);
      lastDeviceShown = p.device;
      ++y;
      if (y >= out.height - bottomReserved) break;
    }

    const int remainingPanels = n - i;
    const int remainingUsable = std::max(0, (out.height - bottomReserved) - y);
    const int height = barView ? 1 : ((remainingPanels == 1) ? std::max(0, remainingUsable) : std::min(perPanel, remainingUsable));
    if (height <= 0) break;

    const std::string right;

    // Compute peak value string if enabled and timeline available
    std::string peakStr;
    if (cfg.showPeakValues && p.tl && p.tl->size() > 0) {
      const double peakVal = p.tl->maxLast(peakSamples);
      peakStr = fmt1(peakVal) + " " + p.unit;
    }

    std::string section = p.name;
    const int titleY = barView ? y : (y + height - 1);

    if (!barView) {
      const int graphTop = y;
      const int graphH = std::max(0, height - 1);
      if (graphH > 0) {
        if (p.tl) {
          const int sampleWindow = std::max(static_cast<int>(cfg.timelineSamples), out.width);
          drawScrollingBars(out, graphTop, 0, graphH, out.width, *p.tl, p.maxV, sampleWindow, cfg.timelineAgg, cfg.timelineGraphStyle);
        } else {
          // Clear graph region even if TL is unavailable.
          for (int r = 0; r < graphH; ++r) {
            const int yy = graphTop + r;
            if (yy < 0 || yy >= out.height) continue;
            for (int x = 0; x < out.width; ++x) {
              auto& c = out.at(x, yy);
              c.ch = L' ';
              c.style = static_cast<std::uint16_t>(Style::Default);
            }
          }
        }
      }
    }

    if (p.sample && p.sample->has_value()) {
      section += ": ";
      const std::string value = fmt1((*p.sample)->value) + " " + (*p.sample)->unit;
      if (barView) {
        drawBarLineWithDots(out, titleY, section, value, right, barStartCol, (*p.sample)->value, p.maxV,
                            metricNameStyle(cfg));
      } else {
        drawSectionTitleLineSplit(out, titleY, section, value, right, metricNameStyle(cfg), peakStr);
      }
    } else if (p.sample != nullptr) {
      section += ": ";
      if (barView) {
        drawBarLineWithDots(out, titleY, section, "unavailable", right, barStartCol,
                            std::numeric_limits<double>::quiet_NaN(), p.maxV, metricNameStyle(cfg));
      } else {
        drawSectionTitleLineSplit(out, titleY, section, "unavailable", right, metricNameStyle(cfg), peakStr);
      }
    } else {
      if (barView) {
        drawBarLineWithDots(out, titleY, section, std::string{}, right, barStartCol,
                            std::numeric_limits<double>::quiet_NaN(), p.maxV, metricNameStyle(cfg));
      } else {
        drawSectionTitleLineSplit(out, titleY, section, std::string{}, right, metricNameStyle(cfg), peakStr);
      }
    }

    y += height;
  }
}

static void renderMinimal(Frame& out, int /*bodyTop*/, const TuiState& state) {
  // Reuse the Timelines top header (stats rows), but do not draw any scrolling bars.
  // This view is intended to be lightweight / text-only.
  const int headerRows = 3 + static_cast<int>(state.latest.gpus.size());

  // Render the same stats header as Timelines by calling into renderTimelines' logic.
  // We keep it simple by delegating through a minimal local copy of the header code.
  // (Avoids exposing new shared helpers in headers.)
  {
    // Row 0
    drawBodyLine(out, 0, L"", Style::Default);

    int x = 0;
    auto addLabelTight = [&](const std::string& label) {
      drawText(out, x, 0, widenAscii(label), Style::FooterBlock);
      x += static_cast<int>(label.size());
    };
    auto addLabelSpaced = [&](const std::string& label) {
      drawText(out, x, 0, widenAscii(label), Style::FooterBlock);
      x += static_cast<int>(label.size());
      if (x < out.width) {
        drawText(out, x, 0, L" ", Style::Default);
        ++x;
      }
    };
    auto addValueField = [&](std::string value, std::size_t w, Align a) {
      (void)a;
      if (value.size() > w) value.resize(w);
      drawText(out, x, 0, widenAscii(value), Style::Default);
      x += static_cast<int>(value.size());
      if (x + 1 < out.width) {
        drawText(out, x, 0, L"  ", Style::Default);
        x += 2;
      }
    };

    const std::string cpuStr = state.latest.cpu ? (fmt0(state.latest.cpu->value) + "%") : std::string("--");
    addLabelTight("CPU0:");
    addValueField(cpuStr, 4, Align::Right);

    addLabelTight("RAM:");
    addValueField(!state.latest.ramText.empty() ? state.latest.ramText : std::string("--"), 18, Align::Left);
  }

  for (int i = 0; i < static_cast<int>(state.latest.gpus.size()); ++i) {
    const int row = 1 + i;
    if (row >= out.height) break;
    drawBodyLine(out, row, L"", Style::Default);

    int x = 0;
    auto addLabelTight = [&](const std::string& label) {
      drawText(out, x, row, widenAscii(label), Style::FooterBlock);
      x += static_cast<int>(label.size());
    };
    auto addLabelSpaced = [&](const std::string& label) {
      drawText(out, x, row, widenAscii(label), Style::FooterBlock);
      x += static_cast<int>(label.size());
      if (x < out.width) {
        drawText(out, x, row, L" ", Style::Default);
        ++x;
      }
    };
    auto addValueField = [&](std::string value, std::size_t w, Align a) {
      (void)a;
      if (value.size() > w) value.resize(w);
      drawText(out, x, row, widenAscii(value), Style::Default);
      x += static_cast<int>(value.size());
      if (x + 1 < out.width) {
        drawText(out, x, row, L"  ", Style::Default);
        x += 2;
      }
    };

    const auto& gt = state.latest.gpus[static_cast<std::size_t>(i)];
    const std::string gpuPrefix = "GPU" + std::to_string(i) + ":";
    addLabelTight(gpuPrefix);

    addValueField(gt.utilPct ? (fmt0(*gt.utilPct) + "%") : std::string("--"), 4, Align::Right);

    std::string vramStr = "--";
    if (gt.vramUsedGiB && gt.vramTotalGiB) {
      const double used = *gt.vramUsedGiB;
      const double total = *gt.vramTotalGiB;
      const double pct = total > 0.0 ? (100.0 * used / total) : 0.0;
      vramStr = fmt1(used) + "/" + fmt1(total) + "G(" + fmt0(pct) + "%)";
    }
    addLabelTight("VRAM:");
    addValueField(vramStr, 18, Align::Left);

    addLabelTight("MEMCTRL:");
    addValueField(gt.memUtilPct ? (fmt0(*gt.memUtilPct) + "%") : std::string("--"), 4, Align::Right);

    addLabelTight("GPU MHz:");
    addValueField(gt.gpuClockMHz ? std::to_string(*gt.gpuClockMHz) : std::string("--"), 6, Align::Right);

    addLabelTight("MEM MHz:");
    addValueField(gt.memClockMHz ? std::to_string(*gt.memClockMHz) : std::string("--"), 6, Align::Right);

    addLabelTight("ENC:");
    addValueField(gt.encoderUtilPct ? (fmt0(*gt.encoderUtilPct) + "%") : std::string("--"), 4, Align::Right);

    addLabelTight("DEC:");
    addValueField(gt.decoderUtilPct ? (fmt0(*gt.decoderUtilPct) + "%") : std::string("--"), 4, Align::Right);

    addLabelTight("WATTS:");
    addValueField(gt.watts ? (fmt0(*gt.watts) + "W") : std::string("--"), 5, Align::Right);

    addLabelTight("TEMP:");
    addValueField(gt.tempC ? (fmt0(*gt.tempC) + "C") : std::string("--"), 4, Align::Right);

    addLabelTight("POWER:");
    addValueField(!gt.pstate.empty() ? gt.pstate : std::string("--"), 4, Align::Left);

    std::string linkStr = "--";
    if (gt.pcieLinkWidth && gt.pcieLinkGen) {
      linkStr = "GEN " + std::to_string(*gt.pcieLinkGen) + "@" + std::to_string(*gt.pcieLinkWidth) + "x";
    } else if (!gt.pcieLinkNote.empty()) {
      linkStr = "-- (" + gt.pcieLinkNote + ")";
    }
    const std::string pcieStr = linkStr
        + " RX: " + fmtSampleOrDashSpaced(state.latest.pcieRx)
        + " TX: " + fmtSampleOrDashSpaced(state.latest.pcieTx);
    addLabelTight("PCIE:");
    addValueField(pcieStr, 40, Align::Left);
  }

  {
    const int row = 1 + static_cast<int>(state.latest.gpus.size());
    if (row < out.height) {
      drawBodyLine(out, row, L"", Style::Default);

      int x = 0;
      auto addLabelTight = [&](const std::string& label) {
        drawText(out, x, row, widenAscii(label), Style::FooterBlock);
        x += static_cast<int>(label.size());
      };
      auto addLabelSpaced = [&](const std::string& label) {
        drawText(out, x, row, widenAscii(label), Style::FooterBlock);
        x += static_cast<int>(label.size());
        if (x < out.width) {
          drawText(out, x, row, L" ", Style::Default);
          ++x;
        }
      };
      auto addValueField = [&](std::string value, std::size_t w, Align a) {
        (void)a;
        if (value.size() > w) value.resize(w);
        drawText(out, x, row, widenAscii(value), Style::Default);
        x += static_cast<int>(value.size());
        if (x + 1 < out.width) {
          drawText(out, x, row, L"  ", Style::Default);
          x += 2;
        }
      };

      addLabelSpaced("Disk 0");
      addLabelTight("R:");
      addValueField(fmtSampleOrDashSpaced(state.latest.diskRead), 12, Align::Left);
      addLabelTight("W:");
      addValueField(fmtSampleOrDashSpaced(state.latest.diskWrite), 12, Align::Left);
    }
  }

  {
    const int row = 2 + static_cast<int>(state.latest.gpus.size());
    if (row < out.height) {
      drawBodyLine(out, row, L"", Style::Default);

      int x = 0;
      auto addLabelTight = [&](const std::string& label) {
        drawText(out, x, row, widenAscii(label), Style::FooterBlock);
        x += static_cast<int>(label.size());
      };
      auto addLabelSpaced = [&](const std::string& label) {
        drawText(out, x, row, widenAscii(label), Style::FooterBlock);
        x += static_cast<int>(label.size());
        if (x < out.width) {
          drawText(out, x, row, L" ", Style::Default);
          ++x;
        }
      };
      auto addValueField = [&](std::string value, std::size_t w, Align a) {
        (void)a;
        if (value.size() > w) value.resize(w);
        drawText(out, x, row, widenAscii(value), Style::Default);
        x += static_cast<int>(value.size());
        if (x + 1 < out.width) {
          drawText(out, x, row, L"  ", Style::Default);
          x += 2;
        }
      };

      addLabelSpaced("NET 0");
      addLabelTight("RX:");
      addValueField(fmtSampleOrDashSpaced(state.latest.netRx), 12, Align::Left);
      addLabelTight("TX:");
      addValueField(fmtSampleOrDashSpaced(state.latest.netTx), 12, Align::Left);
    }
  }

}

static void renderHelp(Frame& out, int bodyTop) {
  int y = bodyTop + 1;
  const std::vector<std::string> lines = {
      std::string("AI-Z ") + AIZ_VERSION,
      "",
      "Terminal app to display performances metrics of CPU/NPU/GPU and run AI related benchmarks",
      "",
      "Pages:",
      "  Timelines  Live performance timelines (CPU/GPU/RAM/IO/Net/PCIe)",
      "  Hardware   OS/CPU/RAM/GPU/driver information",
      "  Benchmarks Run AI and bandwidth benchmarks",
      "  Config     Toggle timelines and view sampling settings",
      "  Minimal    Condensed live metrics view",
      "  Processes  Per-process CPU/GPU usage (when available)",
      "  Help       This help screen",
      "",
      "Options:",
      "  --debug      Run with synthetic/fake timelines",
      "  --help, -h   Show help and exit",
      "  --version    Print version and exit",
      "  --lang TAG   UI language (en, zh-CN). Also reads AI_Z_LANG / LANG",
      "",
  };

  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (y >= out.height - 2) break;
    const auto& line = lines[i];
    // Render the line but hide the parenthesized shortcut, then highlight the hot
    // letter inside the label word itself (htop-style).
    std::string shown = line;
    const std::size_t open = shown.find('(');
    if (open != std::string::npos) {
      const std::size_t close = shown.find(')', open);
      if (close != std::string::npos) {
        shown.erase(open, (close - open) + 1);
        while (open < shown.size() && shown[open] == ' ') shown.erase(open, 1);
      }
    }

    Style lineStyle = Style::Default;
    if (i == 0) {
      lineStyle = Style::Section;
    } else if (!line.empty()) {
      const std::size_t first = line.find_first_not_of(' ');
      if (first == 0 && line.back() == ':') lineStyle = Style::Section;
    }
    drawBodyLine(out, y, widenAscii(shown), lineStyle);

    // Highlight the intended hot letter within the label.
    // We infer it from the hidden (X) when present.
    if (open != std::string::npos && open + 2 < line.size() && line[open + 2] == ')') {
      const char hot = line[open + 1];
      if ((hot >= 'A' && hot <= 'Z') || (hot >= 'a' && hot <= 'z')) {
        const std::size_t pos = shown.find(hot);
        if (pos != std::string::npos) {
          drawText(out, static_cast<int>(pos), y, widenAscii(std::string(1, shown[pos])), Style::Hot);
        } else {
          const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(hot)));
          const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(hot)));
          std::size_t p2 = shown.find(lower);
          if (p2 == std::string::npos) p2 = shown.find(upper);
          if (p2 != std::string::npos) {
            drawText(out, static_cast<int>(p2), y, widenAscii(std::string(1, shown[p2])), Style::Hot);
          }
        }
      }
    }
    ++y;
  }

  if (out.height >= 3) {
    drawBodyLine(out, out.height - 2, L"Esc: back", Style::FooterKey);
  }
}

static void renderConfig(Frame& out, int bodyTop, const Config& cfg, const TuiState& state) {
  const int kToggleCount = configToggleCount();

  // Keep the two toggle tables symmetric.
  static_assert(kConfigToggleItems.size() == kConfigToggleItemsBars.size(), "toggle table mismatch");

  const int width = std::max(0, out.width);
  const int colGap = 2;
  const int colW = std::max(1, (width - colGap) / 2);
  const int colX0 = 0;
  const int colX1 = std::min(width, colW + colGap);

  int y = bodyTop + 1;
  if (y < out.height - 2) {
    ++y;  // spacer before toggles
  }

  // Section titles (two columns).
  drawBodyLine(out, y, L"", Style::Default);
  drawText(out, colX0, y, clipToWidth(i18n::tr(i18n::MsgId::ConfigSectionTimelines), static_cast<std::size_t>(colW)), Style::Section);
  drawText(out, colX1, y, clipToWidth(L"H. Bars", static_cast<std::size_t>(std::max(0, width - colX1))), Style::Section);
  ++y;

  std::size_t maxLabelW = 0;
  for (const auto& it : kConfigToggleItems) {
    maxLabelW = std::max<std::size_t>(maxLabelW, textWidth(i18n::tr(it.label)));
  }
  maxLabelW = std::max<std::size_t>(maxLabelW, textWidth(i18n::tr(i18n::MsgId::ConfigReadonlySamplesPerBucket)));
  maxLabelW = std::max<std::size_t>(maxLabelW, textWidth(i18n::tr(i18n::MsgId::ConfigReadonlySamplingRate)));
  maxLabelW = std::max<std::size_t>(maxLabelW, textWidth(i18n::tr(i18n::MsgId::ConfigTogglePeakValues)));
  maxLabelW = std::max<std::size_t>(maxLabelW, textWidth(i18n::tr(i18n::MsgId::ConfigReadonlyPeakWindow)));
  maxLabelW = std::max<std::size_t>(maxLabelW, textWidth(i18n::tr(i18n::MsgId::ConfigReadonlyValueColor)));
  maxLabelW = std::max<std::size_t>(maxLabelW, textWidth(i18n::tr(i18n::MsgId::ConfigReadonlyMetricNameColor)));

  auto drawToggleRow = [&](int x0, int colIndex, int rowIndex, const ConfigToggleItem& it, int yRow) {
    if (yRow >= out.height - 2) return;
    const bool selected = (rowIndex == state.configSel) && (state.configCol == colIndex);
    std::wstring line = selected ? L"> " : L"  ";

    const std::wstring_view label = i18n::tr(it.label);
    line += label;
    const std::size_t labelW = textWidth(label);
    if (labelW < maxLabelW) line.append(maxLabelW - labelW, L' ');
    line += L": ";

    const std::wstring_view onOff = (cfg.*(it.field)) ? i18n::tr(i18n::MsgId::ConfigToggleOn)
                                                      : i18n::tr(i18n::MsgId::ConfigToggleOff);
    line += std::wstring(onOff);

    drawText(out, x0, yRow, clipToWidth(line, static_cast<std::size_t>(colW)), Style::Value);

    const std::size_t valueStart = textWidth(std::wstring_view(line).substr(0, line.size() - onOff.size()));
    if (static_cast<int>(valueStart) < colW) {
      drawText(out, x0 + static_cast<int>(valueStart), yRow,
               clipToWidth(onOff, static_cast<std::size_t>(std::max(0, colW - static_cast<int>(valueStart)))),
               Style::Default);
    }
  };

  for (int i = 0; i < kToggleCount; ++i) {
    if (y >= out.height - 2) break;
    drawToggleRow(colX0, 0, i, kConfigToggleItems[static_cast<std::size_t>(i)], y);
    drawToggleRow(colX1, 1, i, kConfigToggleItemsBars[static_cast<std::size_t>(i)], y);
    ++y;
  }

  // Read-only misc info under Timelines.
  int readonlyIndex = configToggleCount();
  auto drawReadonly = [&](const std::wstring& label, const std::wstring& value) {
    if (y >= out.height - 2) return;
    const bool selected = (state.configSel == readonlyIndex);
    std::wstring line = selected ? L"> " : L"  ";
    line += label;
    const std::size_t labelW = textWidth(label);
    if (labelW < maxLabelW) line.append(maxLabelW - labelW, L' ');
    line += L": ";
    line += value;
    drawBodyLine(out, y, line, Style::Value);
    const std::size_t valueStart = textWidth(std::wstring_view(line).substr(0, line.size() - value.size()));
    drawText(out, static_cast<int>(valueStart), y, value, Style::Default);
    ++y;
    ++readonlyIndex;
  };

  if (y < out.height - 2) {
    ++y;  // spacer
  }
  if (y < out.height - 2) {
    drawBodyLine(out, y, std::wstring(i18n::tr(i18n::MsgId::ConfigSectionMisc)), Style::Section);
    ++y;
  }

  const int safeW = std::max(1, out.width);
  const std::uint32_t samples = cfg.timelineSamples;
  const std::uint32_t bucket = (samples > static_cast<std::uint32_t>(safeW))
      ? ((samples + static_cast<std::uint32_t>(safeW) - 1u) / static_cast<std::uint32_t>(safeW))
      : 1u;

  drawReadonly(std::wstring(i18n::tr(i18n::MsgId::ConfigReadonlySamplesPerBucket)), std::to_wstring(bucket));
  drawReadonly(std::wstring(i18n::tr(i18n::MsgId::ConfigReadonlySamplingRate)),
               std::to_wstring(cfg.refreshMs) + L"ms");
  drawReadonly(std::wstring(i18n::tr(i18n::MsgId::ConfigTogglePeakValues)),
               cfg.showPeakValues ? std::wstring(i18n::tr(i18n::MsgId::ConfigToggleOn))
                                  : std::wstring(i18n::tr(i18n::MsgId::ConfigToggleOff)));
  drawReadonly(std::wstring(i18n::tr(i18n::MsgId::ConfigReadonlyPeakWindow)),
               std::to_wstring(cfg.peakWindowSec) + L"s");
  drawReadonly(std::wstring(i18n::tr(i18n::MsgId::ConfigReadonlyValueColor)), L"white (default)");
  drawReadonly(std::wstring(i18n::tr(i18n::MsgId::ConfigReadonlyMetricNameColor)), metricNameColorLabel(cfg));
  drawReadonly(std::wstring(i18n::tr(i18n::MsgId::ConfigReadonlyGraphStyle)), graphStyleLabel(cfg));

}

static void renderHardware(Frame& out, int bodyTop, const TuiState& state) {
  struct Item {
    std::string key;
    std::string value;
  };

  std::vector<Item> items;
  items.reserve(state.hardwareLines.size());

  std::size_t maxKeyLen = 0;
  for (const auto& line : state.hardwareLines) {
    const std::size_t sep = line.find(": ");
    if (sep == std::string::npos) {
      items.push_back(Item{line, ""});
      maxKeyLen = std::max(maxKeyLen, line.size());
      continue;
    }

    const std::string key = line.substr(0, sep + 1);
    const std::string value = line.substr(sep + 2);
    items.push_back(Item{key, value});
    maxKeyLen = std::max(maxKeyLen, key.size());
  }

  const int yStart = bodyTop;
  const int yEnd = out.height - 2;
  const int valueCol = std::min(out.width - 1, static_cast<int>(maxKeyLen) + 2);

  int y = yStart;
  for (const auto& it : items) {
    if (y >= yEnd) break;
    drawBodyLine(out, y, widenAscii(it.key), Style::Section);
    if (!it.value.empty() && valueCol < out.width) {
      drawText(out, valueCol, y, widenAscii(it.value), Style::Default);
    }
    ++y;
  }

}

static void renderProcesses(Frame& out, int bodyTop, const TuiState& state) {
  const int sortY = bodyTop;
  const int headerY = bodyTop + 1;
  const int listStart = bodyTop + 2;
  const int listEnd = out.height - 2;

  if (sortY < out.height) {
    const std::wstring left = L"SORT BY: 1PROCESS NAME 2CPU 3GPU 4RAM 5VRAM";
    const std::wstring right = state.processesGpuOnly
        ? L"FILTER: G GPU ONLY [ON]"
        : L"FILTER: G GPU ONLY [OFF]";

    const int width = std::max(0, out.width);
    std::wstring line(static_cast<std::size_t>(width), L' ');
    std::vector<Style> styles(static_cast<std::size_t>(width), Style::Default);

    auto writeAt = [&](int x, std::wstring_view text) {
      if (x < 0 || x >= width) return;
      const int n = std::min<int>(static_cast<int>(text.size()), width - x);
      for (int i = 0; i < n; ++i) line[static_cast<std::size_t>(x + i)] = text[static_cast<std::size_t>(i)];
    };

    auto markAt = [&](int x, std::wstring_view text) {
      if (x < 0 || x >= width) return;
      const int n = std::min<int>(static_cast<int>(text.size()), width - x);
      for (int i = 0; i < n; ++i) styles[static_cast<std::size_t>(x + i)] = Style::FooterBlock;
    };

    auto markIn = [&](std::wstring_view hay, std::wstring_view needle, int offset) {
      const std::size_t pos = hay.find(needle);
      if (pos == std::wstring::npos) return;
      markAt(offset + static_cast<int>(pos), needle);
    };

    const int rightX = std::max(0, width - static_cast<int>(right.size()));
    writeAt(0, left);
    writeAt(rightX, right);

    markIn(left, L"SORT BY:", 0);
    markIn(left, L"PROCESS NAME", 0);
    markIn(left, L"CPU", 0);
    markIn(left, L"GPU", 0);
    markIn(left, L"RAM", 0);
    markIn(left, L"VRAM", 0);
    markIn(right, L"FILTER:", rightX);
    markIn(right, L"GPU ONLY", rightX);
    markIn(right, L"G", rightX);

    drawTextStyled(out, 0, sortY, line, styles);
  }

  if (headerY < out.height) {
    const int pidW = 6;
    const int whereW = 8;
    const int cpuW = 7;
    const int ramW = 9;
    const int gpuW = 7;
    const int vramW = 9;
    const int fixed = pidW + whereW + cpuW + ramW + gpuW + vramW + 6;
    const int remaining = std::max(0, out.width - fixed);
    int cmdW = 0;
    int nameW = remaining;
    if (remaining > 10) {
      cmdW = remaining / 2;
      nameW = remaining - cmdW;
    }

    std::string header =
        fit("PID", pidW, Align::Right) + " " +
        fit("Where", whereW, Align::Left) + " " +
        fit("Process", static_cast<std::size_t>(nameW), Align::Left) + " " +
        fit("CPU", cpuW, Align::Right) + " " +
        fit("RAM", ramW, Align::Right) + " " +
        fit("GPU", gpuW, Align::Right) + " " +
      fit("VRAM", vramW, Align::Right);
    if (cmdW > 0) header += " " + fit("Cmd", static_cast<std::size_t>(cmdW), Align::Left);
    drawBodyLine(out, headerY, widenAscii(header), Style::Section);
  }

  int y = listStart;
  if (y >= listEnd) return;

  if (state.processes.empty()) {
    drawBodyLine(out, y, L"No process data available.", Style::Value);
    return;
  }

  for (const auto& p : state.processes) {
    if (y >= listEnd) break;
    const int pidW = 6;
    const int whereW = 8;
    const int cpuW = 7;
    const int ramW = 9;
    const int gpuW = 7;
    const int vramW = 9;
    const int fixed = pidW + whereW + cpuW + ramW + gpuW + vramW + 6;
    const int remaining = std::max(0, out.width - fixed);
    int cmdW = 0;
    int nameW = remaining;
    if (remaining > 10) {
      cmdW = remaining / 2;
      nameW = remaining - cmdW;
    }

    const std::string pidStr = fit(std::to_string(p.pid), pidW, Align::Right);
    const std::string whereStr = fit(p.gpuIndex ? ("GPU" + std::to_string(*p.gpuIndex)) : "CPU", whereW, Align::Left);
    const std::string nameStr = fit(p.name.empty() ? std::string("?") : p.name, static_cast<std::size_t>(nameW), Align::Left);
    const std::string cpuStr = fit((p.cpuPct > 0.0) ? (fmt1(p.cpuPct) + "%") : std::string("--"), cpuW, Align::Right);
    const std::string ramStr = fit(p.ramBytes > 0 ? fmtBytes(p.ramBytes) : std::string("--"), ramW, Align::Right);
    const std::string gpuStr = fit(p.gpuUtilPct ? (fmt1(*p.gpuUtilPct) + "%") : std::string("--"), gpuW, Align::Right);
    const std::string vramStr = fit(p.vramUsedGiB ? (fmt1(*p.vramUsedGiB) + "G") : std::string("--"), vramW, Align::Right);

    std::string line = pidStr + " " + whereStr + " " + nameStr + " " + cpuStr + " " + ramStr + " " + gpuStr + " " + vramStr;
    if (cmdW > 0) {
      const std::string cmdStr = fit(p.cmdline.empty() ? std::string("--") : p.cmdline, static_cast<std::size_t>(cmdW), Align::Left);
      line += " " + cmdStr;
    }
    drawBodyLine(out, y, widenAscii(line), Style::Value);
    ++y;
  }
}

static void renderBenchmarks(Frame& out, int bodyTop, const TuiState& state) {
  // Snapshot benchmark execution state to avoid races with a worker thread.
  bool running = false;
  int runningIdx = -1;
  std::vector<std::string> results;
  {
    std::lock_guard<std::mutex> lk(state.benchMutex);
    running = state.benchmarksRunning;
    runningIdx = state.runningBenchIndex;
    results = state.benchResults;
  }

  int y = bodyTop;

  const auto& titles = (!state.benchRowTitles.empty() && state.benchRowTitles.size() == state.benches.size()) ? state.benchRowTitles : std::vector<std::string>{};
  const auto& isHeader = (!state.benchRowIsHeader.empty() && state.benchRowIsHeader.size() == state.benches.size()) ? state.benchRowIsHeader : std::vector<bool>{};

  std::size_t maxBenchNameLen = 0;
  for (std::size_t i = 0; i < state.benches.size(); ++i) {
    const bool hdr = (!isHeader.empty() ? isHeader[i] : (state.benches[i] == nullptr));
    if (hdr) continue;
    const std::string name = (!titles.empty() ? titles[i] : (state.benches[i] ? state.benches[i]->name() : std::string("(null)")));
    maxBenchNameLen = std::max(maxBenchNameLen, name.size());
  }

  auto placeholderForName = [&](const std::string& n) -> std::string {
    if (n.find("PCIe") != std::string::npos || n.find("PCI") != std::string::npos) return "-- GB/s";
    if (n.find("FP") != std::string::npos || n.find("FLOPS") != std::string::npos) return "-- GFLOPS";
    if (n.find("INT") != std::string::npos) return "-- GOPS";
    return "--";
  };

  auto resultTextForRow = [&](int row) -> std::string {
    if (row < 0 || row >= static_cast<int>(results.size())) return "--";
    const auto& s = results[static_cast<std::size_t>(row)];
    if (!s.empty()) return s;
    const std::string n = (!titles.empty() && row >= 0 && static_cast<std::size_t>(row) < titles.size()) ? titles[static_cast<std::size_t>(row)] : std::string{};
    return placeholderForName(n);
  };

  std::string lastHeaderKind;

  // Special entry: run all.
  {
    if (y < out.height - 2) {
      drawBodyLine(out, y, L"", Style::Default);
      const std::wstring prefix = L"1";
      const std::wstring runAll = L"RUN ALL BENCHMARKS";
      const std::wstring mid = L" 2";
      const std::wstring gen = L"GENERATE HTML REPORT";

      int x = 0;
      drawText(out, x, y, prefix, Style::Default);
      x += static_cast<int>(prefix.size());
      drawText(out, x, y, runAll, Style::FooterActive);
      x += static_cast<int>(runAll.size());
      drawText(out, x, y, mid, Style::Default);
      x += static_cast<int>(mid.size());
      drawText(out, x, y, gen, Style::FooterActive);
      ++y;
    }
  }

  for (int row = 0; row < static_cast<int>(state.benches.size()); ++row) {
    if (y >= out.height - 2) break;

    const int rowIndex = row + 1;  // shifted by run-all
    const bool hdr = (!isHeader.empty() ? isHeader[static_cast<std::size_t>(row)] : (state.benches[static_cast<std::size_t>(row)] == nullptr));
    const auto& b = state.benches[static_cast<std::size_t>(row)];

    const std::string name = (!titles.empty() ? titles[static_cast<std::size_t>(row)] : (b ? b->name() : std::string("(null)")));
    const bool avail = (!hdr && b && b->isAvailable());

    const bool selected = (!hdr && rowIndex == state.benchmarksSel);
    const std::wstring prefix = selected ? L"> " : L"  ";

    drawBodyLine(out, y, L"", Style::Default);
    drawText(out, 0, y, prefix, selected ? Style::Hot : Style::Default);

    if (hdr) {
      const std::string kind = (name.rfind("GPU", 0) == 0) ? "GPU" : ((name.rfind("CPU", 0) == 0) ? "CPU" : "");
      if (kind == "CPU" && lastHeaderKind == "GPU") {
        if (y < out.height - 2) {
          ++y;
          if (y >= out.height - 2) break;
          drawBodyLine(out, y, L"", Style::Default);
        }
        if (y < out.height - 2) {
          ++y;
          if (y >= out.height - 2) break;
          drawBodyLine(out, y, L"", Style::Default);
        }
      }

      drawText(out, static_cast<int>(prefix.size()), y, widenAscii(name), Style::Section);
      if (!kind.empty()) lastHeaderKind = kind;
      ++y;
      continue;
    }

    // Benchmark rows: show a 1-char spinner and "NAME: value" on a single line.
    std::string r;
    std::wstring firstLineW;
    if (row >= 0 && row < static_cast<int>(results.size()) && !results[static_cast<std::size_t>(row)].empty()) {
      r = results[static_cast<std::size_t>(row)];
    } else if (b && !b->isAvailable()) {
      firstLineW = std::wstring(i18n::tr(i18n::MsgId::BenchUnavailable));
    } else {
      r = resultTextForRow(row);
    }

    const std::size_t end0 = r.find('\n');
    const std::string firstLine = (end0 == std::string::npos) ? r : r.substr(0, end0);
    if (firstLineW.empty()) firstLineW = widenAscii(firstLine);

    wchar_t spin = L' ';
    if (running && runningIdx == row) {
      static const wchar_t kSpin[4] = {L'|', L'/', L'-', L'\\'};
      spin = kSpin[static_cast<std::size_t>(state.tick % 4u)];
    }

    constexpr int kMetricIndent = 4;
    std::wstring namePart(kMetricIndent, L' ');
    namePart.push_back(spin);
    namePart.push_back(L' ');
    namePart += widenAscii(name);

    const std::size_t targetLen = static_cast<std::size_t>(kMetricIndent) + 2u + maxBenchNameLen;
    if (namePart.size() < targetLen) namePart.append(targetLen - namePart.size(), L' ');

    const std::wstring valuePart = L": " + firstLineW;

    int x = static_cast<int>(prefix.size());
    drawText(out, x, y, namePart, avail ? Style::Section : Style::Value);
    x += static_cast<int>(namePart.size());
    drawText(out, x, y, valuePart, Style::Default);
    ++y;

    // Extra result lines (errors/details) underneath, indented.
    if (!r.empty() && end0 != std::string::npos) {
      std::size_t start = end0 + 1;
      while (start <= r.size() && y < out.height - 2) {
        const std::size_t end = r.find('\n', start);
        const std::string extra = (end == std::string::npos) ? r.substr(start) : r.substr(start, end - start);
        drawBodyLine(out, y, L"", Style::Default);
        const int ix = static_cast<int>(prefix.size()) + 2;
        drawText(out, ix, y, widenAscii(extra), Style::Default);
        ++y;
        if (end == std::string::npos) break;
        start = end + 1;
      }
    }
  }

}

void renderFrame(Frame& out, const Viewport& vp, const TuiState& state, const Config& cfg, bool /*debugMode*/) {
  out.resize(vp.width, vp.height);
  out.clear(Cell{L' ', static_cast<std::uint16_t>(Style::Default)});

  if (out.width <= 0 || out.height <= 0) return;

  // Header is drawn per-screen; avoid filling with a background color.
  drawBodyLine(out, 0, L"", Style::Default);

  // Footer
  if (out.height >= 2) {
    const int y = out.height - 1;
    drawBodyLine(out, y, L"", Style::Default);

    const std::wstring_view footer = i18n::tr(i18n::MsgId::FooterNav);
    std::vector<Style> st;
    // Default white for everything; we'll selectively apply blue to label letters.
    st.resize(footer.size(), Style::Default);

    // Special prefix: ESCMain
    // ESC = default white, Main = blue background, with 'a' highlighted.
    if (footer.size() >= 6 && footer.rfind(L"ESC", 0) == 0) {
      const std::size_t labelStart = 3;
      std::size_t labelEnd = footer.size();

      const std::size_t nextSpace = footer.find(L' ', labelStart);
      if (nextSpace != std::wstring_view::npos) labelEnd = std::min(labelEnd, nextSpace);

      const std::size_t nextF = footer.find(L'F', labelStart);
      if (nextF != std::wstring_view::npos) labelEnd = std::min(labelEnd, nextF);

      for (std::size_t k = labelStart; k < labelEnd; ++k) {
        st[k] = Style::FooterBlock;
      }

      if (state.screen == Screen::Timelines) {
        for (std::size_t k = labelStart; k < labelEnd; ++k) {
          st[k] = Style::FooterActive;
        }
      }

      for (std::size_t k = labelStart; k < labelEnd; ++k) {
        if (footer[k] == L'a' || footer[k] == L'A') {
          if (state.screen != Screen::Timelines) st[k] = Style::FooterHot;
          break;
        }
      }
    }

    // Blue background for the *label text* following F-key tokens.
    // Example (en): "F1 Help (H)" -> highlight "Help"
    // Example (zh): "F1 帮助(H)" -> highlight "帮助"
    for (std::size_t i = 0; i < footer.size(); ++i) {
      if (footer[i] != L'F') continue;

      // Parse F + digits.
      std::size_t j = i + 1;
      bool anyDigit = false;
      while (j < footer.size() && footer[j] >= L'0' && footer[j] <= L'9') {
        anyDigit = true;
        ++j;
      }
      if (!anyDigit) continue;

      // Label starts immediately after the digits (F1Help style).
      std::size_t labelStart = j;
      if (labelStart >= footer.size()) {
        i = j;
        continue;
      }

      // Label ends at the next token boundary.
      // Historically tokens were space-separated. Now we also support a compact
      // format like "...F1HelpF2Hardware...".
      std::size_t labelEnd = footer.size();
      const std::size_t nextSpace = footer.find(L' ', labelStart);
      if (nextSpace != std::wstring_view::npos) labelEnd = std::min(labelEnd, nextSpace);
      const std::size_t nextF = footer.find(L'F', labelStart);
      if (nextF != std::wstring_view::npos) labelEnd = std::min(labelEnd, nextF);

      for (std::size_t k = labelStart; k < labelEnd; ++k) {
        st[k] = Style::FooterBlock;
      }

      // Hot letter inside the label word itself.
      int fnum = 0;
      for (std::size_t p = i + 1; p < j; ++p) {
        fnum = (fnum * 10) + static_cast<int>(footer[p] - L'0');
      }
      wchar_t hot = 0;
      Screen target = Screen::Timelines;
      switch (fnum) {
        case 1: hot = L'H'; target = Screen::Help; break;        // Help
        case 2: hot = L'W'; target = Screen::Hardware; break;    // Hardware (the 'w' in HardWare)
        case 3: hot = L'B'; target = Screen::Benchmarks; break;  // Bench
        case 4: hot = L'C'; target = Screen::Config; break;      // Config
        case 5: hot = L'P'; target = Screen::Processes; break;   // Processes
        case 10: hot = L'Q'; break;  // Quit
        default: break;
      }

      const bool isActive = (state.screen == target && target != Screen::Timelines);
      if (isActive) {
        for (std::size_t k = labelStart; k < labelEnd; ++k) {
          st[k] = Style::FooterActive;
        }
      }

      if (hot != 0 && !isActive) {
        for (std::size_t k = labelStart; k < labelEnd; ++k) {
          const wchar_t ch = footer[k];
          if (ch == hot || ch == static_cast<wchar_t>(std::towlower(static_cast<wint_t>(hot))) ||
              ch == static_cast<wchar_t>(std::towupper(static_cast<wint_t>(hot)))) {
            st[k] = Style::FooterHot;
            break;
          }
        }
      }

      i = (labelEnd > 0) ? (labelEnd - 1) : i;
    }

    // View labels: highlight the active view name.
    {
      const std::wstring_view view1 = i18n::tr(i18n::MsgId::FooterViewTimelines);
      const std::wstring_view view2 = i18n::tr(i18n::MsgId::FooterViewBars);
      const std::wstring_view view3 = i18n::tr(i18n::MsgId::FooterViewMinimal);

      const std::size_t pos1 = footer.find(view1);
      if (pos1 != std::wstring_view::npos) {
        for (std::size_t k = pos1; k < pos1 + view1.size() && k < st.size(); ++k) {
          st[k] = (state.screen == Screen::Timelines && state.timelineView == TimelineView::Timelines) 
                  ? Style::FooterActive : Style::FooterBlock;
        }
      }

      const std::size_t pos2 = footer.find(view2);
      if (pos2 != std::wstring_view::npos) {
        for (std::size_t k = pos2; k < pos2 + view2.size() && k < st.size(); ++k) {
          st[k] = (state.screen == Screen::Timelines && state.timelineView == TimelineView::Bars) 
                  ? Style::FooterActive : Style::FooterBlock;
        }
      }

      const std::size_t pos3 = footer.find(view3);
      if (pos3 != std::wstring_view::npos) {
        for (std::size_t k = pos3; k < pos3 + view3.size() && k < st.size(); ++k) {
          st[k] = (state.screen == Screen::Minimal) ? Style::FooterActive : Style::FooterBlock;
        }
      }
    }

    drawTextStyled(out, 0, y, footer, st);
  }

  // Body
  const int bodyTop = 1;

  switch (state.screen) {
    case Screen::Timelines:
      // Timelines uses the full header area, so skip the generic title row.
      renderTimelines(out, bodyTop, state, cfg);
      break;
    case Screen::Minimal:
      // Minimal uses the same top stats rows as Timelines, but no graphs.
      renderMinimal(out, bodyTop, state);
      break;
    case Screen::Help:
      renderHelp(out, bodyTop);
      break;
    case Screen::Config:
      {
        const std::wstring hint = L"Space:Toggle  Tab/←/→:Column  S:Save  R:Reset to Defaults";
        std::vector<Style> hintStyles(hint.size(), Style::Section);

        const std::size_t togglePos = hint.find(L"Toggle");
        if (togglePos != std::wstring::npos) {
          for (std::size_t i = 0; i < 6 && togglePos + i < hintStyles.size(); ++i) {
            hintStyles[togglePos + i] = Style::FooterBlock;
          }
        }

        const std::size_t savePos = hint.find(L"Save");
        if (savePos != std::wstring::npos) {
          for (std::size_t i = savePos; i < savePos + 4 && i < hintStyles.size(); ++i) {
            hintStyles[i] = Style::FooterBlock;
          }
        }

        const std::size_t resetPos = hint.find(L"Reset to Defaults");
        if (resetPos != std::wstring::npos) {
          for (std::size_t i = resetPos; i < resetPos + 18 && i < hintStyles.size(); ++i) {
            hintStyles[i] = Style::FooterBlock;
          }
        }

        drawTextStyled(out, 0, bodyTop, hint, hintStyles);
      }
      renderConfig(out, bodyTop, cfg, state);
      break;
    case Screen::Hardware:
      renderHardware(out, bodyTop, state);
      break;
    case Screen::Benchmarks:
      renderBenchmarks(out, bodyTop, state);
      break;
    case Screen::Processes:
      renderProcesses(out, bodyTop, state);
      break;
    default:
      drawText(out, 0, bodyTop + 1, L"(Screen not yet ported to shared core)", Style::Value);
      break;
  }

  // Status line (one row above footer).
  if ((state.screen == Screen::Benchmarks || state.screen == Screen::Config) &&
      out.height >= 2 && state.statusLine && !state.statusLine->empty()) {
    const int y = out.height - 2;
    drawBodyLine(out, y, L"", Style::Default);
    drawText(out, 0, y, widenAscii(*state.statusLine), Style::Warning);
  }
}

}  // namespace aiz
