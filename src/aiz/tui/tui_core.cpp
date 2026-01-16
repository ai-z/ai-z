#include <aiz/tui/tui_core.h>

#include <aiz/bench/bench.h>
#include <aiz/hw/hardware_info.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace aiz {

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
    ++cx;
  }
}

static void fillLine(Frame& f, int y, wchar_t ch, Style style) {
  if (y < 0 || y >= f.height) return;
  for (int x = 0; x < f.width; ++x) {
    auto& c = f.at(x, y);
    c.ch = ch;
    c.style = static_cast<std::uint16_t>(style);
  }
}

void applyCommand(TuiState& state, const Config& /*cfg*/, Command cmd) {
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
    case Command::NavTimelines:
      state.screen = Screen::Timelines;
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
    if (cmd == Command::Up) state.benchmarksSel = std::max(0, state.benchmarksSel - 1);
    if (cmd == Command::Down) state.benchmarksSel = std::min(maxSel, state.benchmarksSel + 1);
    return;
  }

  if (state.screen == Screen::Config) {
    if (cmd == Command::Up) state.configSel = std::max(0, state.configSel - 1);
    if (cmd == Command::Down) state.configSel = std::min(6, state.configSel + 1);
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
      c.style = static_cast<std::uint16_t>(Style::Value);
    }
  }
}

static void renderTimelines(Frame& out, int /*bodyTop*/, const TuiState& state, const Config& cfg) {
  // This is an incremental port: start with the title bar numbers, then add scrolling graphs.
  fillLine(out, 0, L' ', Style::Header);

  std::string title = "AI-Z  ";

  title += "CPU ";
  title += state.latest.cpu ? (fmt0(state.latest.cpu->value) + "%") : std::string("--");
  title += "  ";

  title += "RAM ";
  title += !state.latest.ramText.empty() ? state.latest.ramText : std::string("--");
  title += "  ";

  title += "VRAM ";
  title += state.latest.vramPct ? (fmt0(state.latest.vramPct->value) + "%") : std::string("--");
  title += "  ";

  title += "Disk ";
  if (state.latest.disk) {
    title += fmt1(state.latest.disk->value);
    title += state.latest.disk->unit;
  } else {
    title += "--";
  }
  title += "  ";

  title += "PCIeRX ";
  if (state.latest.pcieRx) {
    title += fmt0(state.latest.pcieRx->value);
    title += state.latest.pcieRx->unit;
  } else {
    title += "--";
  }
  title += "  ";

  title += "PCIeTX ";
  if (state.latest.pcieTx) {
    title += fmt0(state.latest.pcieTx->value);
    title += state.latest.pcieTx->unit;
  } else {
    title += "--";
  }
  title += "  ";

  title += "t=";
  title += std::to_string(state.tick);

  drawText(out, 0, 0, widenAscii(title), Style::Header);

  // Layout matches ncurses: start under a 2-row header, reserve 1 row footer.
  const int top = 2;
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
  };

  std::vector<Panel> panels;
  panels.push_back(Panel{"CPU", cfg.showCpu, &state.latest.cpu, &state.cpuTl, nullptr, 100.0, false});
  panels.push_back(Panel{"RAM", cfg.showRam, &state.latest.ramPct, &state.ramTl, nullptr, 100.0, false});
  panels.push_back(Panel{"VRAM", cfg.showVram, &state.latest.vramPct, &state.vramTl, nullptr, 100.0, false});
  panels.push_back(Panel{"Disk", cfg.showDisk, &state.latest.disk, &state.diskTl, nullptr, 5000.0, false});
  // PCIe samples are in MB/s (from NVML). Render RX and TX as two stacked mini-graphs.
  panels.push_back(Panel{"PCIe (RX/TX)", (cfg.showPcieRx || cfg.showPcieTx), &state.latest.pcieRx, &state.pcieRxTl, &state.pcieTxTl, 32'000.0, true});

  panels.erase(
      std::remove_if(panels.begin(), panels.end(), [](const Panel& p) { return !p.enabled; }),
      panels.end());

  if (panels.empty()) {
    drawBodyLine(out, 2, L"No timelines enabled. Use Config (F4 / C) to enable.", Style::Value);
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

    std::string section = p.name;
    if (p.sample && p.sample->has_value()) {
      section += ": ";
      section += fmt1((*p.sample)->value);
      section += " ";
      section += (*p.sample)->unit;
      if (!(*p.sample)->label.empty()) {
        section += " (" + (*p.sample)->label + ")";
      }
    } else {
      section += ": unavailable";
    }

    // Special-case title for two-line PCIe: show both RX and TX values.
    if (p.twoLine && p.tl && p.tl2) {
      std::string pcieTitle = p.name;
      pcieTitle += ": ";
      if (cfg.showPcieRx && state.latest.pcieRx) {
        pcieTitle += "RX ";
        pcieTitle += fmt1(state.latest.pcieRx->value);
        pcieTitle += " ";
        pcieTitle += state.latest.pcieRx->unit;
      } else {
        pcieTitle += "RX --";
      }
      pcieTitle += "  ";
      if (cfg.showPcieTx && state.latest.pcieTx) {
        pcieTitle += "TX ";
        pcieTitle += fmt1(state.latest.pcieTx->value);
        pcieTitle += " ";
        pcieTitle += state.latest.pcieTx->unit;
      } else {
        pcieTitle += "TX --";
      }
      drawSectionTitleLine(out, y, pcieTitle);
    } else {
      drawSectionTitleLine(out, y, section);
    }

    const int graphTop = y + 1;
    const int graphH = std::max(0, height - 1);
    if (graphH > 0 && p.tl) {
      if (p.twoLine && p.tl2 && graphH >= 2) {
        // Split available height into two sub-graphs (top=RX, bottom=TX).
        const int rxH = std::max(1, graphH / 2);
        const int txH = std::max(1, graphH - rxH);
        if (cfg.showPcieRx) {
          drawScrollingBars(out, graphTop, 0, rxH, out.width, *p.tl, p.maxV, cfg.timelineAgg);
        }
        if (cfg.showPcieTx) {
          drawScrollingBars(out, graphTop + rxH, 0, txH, out.width, *p.tl2, p.maxV, cfg.timelineAgg);
        }
      } else {
        drawScrollingBars(out, graphTop, 0, graphH, out.width, *p.tl, p.maxV, cfg.timelineAgg);
      }
    }

    y += height;
  }
}

static void renderHelp(Frame& out, int bodyTop) {
  int y = bodyTop + 1;
  const std::vector<std::string> lines = {
      "ai-z",  // version/website will be added when shared core has access to version.h
      "",
      "Description:",
      "  C++ TUI for performance timelines and benchmarks.",
      "  Timelines: CPU, RAM, VRAM, GPU, Disk bandwidth, PCIe RX/TX.",
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
      "  F10 Quit        (Q)",
      "  Esc Back to timelines",
  };

  for (const auto& line : lines) {
    if (y >= out.height - 2) break;
    drawBodyLine(out, y, widenAscii(line), Style::Value);
    ++y;
  }

  if (out.height >= 3) {
    drawBodyLine(out, out.height - 2, L"Esc: back", Style::FooterKey);
  }
}

static void renderConfig(Frame& out, int bodyTop, const Config& cfg, const TuiState& state) {
  struct Item {
    const wchar_t* label;
    bool value;
  };
  const Item items[] = {
      {L"Show CPU usage", cfg.showCpu},
      {L"Show GPU usage", cfg.showGpu},
      {L"Show Disk bandwidth", cfg.showDisk},
      {L"Show PCIe RX", cfg.showPcieRx},
      {L"Show PCIe TX", cfg.showPcieTx},
      {L"Show RAM usage", cfg.showRam},
      {L"Show VRAM usage", cfg.showVram},
  };

  int y = bodyTop + 1;
  constexpr int kCount = static_cast<int>(sizeof(items) / sizeof(items[0]));
  for (int i = 0; i < kCount; ++i) {
    std::wstring line = (i == state.configSel ? L"> " : L"  ");
    line += items[i].label;
    line += L": ";
    line += (items[i].value ? L"ON" : L"OFF");
    if (y >= out.height - 2) break;
    drawBodyLine(out, y, line, Style::Value);
    ++y;
  }

  if (out.height >= 3) {
    drawBodyLine(out, out.height - 2, L"Space: toggle   s: save   Esc: back", Style::FooterKey);
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
    drawBodyLine(out, out.height - 2, L"r: refresh   Esc: back", Style::FooterKey);
  }
}

static void renderBenchmarks(Frame& out, int bodyTop, const TuiState& state) {
  int y = bodyTop + 1;
  // Two-column layout: left = benchmark names, right = results.
  std::size_t maxNameLen = 0;
  for (const auto& b : state.benches) {
    if (!b) continue;
    std::string n = b->name();
    if (!b->isAvailable()) n += " [unavailable]";
    maxNameLen = std::max(maxNameLen, n.size());
  }
  const int minValueCol = 24;
  const int maxValueCol = std::max(0, out.width / 2);
  const int valueCol = std::min(out.width - 1, std::max(minValueCol, std::min(maxValueCol, static_cast<int>(maxNameLen) + 4)));

  // Inside results column, align "label: value" pairs.
  std::size_t maxLabelLen = 0;
  for (const auto& r : state.benchResults) {
    std::size_t start = 0;
    while (start <= r.size()) {
      const std::size_t end = r.find('\n', start);
      const std::string line = (end == std::string::npos) ? r.substr(start) : r.substr(start, end - start);
      const std::size_t sep = line.find(": ");
      if (sep != std::string::npos) maxLabelLen = std::max(maxLabelLen, sep);
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  maxLabelLen = std::min<std::size_t>(maxLabelLen, 8);
  constexpr int kResultIndent = 4;
  const int labelCol = std::min(out.width - 1, valueCol + kResultIndent);
  const int valCol = std::min(out.width - 1, labelCol + static_cast<int>(maxLabelLen) + 2);

  // Special entry: run all.
  {
    std::wstring line = (state.benchmarksSel == 0 ? L"> " : L"  ");
    line += L"Run all benchmarks";
    if (y < out.height - 2) {
      drawBodyLine(out, y, line, Style::Value);
      ++y;
    }

    // Blank spacer line.
    if (y < out.height - 2) {
      drawBodyLine(out, y, L"", Style::Value);
      ++y;
    }
  }

  for (int i = 0; i < static_cast<int>(state.benches.size()); ++i) {
    const int rowIndex = i + 1;  // shifted by run-all
    const auto& b = state.benches[static_cast<std::size_t>(i)];

    const std::wstring prefix = (rowIndex == state.benchmarksSel ? L"> " : L"  ");
    const std::string name = b ? b->name() : std::string("(null)");
    const bool avail = b && b->isAvailable();
    const std::string suffix = (b && !avail) ? " [unavailable]" : std::string{};

    if (y < out.height - 2) {
      // Clear line.
      for (int x = 0; x < out.width; ++x) {
        auto& c = out.at(x, y);
        c.ch = L' ';
        c.style = static_cast<std::uint16_t>(Style::Default);
      }

      // Left column: benchmark name.
      drawText(out, 0, y, prefix, Style::Value);
      drawText(out, static_cast<int>(prefix.size()), y, widenAscii(name), avail ? Style::Section : Style::Value);
      if (!suffix.empty()) {
        const int sx = static_cast<int>(prefix.size() + name.size());
        drawText(out, sx, y, widenAscii(suffix), Style::Value);
      }

      // Right column: results.
      if (i >= 0 && i < static_cast<int>(state.benchResults.size()) && !state.benchResults[static_cast<std::size_t>(i)].empty()) {
        std::string r = state.benchResults[static_cast<std::size_t>(i)];
        std::size_t start = 0;
        int ry = y;
        while (start <= r.size() && ry < out.height - 2) {
          const std::size_t end = r.find('\n', start);
          const std::string line = (end == std::string::npos) ? r.substr(start) : r.substr(start, end - start);

          if (ry != y) {
            for (int x = 0; x < out.width; ++x) {
              auto& c = out.at(x, ry);
              c.ch = L' ';
              c.style = static_cast<std::uint16_t>(Style::Default);
            }
          }

          const std::size_t sep = line.find(": ");
          if (sep != std::string::npos) {
            const std::string label = line.substr(0, sep + 1);
            const std::string val = line.substr(sep + 2);
            if (labelCol < out.width) drawText(out, labelCol, ry, widenAscii(label), Style::Value);
            if (valCol < out.width) drawText(out, valCol, ry, widenAscii(val), Style::Value);
          } else {
            if (labelCol < out.width) drawText(out, labelCol, ry, widenAscii(line), Style::Value);
          }

          if (end == std::string::npos) break;
          start = end + 1;
          ++ry;
        }
        y = ry;
      }

      ++y;
    }
  }

  if (out.height >= 4) {
    drawBodyLine(out, out.height - 3, L"Enter: run   Esc: back", Style::FooterKey);
  }
  if (out.height >= 3) {
    drawBodyLine(out, out.height - 2, widenAscii(state.lastBenchResult), Style::Value);
  }
}

void renderFrame(Frame& out, const Viewport& vp, const TuiState& state, const Config& cfg, bool /*debugMode*/) {
  out.resize(vp.width, vp.height);
  out.clear(Cell{L' ', static_cast<std::uint16_t>(Style::Default)});

  if (out.width <= 0 || out.height <= 0) return;

  // Header (Timelines overrides to show telemetry).
  fillLine(out, 0, L' ', Style::Header);
  drawText(out, 0, 0, L" AI-Z ", Style::Header);

  // Footer
  if (out.height >= 2) {
    const int y = out.height - 1;
    fillLine(out, y, L' ', Style::FooterKey);
    std::wstring footer;
    // Debug: show current screen + tick.
    const wchar_t* screenName = L"?";
    switch (state.screen) {
      case Screen::Timelines:
        screenName = L"Timelines";
        break;
      case Screen::Help:
        screenName = L"Help";
        break;
      case Screen::Hardware:
        screenName = L"Hardware";
        break;
      case Screen::Benchmarks:
        screenName = L"Benchmarks";
        break;
      case Screen::Config:
        screenName = L"Config";
        break;
    }
    footer += L"screen=";
    footer += screenName;
    footer += L" tick=";
    footer += widenAscii(std::to_string(state.tick));
    footer += L"  ";

    footer += L"F1 Help  F2 Hardware  F3 Bench  F4 Config  F5 Timelines  F10 Quit";
    drawText(out, 0, y, footer, Style::FooterKey);
  }

  // Body
  const int bodyTop = 1;

  auto titleFor = [](Screen s) {
    switch (s) {
      case Screen::Timelines:
        return L"Timelines";
      case Screen::Help:
        return L"Help";
      case Screen::Hardware:
        return L"Hardware";
      case Screen::Benchmarks:
        return L"Benchmarks";
      case Screen::Config:
        return L"Config";
    }
    return L"";
  };

  drawText(out, 0, bodyTop, std::wstring(L"AI-Z - ") + titleFor(state.screen), Style::Section);

  switch (state.screen) {
    case Screen::Timelines:
      renderTimelines(out, bodyTop, state, cfg);
      break;
    case Screen::Help:
      renderHelp(out, bodyTop);
      break;
    case Screen::Config:
      renderConfig(out, bodyTop, cfg, state);
      break;
    case Screen::Hardware:
      renderHardware(out, bodyTop, state);
      break;
    case Screen::Benchmarks:
      renderBenchmarks(out, bodyTop, state);
      break;
    default:
      drawText(out, 0, bodyTop + 1, L"(Screen not yet ported to shared core)", Style::Value);
      break;
  }
}

}  // namespace aiz
