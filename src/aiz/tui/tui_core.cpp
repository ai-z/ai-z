#include <aiz/tui/tui_core.h>

#include <aiz/bench/bench.h>
#include <aiz/i18n.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <limits>
#include <mutex>
#include <wchar.h>

namespace aiz {

struct ConfigToggleItem {
  i18n::MsgId label;
  bool Config::*field;
};

static constexpr std::array<ConfigToggleItem, 11> kConfigToggleItems = {{
    {i18n::MsgId::ConfigToggleCpuUsage, &Config::showCpu},
    {i18n::MsgId::ConfigToggleGpuUsage, &Config::showGpu},
    {i18n::MsgId::ConfigToggleGpuMemCtrl, &Config::showGpuMem},
    {i18n::MsgId::ConfigToggleDiskRead, &Config::showDiskRead},
    {i18n::MsgId::ConfigToggleDiskWrite, &Config::showDiskWrite},
    {i18n::MsgId::ConfigToggleNetRx, &Config::showNetRx},
    {i18n::MsgId::ConfigToggleNetTx, &Config::showNetTx},
    {i18n::MsgId::ConfigTogglePcieRx, &Config::showPcieRx},
    {i18n::MsgId::ConfigTogglePcieTx, &Config::showPcieTx},
    {i18n::MsgId::ConfigToggleRamUsage, &Config::showRam},
    {i18n::MsgId::ConfigToggleVramUsage, &Config::showVram},
}};

static constexpr int configToggleCount() {
  return static_cast<int>(kConfigToggleItems.size());
}

static constexpr int configResetRowIndex() {
  return configToggleCount();
}

static constexpr int configItemCount() {
  return configToggleCount() + 1;
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

    if (cmd == Command::Toggle) {
      if (state.configSel >= 0 && state.configSel < configToggleCount()) {
        const auto& it = kConfigToggleItems[static_cast<std::size_t>(state.configSel)];
        cfg.*(it.field) = !(cfg.*(it.field));
      } else if (state.configSel == configResetRowIndex()) {
        cfg = Config{};
      }
      return;
    }

    if (cmd == Command::Defaults) {
      cfg = Config{};
      return;
    }

    if (cmd == Command::Save) {
      cfg.save();
      return;
    }

    if (cmd == Command::Activate) {
      // Enter only activates the action row to avoid accidental toggles.
      if (state.configSel == configResetRowIndex()) cfg = Config{};
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

static void drawSectionTitleLineSplit(
    Frame& out,
    int y,
    const std::string& left,
    const std::string& value,
    const std::string& right) {
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
    std::vector<Style> styles(wtitle.size(), Style::Section);
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
      drawText(out, rightStart, y, widenAscii(rightFit), Style::Section);
    }
  }

  // Fill separator dashes in the gap between left and right.
  const int dashStart = std::min(out.width, static_cast<int>(title.size()) + 1);
  const int dashEnd = std::min(out.width, rightStart);
  for (int x = dashStart; x < dashEnd; ++x) {
    auto& c = out.at(x, y);
    c.ch = L'-';
    c.style = static_cast<std::uint16_t>(Style::Section);
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

static void drawScrollingBars(
    Frame& out,
    int topY,
    int leftX,
    int height,
    int width,
    const Timeline& tl,
    double maxV,
    TimelineAgg agg) {
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

  // If we have more samples than columns, downsample into buckets.
  const int cols = colsToDraw;
  const int count = std::min(cols, available);
  const int xOffset = cols - count;

  const bool needsBucketing = available > cols;
  const int bucket = needsBucketing ? static_cast<int>((available + cols - 1) / cols) : 1;

  for (int i = 0; i < count; ++i) {
    const int x = leftX + xOffset + i;
    if (x < 0 || x >= out.width) continue;

    double v = 0.0;
    if (!needsBucketing) {
      // 1:1 mapping for the most recent samples.
      const int startIdx = std::max(0, available - count);
      const int idx = startIdx + i;
      if (idx < 0 || idx >= available) continue;
      v = vals[static_cast<std::size_t>(idx)];
    } else {
      // Map the full history into the available columns.
      const int start = i * bucket;
      const int end = std::min(available, start + bucket);
      if (start >= end) continue;

      if (agg == TimelineAgg::Avg) {
        double sum = 0.0;
        int n = 0;
        for (int j = start; j < end; ++j) {
          double s = vals[static_cast<std::size_t>(j)];
          if (!std::isfinite(s)) continue;
          sum += s;
          ++n;
        }
        v = (n > 0) ? (sum / static_cast<double>(n)) : 0.0;
      } else {
        // Default: max in bucket (preserves spikes).
        double maxBucket = -std::numeric_limits<double>::infinity();
        for (int j = start; j < end; ++j) {
          double s = vals[static_cast<std::size_t>(j)];
          if (!std::isfinite(s)) continue;
          maxBucket = std::max(maxBucket, s);
        }
        if (!std::isfinite(maxBucket)) maxBucket = 0.0;
        v = maxBucket;
      }
    }

    if (!std::isfinite(v)) v = 0.0;
    v = std::clamp(v, 0.0, maxV);

    const int barH = static_cast<int>(std::round((v / maxV) * static_cast<double>(height)));
    const int clampedH = std::clamp(barH, 0, height);

    for (int r = 0; r < clampedH; ++r) {
      const int y = topY + (height - 1 - r);
      if (y < 0 || y >= out.height) continue;
      auto& c = out.at(x, y);
      c.ch = 0x2593;  // dark shade block
      // Timeline bars used to render in the default terminal color (white).
      // Keep title values highlighted, but keep the bars neutral.
      c.style = static_cast<std::uint16_t>(Style::Default);
    }
  }
}

static void renderTimelines(Frame& out, int /*bodyTop*/, const TuiState& state, const Config& cfg) {
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
        drawText(f, x, 0, widenAscii(label), Style::Section);
        x += static_cast<int>(label.size());
        if (x < f.width) {
          drawText(f, x, 0, L" ", Style::Default);
          ++x;
        }
      };
      auto addValueField = [&](std::string value, std::size_t w, Align a) {
        value = fit(std::move(value), w, a);
        drawText(f, x, 0, widenAscii(value), Style::Value);
        x += static_cast<int>(value.size());
        if (x + 1 < f.width) {
          drawText(f, x, 0, L"  ", Style::Default);
          x += 2;
        }
      };

      const std::string cpuStr = state.latest.cpu ? (fmt0(state.latest.cpu->value) + "%") : std::string("--");
      addBadge("CPU 0");
      addPunct(L":", Style::Section);
      addValueField(cpuStr, 4, Align::Right);

      addLabel("RAM:");
      addValueField(!state.latest.ramText.empty() ? state.latest.ramText : std::string("--"), 18, Align::Left);

      const std::string diskStr = fmtSampleOrDash(state.latest.diskRead) + "/" + fmtSampleOrDash(state.latest.diskWrite);
      addLabel("DISK R/W:");
      addValueField(diskStr, 24, Align::Left);

      const std::string netStr = fmtSampleOrDash(state.latest.netRx) + "/" + fmtSampleOrDash(state.latest.netTx);
      addLabel("NET R/T:");
      addValueField(netStr, 24, Align::Left);
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
        drawText(f, x, row, widenAscii(label), Style::Section);
        x += static_cast<int>(label.size());
        if (x < f.width) {
          drawText(f, x, row, L" ", Style::Default);
          ++x;
        }
      };
      auto addValueField = [&](std::string value, std::size_t w, Align a) {
        value = fit(std::move(value), w, a);
        drawText(f, x, row, widenAscii(value), Style::Value);
        x += static_cast<int>(value.size());
        if (x + 1 < f.width) {
          drawText(f, x, row, L"  ", Style::Default);
          x += 2;
        }
      };

      const auto& gt = state.latest.gpus[static_cast<std::size_t>(i)];
      // Header should be compact: GPU label is just "GPU <n>" (device name is shown elsewhere).
      const std::string gpuId = "GPU " + std::to_string(i);
      addBadge(gpuId);
      addPunct(L":", Style::Section);

      addLabel("USAGE");
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
        linkStr = std::to_string(*gt.pcieLinkWidth) + "x@" + fmt1(static_cast<double>(*gt.pcieLinkGen));
      }
      addLabel("LINK:");
      addValueField(linkStr, 9, Align::Left);

      const std::string pcieTStr = state.latest.pcieTx ? (fmt0(state.latest.pcieTx->value) + state.latest.pcieTx->unit) : std::string("--");
      const std::string pcieRStr = state.latest.pcieRx ? (fmt0(state.latest.pcieRx->value) + state.latest.pcieRx->unit) : std::string("--");
      addLabel("PCIE T/R:");
      addValueField(pcieTStr + "/" + pcieRStr, 22, Align::Left);
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
  };

  auto gpuContext = [&](std::size_t index) -> std::string {
    if (index >= state.gpuDeviceNames.size()) return "#" + std::to_string(index);
    const std::string& n = state.gpuDeviceNames[index];
    if (n.empty() || n == ("GPU" + std::to_string(index))) return "#" + std::to_string(index);
    return n + " #" + std::to_string(index);
  };

  // Build per-GPU usage samples for title lines (USAGE: xx %).
  std::vector<std::optional<Sample>> gpuUsageSamples;
  gpuUsageSamples.reserve(state.latest.gpus.size());
  for (const auto& gt : state.latest.gpus) {
    if (gt.utilPct) gpuUsageSamples.push_back(Sample{*gt.utilPct, "%", {}});
    else gpuUsageSamples.push_back(std::nullopt);
  }

  std::vector<Panel> panels;
  panels.push_back(Panel{"CPU", cfg.showCpu, &state.latest.cpu, &state.cpuTl, nullptr, 100.0, false, state.cpuDevice});

  // CPU/RAM first.
  panels.push_back(Panel{"RAM", cfg.showRam, &state.latest.ramPct, &state.ramTl, nullptr, 100.0, false, state.ramDevice});

  // GPU usage before VRAM and MemCtrl.
  if (cfg.showGpu) {
    const std::size_t n = std::min(state.gpuTls.size(), gpuUsageSamples.size());
    for (std::size_t i = 0; i < n; ++i) {
      panels.push_back(Panel{"USAGE", true, &gpuUsageSamples[i], &state.gpuTls[i], nullptr, 100.0, false, gpuContext(i)});
    }
  }

  panels.push_back(Panel{"VRAM", cfg.showVram, &state.latest.vramPct, &state.vramTl, nullptr, 100.0, false, gpuContext(0)});
  panels.push_back(Panel{"MemCtrl", cfg.showGpuMem, &state.latest.gpuMemUtil, &state.gpuMemUtilTl, nullptr, 100.0, false, gpuContext(0)});

  // PCIe split RX/TX (tweak restored).
  panels.push_back(Panel{"PCIe RX", cfg.showPcieRx, &state.latest.pcieRx, &state.pcieRxTl, nullptr, 32'000.0, false, gpuContext(0)});
  panels.push_back(Panel{"PCIe TX", cfg.showPcieTx, &state.latest.pcieTx, &state.pcieTxTl, nullptr, 32'000.0, false, gpuContext(0)});

  // Disk/network last.
  panels.push_back(Panel{"Disk Read", cfg.showDiskRead, &state.latest.diskRead, &state.diskReadTl, nullptr, 5000.0, false, state.diskDevice});
  panels.push_back(Panel{"Disk Write", cfg.showDiskWrite, &state.latest.diskWrite, &state.diskWriteTl, nullptr, 5000.0, false, state.diskDevice});

  panels.push_back(Panel{"Net RX", cfg.showNetRx, &state.latest.netRx, &state.netRxTl, nullptr, 5000.0, false, state.netDevice});
  panels.push_back(Panel{"Net TX", cfg.showNetTx, &state.latest.netTx, &state.netTxTl, nullptr, 5000.0, false, state.netDevice});

  panels.erase(
      std::remove_if(panels.begin(), panels.end(), [](const Panel& p) { return !p.enabled; }),
      panels.end());

  if (panels.empty()) {
    drawBodyLine(out, 2, std::wstring(i18n::tr(i18n::MsgId::TimelinesNoneEnabled)), Style::Value);
    return;
  }

  const int labelRows = 1;
  const int n = static_cast<int>(panels.size());
  const int minGraphRows = 3;
  const int perPanel = std::max(labelRows + minGraphRows, usable / std::max(1, n));

  int y = top;
  for (int i = 0; i < n; ++i) {
    const auto& p = panels[static_cast<std::size_t>(i)];
    const int remainingPanels = n - i;
    const int remainingUsable = std::max(0, (out.height - bottomReserved) - y);
    const int height = (remainingPanels == 1) ? std::max(0, remainingUsable) : std::min(perPanel, remainingUsable);
    if (height <= 0) break;

    const std::string right = p.device;
    std::string section = p.name;
    if (p.sample && p.sample->has_value()) {
      section += ": ";
      const std::string value = fmt1((*p.sample)->value) + " " + (*p.sample)->unit;
      drawSectionTitleLineSplit(out, y, section, value, right);
    } else if (p.sample != nullptr) {
      section += ": ";
      drawSectionTitleLineSplit(out, y, section, "unavailable", right);
    } else {
      drawSectionTitleLineSplit(out, y, section, std::string{}, right);
    }

    const int graphTop = y + 1;
    const int graphH = std::max(0, height - 1);
    if (graphH > 0 && p.tl) {
      drawScrollingBars(out, graphTop, 0, graphH, out.width, *p.tl, p.maxV, cfg.timelineAgg);
    }

    y += height;
  }
}

static void renderMinimal(Frame& out, int /*bodyTop*/, const TuiState& state) {
  // Reuse the Timelines top header (stats rows), but do not draw any scrolling bars.
  // This view is intended to be lightweight / text-only.
  const int headerRows = 1 + static_cast<int>(state.latest.gpus.size());

  // Render the same stats header as Timelines by calling into renderTimelines' logic.
  // We keep it simple by delegating through a minimal local copy of the header code.
  // (Avoids exposing new shared helpers in headers.)
  {
    // Row 0
    drawBodyLine(out, 0, L"", Style::Default);

    int x = 0;
    auto addLabel = [&](const std::string& label) {
      drawText(out, x, 0, widenAscii(label), Style::Section);
      x += static_cast<int>(label.size());
      if (x < out.width) {
        drawText(out, x, 0, L" ", Style::Default);
        ++x;
      }
    };
    auto addValueField = [&](std::string value, std::size_t w, Align a) {
      value = fit(std::move(value), w, a);
      drawText(out, x, 0, widenAscii(value), Style::Value);
      x += static_cast<int>(value.size());
      if (x + 1 < out.width) {
        drawText(out, x, 0, L"  ", Style::Default);
        x += 2;
      }
    };

    const std::string cpuStr = state.latest.cpu ? (fmt0(state.latest.cpu->value) + "%") : std::string("--");
    addLabel("CPU:");
    addValueField(cpuStr, 4, Align::Right);

    addLabel("RAM:");
    addValueField(!state.latest.ramText.empty() ? state.latest.ramText : std::string("--"), 18, Align::Left);

    const std::string diskStr = fmtSampleOrDash(state.latest.diskRead) + "/" + fmtSampleOrDash(state.latest.diskWrite);
    addLabel("DISK R/W:");
    addValueField(diskStr, 24, Align::Left);

    const std::string netStr = fmtSampleOrDash(state.latest.netRx) + "/" + fmtSampleOrDash(state.latest.netTx);
    addLabel("NET R/T:");
    addValueField(netStr, 24, Align::Left);
  }

  for (int i = 0; i < static_cast<int>(state.latest.gpus.size()); ++i) {
    const int row = 1 + i;
    if (row >= out.height) break;
    drawBodyLine(out, row, L"", Style::Default);

    int x = 0;
    auto addLabel = [&](const std::string& label) {
      drawText(out, x, row, widenAscii(label), Style::Section);
      x += static_cast<int>(label.size());
      if (x < out.width) {
        drawText(out, x, row, L" ", Style::Default);
        ++x;
      }
    };
    auto addValueField = [&](std::string value, std::size_t w, Align a) {
      value = fit(std::move(value), w, a);
      drawText(out, x, row, widenAscii(value), Style::Value);
      x += static_cast<int>(value.size());
      if (x + 1 < out.width) {
        drawText(out, x, row, L"  ", Style::Default);
        x += 2;
      }
    };

    const auto& gt = state.latest.gpus[static_cast<std::size_t>(i)];
    std::string gpuPrefix = "GPU " + std::to_string(i);
    if (static_cast<std::size_t>(i) < state.gpuDeviceNames.size()) {
      const std::string& n = state.gpuDeviceNames[static_cast<std::size_t>(i)];
      if (!n.empty() && n != ("GPU" + std::to_string(i)) && n != "--" && n != "unknown") {
        gpuPrefix += " - ";
        gpuPrefix += n;
      }
    }
    addLabel(gpuPrefix + ":");

    addLabel("USAGE");
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
      linkStr = std::to_string(*gt.pcieLinkWidth) + "x@" + fmt1(static_cast<double>(*gt.pcieLinkGen));
    }
    addLabel("LINK:");
    addValueField(linkStr, 9, Align::Left);

    const std::string pcieTStr = state.latest.pcieTx ? (fmt0(state.latest.pcieTx->value) + state.latest.pcieTx->unit) : std::string("--");
    const std::string pcieRStr = state.latest.pcieRx ? (fmt0(state.latest.pcieRx->value) + state.latest.pcieRx->unit) : std::string("--");
    addLabel("PCIE T/R:");
    addValueField(pcieTStr + "/" + pcieRStr, 22, Align::Left);
  }

  const int noteY = headerRows + 1;
  if (noteY < out.height - 2) {
    drawBodyLine(out, noteY, L"Minimal: stats only (no timelines). Press Esc for timelines.", Style::Value);
  }
}

static void renderHelp(Frame& out, int bodyTop) {
  int y = bodyTop + 1;
  const std::vector<std::string> lines = {
      "ai-z",  // version/website will be added when shared core has access to version.h
      "",
      "Description:",
      "  C++ TUI for performance timelines and benchmarks.",
      "  Timelines: CPU, RAM, VRAM, GPU, Disk Read/Write, Net RX/TX, PCIe RX/TX.",
      "",
      "Guide:",
      "  - Timelines are vertical bars that scroll over time.",
      "  - Use Config to enable/disable timelines.",
      "  - Use Hardware to view OS/CPU/RAM/GPU/driver information.",
      "  - Use Benchmarks to run synthetic tests (stubs for now).",
      "",
      "Keys:",
      "  F1  Help        (H)",
      "  F2  Hardware    (W)",
      "  F3  Benchmarks  (B)",
      "  F4  Config      (C)",
      "  F5  Minimal     (M)",
      "  F10 Quit        (Q)",
        "  Esc Main",
  };

  for (const auto& line : lines) {
    if (y >= out.height - 2) break;
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

    drawBodyLine(out, y, widenAscii(shown), Style::Value);

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
  const int kActionReset = configResetRowIndex();

  int y = bodyTop + 1;
  drawBodyLine(out, y, std::wstring(i18n::tr(i18n::MsgId::ConfigSectionTimelines)), Style::Section);
  ++y;

  std::size_t maxLabelW = 0;
  for (const auto& it : kConfigToggleItems) {
    maxLabelW = std::max<std::size_t>(maxLabelW, textWidth(i18n::tr(it.label)));
  }
  maxLabelW = std::max<std::size_t>(maxLabelW, textWidth(i18n::tr(i18n::MsgId::ConfigResetToDefaults)));
  maxLabelW = std::max<std::size_t>(maxLabelW, textWidth(i18n::tr(i18n::MsgId::ConfigReadonlySamplesPerBucket)));
  maxLabelW = std::max<std::size_t>(maxLabelW, textWidth(i18n::tr(i18n::MsgId::ConfigReadonlyValueColor)));
  maxLabelW = std::max<std::size_t>(maxLabelW, textWidth(i18n::tr(i18n::MsgId::ConfigReadonlyMetricNameColor)));

  for (int i = 0; i < kToggleCount; ++i) {
    std::wstring line = (i == state.configSel ? L"> " : L"  ");
    const auto& it = kConfigToggleItems[static_cast<std::size_t>(i)];
    const std::wstring_view label = i18n::tr(it.label);
    line += label;
    const std::size_t labelW = textWidth(label);
    if (labelW < maxLabelW) line.append(maxLabelW - labelW, L' ');
    line += L": ";
    line += std::wstring((cfg.*(it.field)) ? i18n::tr(i18n::MsgId::ConfigToggleOn) : i18n::tr(i18n::MsgId::ConfigToggleOff));
    if (y >= out.height - 2) break;
    drawBodyLine(out, y, line, Style::Value);
    ++y;
  }

  // Action row.
  {
    std::wstring line = (kActionReset == state.configSel ? L"> " : L"  ");
    const std::wstring_view resetLabel = i18n::tr(i18n::MsgId::ConfigResetToDefaults);
    line += resetLabel;
    const std::size_t resetW = textWidth(resetLabel);
    if (resetW < maxLabelW) line.append(maxLabelW - resetW, L' ');
    line += L": ";
    line += i18n::tr(i18n::MsgId::ConfigResetTag);
    if (y < out.height - 2) {
      drawBodyLine(out, y, line, Style::Value);
      ++y;
    }
  }

  // Read-only misc info under Timelines.
  auto drawReadonly = [&](const std::wstring& label, const std::wstring& value) {
    if (y >= out.height - 2) return;
    std::wstring line = L"  ";
    line += label;
    const std::size_t labelW = textWidth(label);
    if (labelW < maxLabelW) line.append(maxLabelW - labelW, L' ');
    line += L": ";
    line += value;
    drawBodyLine(out, y, line, Style::Value);
    ++y;
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
  drawReadonly(std::wstring(i18n::tr(i18n::MsgId::ConfigReadonlyValueColor)), L"white (default)");
  drawReadonly(std::wstring(i18n::tr(i18n::MsgId::ConfigReadonlyMetricNameColor)), L"light blue (cyan)");

  if (out.height >= 3) {
    drawBodyLine(out, out.height - 2, std::wstring(i18n::tr(i18n::MsgId::ConfigFooterKeys)), Style::FooterKey);
  }
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

  const int yStart = bodyTop + 1;
  const int yEnd = out.height - 2;
  const int valueCol = std::min(out.width - 1, static_cast<int>(maxKeyLen) + 2);

  int y = yStart;
  for (const auto& it : items) {
    if (y >= yEnd) break;
    drawBodyLine(out, y, widenAscii(it.key), Style::Section);
    if (!it.value.empty() && valueCol < out.width) {
      drawText(out, valueCol, y, widenAscii(it.value), Style::Value);
    }
    ++y;
  }

  if (out.height >= 3) {
    drawBodyLine(out, out.height - 2, std::wstring(i18n::tr(i18n::MsgId::HardwareFooterKeys)), Style::FooterKey);
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

  int y = bodyTop + 1;

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
      ++y;
    }

    if (y < out.height - 2) {
      std::wstring line = (state.benchmarksSel == 0 ? L"> " : L"  ");
      line += i18n::tr(i18n::MsgId::BenchRunAll);
      drawBodyLine(out, y, line, Style::Warning);
      ++y;
    }

    if (y < out.height - 2) {
      drawBodyLine(out, y, L"", Style::Default);
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

  if (out.height >= 4) {
    drawBodyLine(out, out.height - 3, std::wstring(i18n::tr(i18n::MsgId::BenchFooterKeys)), Style::FooterKey);
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
      std::size_t labelEnd = labelStart;
      while (labelEnd < footer.size() && footer[labelEnd] != L' ') {
        ++labelEnd;
      }

      for (std::size_t k = labelStart; k < labelEnd; ++k) {
        st[k] = Style::FooterBlock;
      }

      for (std::size_t k = labelStart; k < labelEnd; ++k) {
        if (footer[k] == L'a' || footer[k] == L'A') {
          st[k] = Style::FooterHot;
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

      // Label ends at the next space (tokens are space-separated).
      std::size_t labelEnd = labelStart;
      while (labelEnd < footer.size()) {
        const wchar_t ch = footer[labelEnd];
        if (ch == L' ') break;
        ++labelEnd;
      }

      for (std::size_t k = labelStart; k < labelEnd; ++k) {
        st[k] = Style::FooterBlock;
      }

      // Hot letter inside the label word itself.
      int fnum = 0;
      for (std::size_t p = i + 1; p < j; ++p) {
        fnum = (fnum * 10) + static_cast<int>(footer[p] - L'0');
      }
      wchar_t hot = 0;
      switch (fnum) {
        case 1: hot = L'H'; break;   // Help
        case 2: hot = L'W'; break;   // Hardware (the 'w' in HardWare)
        case 3: hot = L'B'; break;   // Bench
        case 4: hot = L'C'; break;   // Config
        case 5: hot = L'M'; break;   // Minimal
        case 10: hot = L'Q'; break;  // Quit
        default: break;
      }

      if (hot != 0) {
        for (std::size_t k = labelStart; k < labelEnd; ++k) {
          const wchar_t ch = footer[k];
          if (ch == hot || ch == static_cast<wchar_t>(std::towlower(static_cast<wint_t>(hot))) ||
              ch == static_cast<wchar_t>(std::towupper(static_cast<wint_t>(hot)))) {
            st[k] = Style::FooterHot;
            break;
          }
        }
      }

      i = (j > 0) ? (j - 1) : i;
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
      drawText(out, 0, bodyTop, std::wstring(i18n::tr(i18n::MsgId::ScreenHelpTitle)), Style::Section);
      renderHelp(out, bodyTop);
      break;
    case Screen::Config:
      drawText(out, 0, bodyTop, std::wstring(i18n::tr(i18n::MsgId::ScreenConfigTitle)), Style::Section);
      renderConfig(out, bodyTop, cfg, state);
      break;
    case Screen::Hardware:
      drawText(out, 0, bodyTop, std::wstring(i18n::tr(i18n::MsgId::ScreenHardwareTitle)), Style::Section);
      renderHardware(out, bodyTop, state);
      break;
    case Screen::Benchmarks:
      drawText(out, 0, bodyTop, std::wstring(i18n::tr(i18n::MsgId::ScreenBenchmarksTitle)), Style::Section);
      renderBenchmarks(out, bodyTop, state);
      break;
    default:
      drawText(out, 0, bodyTop + 1, L"(Screen not yet ported to shared core)", Style::Value);
      break;
  }
}

}  // namespace aiz
