#include <aiz/tui/win32_terminal_ui.h>

#include <aiz/tui/tui_core.h>

#include <aiz/hw/hardware_info.h>

#include <aiz/metrics/cpu_usage.h>
#include <aiz/metrics/disk_bandwidth.h>
#include <aiz/metrics/network_bandwidth.h>
#include <aiz/metrics/nvidia_nvml.h>
#include <aiz/metrics/pcie_bandwidth.h>
#include <aiz/metrics/ram_usage.h>

#include <aiz/bench/factory.h>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>

namespace aiz {

#if defined(_WIN32)
namespace {

struct ConsoleModeGuard {
  HANDLE hIn = INVALID_HANDLE_VALUE;
  HANDLE hOut = INVALID_HANDLE_VALUE;
  DWORD inMode = 0;
  DWORD outMode = 0;
  bool active = false;

  bool init() {
    hIn = GetStdHandle(STD_INPUT_HANDLE);
    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hOut == INVALID_HANDLE_VALUE) return false;
    if (!GetConsoleMode(hIn, &inMode)) return false;
    if (!GetConsoleMode(hOut, &outMode)) return false;

    DWORD newIn = inMode;
    newIn |= ENABLE_WINDOW_INPUT;
    newIn |= ENABLE_EXTENDED_FLAGS;
    newIn &= ~ENABLE_QUICK_EDIT_MODE;
    newIn &= ~ENABLE_LINE_INPUT;
    newIn &= ~ENABLE_ECHO_INPUT;

    (void)SetConsoleMode(hIn, newIn);

    DWORD newOut = outMode;
    newOut |= ENABLE_PROCESSED_OUTPUT;
    newOut |= ENABLE_WRAP_AT_EOL_OUTPUT;
    (void)SetConsoleMode(hOut, newOut);

    active = true;
    return true;
  }

  ~ConsoleModeGuard() {
    if (!active) return;
    (void)SetConsoleMode(hIn, inMode);
    (void)SetConsoleMode(hOut, outMode);
  }
};

static void restoreInteractiveInput(HANDLE hIn) {
  if (hIn == INVALID_HANDLE_VALUE) return;
  DWORD mode = 0;
  if (!GetConsoleMode(hIn, &mode)) return;
  mode |= ENABLE_PROCESSED_INPUT;
  mode |= ENABLE_LINE_INPUT;
  mode |= ENABLE_ECHO_INPUT;
  // Leave QUICK_EDIT as it was originally; the guard restores exact mode anyway.
  (void)SetConsoleMode(hIn, mode);
}

static void showCursor(HANDLE hOut) {
  if (hOut == INVALID_HANDLE_VALUE) return;
  CONSOLE_CURSOR_INFO cci{};
  if (GetConsoleCursorInfo(hOut, &cci)) {
    cci.bVisible = TRUE;
    (void)SetConsoleCursorInfo(hOut, &cci);
  }
}

static void clearScreen(HANDLE hOut) {
  if (hOut == INVALID_HANDLE_VALUE) return;

  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(hOut, &info)) return;

  // Clear the active buffer (entire buffer, not just visible window).
  const DWORD cellCount = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
  COORD home{0, 0};
  DWORD written = 0;
  (void)FillConsoleOutputCharacterW(hOut, L' ', cellCount, home, &written);
  (void)FillConsoleOutputAttribute(hOut, info.wAttributes, cellCount, home, &written);
  (void)SetConsoleCursorPosition(hOut, home);
}

static void flushConsoleInput(HANDLE hIn) {
  if (hIn == INVALID_HANDLE_VALUE) return;
  // Discard buffered key events so the parent shell doesn't receive stray arrows.
  (void)FlushConsoleInputBuffer(hIn);
}

static Viewport currentViewport(HANDLE hOut) {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(hOut, &info)) return {};
  const int w = info.srWindow.Right - info.srWindow.Left + 1;
  const int h = info.srWindow.Bottom - info.srWindow.Top + 1;
  return {w, h};
}

static WORD styleToAttr(std::uint16_t style) {
  const auto s = static_cast<Style>(style);
  switch (s) {
    case Style::Header:
      return static_cast<WORD>(BACKGROUND_GREEN | BACKGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    case Style::FooterKey:
      return static_cast<WORD>(BACKGROUND_BLUE | BACKGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    case Style::Section:
      return static_cast<WORD>(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    case Style::Value:
      return static_cast<WORD>(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    case Style::Warning:
      return static_cast<WORD>(FOREGROUND_RED | FOREGROUND_INTENSITY);
    default:
      return static_cast<WORD>(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
  }
}

static void present(HANDLE hOut, const Frame& f) {
  if (f.width <= 0 || f.height <= 0) return;

  std::vector<CHAR_INFO> buf;
  buf.resize(static_cast<std::size_t>(f.width * f.height));

  for (int y = 0; y < f.height; ++y) {
    for (int x = 0; x < f.width; ++x) {
      const auto& c = f.at(x, y);
      CHAR_INFO ci{};
      ci.Char.UnicodeChar = c.ch;
      ci.Attributes = styleToAttr(c.style);
      buf[static_cast<std::size_t>(y * f.width + x)] = ci;
    }
  }

  COORD bufSize{static_cast<SHORT>(f.width), static_cast<SHORT>(f.height)};
  COORD bufCoord{0, 0};
  SMALL_RECT region{0, 0, static_cast<SHORT>(f.width - 1), static_cast<SHORT>(f.height - 1)};

  (void)WriteConsoleOutputW(hOut, buf.data(), bufSize, bufCoord, &region);
}

static bool isVsCodeTerminal() {
  const char* termProgram = std::getenv("TERM_PROGRAM");
  if (termProgram && std::string(termProgram) == "vscode") return true;
  // Fallback: VS Code sets a few other env vars depending on shell.
  if (std::getenv("VSCODE_PID") != nullptr) return true;
  if (std::getenv("VSCODE_GIT_ASKPASS_NODE") != nullptr) return true;
  return false;
}

static bool enableVtIfPossible(HANDLE hOut) {
  if (hOut == INVALID_HANDLE_VALUE) return false;
  DWORD mode = 0;
  if (!GetConsoleMode(hOut, &mode)) return false;
  DWORD newMode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
  if (!SetConsoleMode(hOut, newMode)) return false;
  return true;
}

static void setUtf8CodePages() {
  // For conpty/VS Code terminal, writing UTF-8 through WriteConsoleA works well
  // when the console output code page is UTF-8.
  (void)SetConsoleOutputCP(CP_UTF8);
  (void)SetConsoleCP(CP_UTF8);
}

static void appendUtf8(std::string& out, wchar_t wc) {
  // Convert a single UTF-16 code unit (Windows wchar_t) to UTF-8.
  // Our UI glyphs are in BMP (block elements), so this is sufficient.
  if (wc <= 0x7F) {
    out.push_back(static_cast<char>(wc));
  } else if (wc <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | ((wc >> 6) & 0x1F)));
    out.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xE0 | ((wc >> 12) & 0x0F)));
    out.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
  }
}

static void presentAnsi(HANDLE hOut, const Frame& f) {
  if (hOut == INVALID_HANDLE_VALUE) return;
  if (f.width <= 0 || f.height <= 0) return;

  DWORD written = 0;
  // Move cursor home; avoid full clear to reduce flicker under conpty.
  (void)WriteConsoleA(hOut, "\x1b[H", 3, &written, nullptr);

  auto styleToSgr = [](std::uint16_t style) -> const char* {
    const auto s = static_cast<Style>(style);
    switch (s) {
      case Style::Header:
        return "\x1b[30;42;1m";  // black on bright green
      case Style::FooterKey:
        return "\x1b[37;44;1m";  // white on bright blue
      case Style::Section:
        return "\x1b[32;1m";  // bright green
      case Style::Warning:
        return "\x1b[31;1m";  // bright red
      case Style::Value:
      default:
        return "\x1b[0m";
    }
  };

  std::string out;
  out.reserve(static_cast<std::size_t>(f.width * f.height) + static_cast<std::size_t>(f.height * 8));

  std::uint16_t lastStyle = static_cast<std::uint16_t>(Style::Default);
  out += styleToSgr(lastStyle);

  for (int y = 0; y < f.height; ++y) {
    // Ensure we're at column 1 of the current line, then clear it.
    out += "\r\x1b[2K";
    for (int x = 0; x < f.width; ++x) {
      const auto& c = f.at(x, y);
      if (c.style != lastStyle) {
        lastStyle = c.style;
        out += styleToSgr(lastStyle);
      }
      appendUtf8(out, c.ch);
    }
    out += "\x1b[0m";
    if (y != f.height - 1) out += "\r\n";
    lastStyle = static_cast<std::uint16_t>(Style::Default);
  }

  // Keep the cursor inside the drawn region to avoid terminal scrolling.
  out += "\r";

  (void)WriteConsoleA(hOut, out.c_str(), static_cast<DWORD>(out.size()), &written, nullptr);
}

static void presentAnsiSpanDiff(HANDLE hOut, const Frame& f, Frame& prev) {
  if (hOut == INVALID_HANDLE_VALUE) return;
  if (f.width <= 0 || f.height <= 0) return;

  // If size changed, force a full repaint once.
  const bool sizeChanged = (prev.width != f.width) || (prev.height != f.height);
  if (sizeChanged) {
    prev.resize(f.width, f.height);
    prev.clear();
    presentAnsi(hOut, f);
    prev = f;
    return;
  }

  auto styleToSgr = [](std::uint16_t style) -> const char* {
    const auto s = static_cast<Style>(style);
    switch (s) {
      case Style::Header:
        return "\x1b[30;42;1m";
      case Style::FooterKey:
        return "\x1b[37;44;1m";
      case Style::Section:
        return "\x1b[32;1m";
      case Style::Warning:
        return "\x1b[31;1m";
      case Style::Value:
      default:
        return "\x1b[0m";
    }
  };

  auto appendCursorPos = [](std::string& out, int row0, int col0) {
    // 1-based cursor positioning.
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row0 + 1, col0 + 1);
    out += buf;
  };

  std::string out;
  out.reserve(static_cast<std::size_t>(f.width * 4));

  std::uint16_t currentStyle = static_cast<std::uint16_t>(Style::Default);
  out += "\x1b[H";  // keep cursor in a known place
  out += styleToSgr(currentStyle);

  for (int y = 0; y < f.height; ++y) {
    bool rowHadChanges = false;
    int x = 0;
    while (x < f.width) {
      const auto& now = f.at(x, y);
      const auto& was = prev.at(x, y);
      if (now.ch == was.ch && now.style == was.style) {
        ++x;
        continue;
      }

      rowHadChanges = true;

      // Start of a changed span.
      const int start = x;
      int end = x + 1;
      while (end < f.width) {
        const auto& n = f.at(end, y);
        const auto& w = prev.at(end, y);
        if (n.ch == w.ch && n.style == w.style) break;
        ++end;
      }

      appendCursorPos(out, y, start);

      // Emit the span, minimizing SGR changes.
      for (int xi = start; xi < end; ++xi) {
        const auto& c = f.at(xi, y);
        if (c.style != currentStyle) {
          currentStyle = c.style;
          out += styleToSgr(currentStyle);
        }
        appendUtf8(out, c.ch);
      }

      x = end;
    }

    // If anything changed on this row, clear the remainder of the line to avoid
    // stale characters when strings shrink (which can look like panels vanished).
    if (rowHadChanges) {
      appendCursorPos(out, y, f.width);
      out += "\x1b[0K";
    }
  }

  out += "\x1b[0m\r";

  DWORD written = 0;
  (void)WriteConsoleA(hOut, out.c_str(), static_cast<DWORD>(out.size()), &written, nullptr);
  prev = f;
}

static Command mapKey(const KEY_EVENT_RECORD& k) {
  if (!k.bKeyDown) return Command::None;

  switch (k.wVirtualKeyCode) {
    case VK_F1:
      return Command::NavHelp;
    case VK_F2:
      return Command::NavHardware;
    case VK_F3:
      return Command::NavBenchmarks;
    case VK_F4:
      return Command::NavConfig;
    case VK_F5:
      return Command::NavTimelines;
    case VK_F10:
      return Command::Quit;

    case VK_ESCAPE:
      return Command::Back;
    case VK_UP:
      return Command::Up;
    case VK_DOWN:
      return Command::Down;
    case VK_RETURN:
      return Command::Activate;
    case VK_SPACE:
      return Command::Toggle;
  }

  const wchar_t ch = k.uChar.UnicodeChar;
  switch (ch) {
    case L'h':
    case L'H':
      return Command::NavHelp;
    case L'w':
    case L'W':
      return Command::NavHardware;
    case L'b':
    case L'B':
      return Command::NavBenchmarks;
    case L'c':
    case L'C':
      return Command::NavConfig;
    case L'q':
    case L'Q':
      return Command::Quit;
    case L't':
    case L'T':
      return Command::NavTimelines;
    case L's':
    case L'S':
      return Command::Save;
    case L'd':
    case L'D':
      return Command::Defaults;
    case L'r':
    case L'R':
      return Command::Refresh;
  }

  return Command::None;
}

}  // namespace
#endif

int Win32TerminalUi::run(Config& cfg, bool debugMode) {
#if !defined(_WIN32)
  (void)cfg;
  (void)debugMode;
  return 1;
#else
  ConsoleModeGuard modes;
  (void)modes.init();

  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut == INVALID_HANDLE_VALUE) return 1;

  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);

  struct ExitCleanup {
    HANDLE hOut = INVALID_HANDLE_VALUE;
    HANDLE hIn = INVALID_HANDLE_VALUE;
    bool active = false;
    ~ExitCleanup() {
      if (!active) return;
      flushConsoleInput(hIn);
      clearScreen(hOut);
      showCursor(hOut);
      restoreInteractiveInput(hIn);
      // Ensure the next shell prompt appears on a clean line.
      DWORD written = 0;
      (void)WriteConsoleW(hOut, L"\r\n", 2, &written, nullptr);
    }
  } cleanup{hOut, hIn, true};

  // Hide cursor.
  CONSOLE_CURSOR_INFO cci{};
  if (GetConsoleCursorInfo(hOut, &cci)) {
    cci.bVisible = FALSE;
    (void)SetConsoleCursorInfo(hOut, &cci);
  }

  const bool useAnsi = isVsCodeTerminal() && enableVtIfPossible(hOut);
  if (useAnsi) setUtf8CodePages();

  TuiState state;
  Frame frame;
  Frame prevAnsi;

  CpuUsageCollector cpuCollector;
  DiskBandwidthCollector diskCollector;
  DiskBandwidthCollector diskReadCollector(DiskBandwidthMode::Read);
  DiskBandwidthCollector diskWriteCollector(DiskBandwidthMode::Write);
  NetworkBandwidthCollector netRxCollector(NetworkBandwidthMode::Rx);
  NetworkBandwidthCollector netTxCollector(NetworkBandwidthMode::Tx);
  PcieRxBandwidthCollector pcieRxCollector;
  PcieTxBandwidthCollector pcieTxCollector;
  const auto lastSampleAtInit = std::chrono::steady_clock::now();
  (void)lastSampleAtInit;

  // Benchmarks list (shared with ncurses UI).
  state.benches.push_back(makePcieBandwidthBenchmark());
  state.benches.push_back(makeFlopsBenchmark());
  state.benches.push_back(makeTorchMatmulBenchmark());
  state.benches.push_back(makeOrtCpuMatMulBenchmark());
  state.benches.push_back(makeOrtCpuMemoryBandwidthBenchmark());
  state.benches.push_back(makeOrtCudaMatMulBenchmark());
  state.benches.push_back(makeOrtCudaMemoryBandwidthBenchmark());
  state.benches.push_back(makeDirectMLMatMulBenchmark());
  state.benches.push_back(makeDirectMLMemoryBandwidthBenchmark());
  state.benchResults.resize(state.benches.size());

  std::atomic<bool> benchStop{false};
  std::thread benchThread;

  auto benchIsRunning = [&]() -> bool {
    std::lock_guard<std::mutex> lk(state.benchMutex);
    return state.benchmarksRunning;
  };

  auto tryJoinBenchThread = [&]() {
    if (!benchThread.joinable()) return;
    if (benchIsRunning()) return;
    benchThread.join();
  };

  auto startBenchThread = [&](bool runAll, int singleIndex) {
    // Ensure we don't leak a completed thread object.
    tryJoinBenchThread();
    if (benchThread.joinable()) return;  // still running

    {
      std::lock_guard<std::mutex> lk(state.benchMutex);
      if (state.benchmarksRunning) return;
      state.benchmarksRunning = true;
      state.runningBenchIndex = -1;
      state.lastBenchResult = runAll ? "Running all..." : "Running...";
    }

    benchStop.store(false);
    benchThread = std::thread([&, runAll, singleIndex]() {
      auto runOne = [&](int i) {
        if (i < 0 || i >= static_cast<int>(state.benches.size())) return;

        {
          std::lock_guard<std::mutex> lk(state.benchMutex);
          state.runningBenchIndex = i;
        }

        auto& b = state.benches[static_cast<std::size_t>(i)];
        const BenchResult r = b ? b->run() : BenchResult{false, "(null benchmark)"};

        {
          std::lock_guard<std::mutex> lk(state.benchMutex);
          if (static_cast<std::size_t>(i) < state.benchResults.size()) {
            state.benchResults[static_cast<std::size_t>(i)] = r.ok ? r.summary : ("FAIL: " + r.summary);
          }
          state.lastBenchResult = r.ok ? ("OK: " + r.summary) : ("FAIL: " + r.summary);
        }
      };

      if (runAll) {
        for (int i = 0; i < static_cast<int>(state.benches.size()); ++i) {
          if (benchStop.load()) break;
          runOne(i);
        }
        std::lock_guard<std::mutex> lk(state.benchMutex);
        state.lastBenchResult = benchStop.load() ? "Stopped." : "Done.";
      } else {
        runOne(singleIndex);
      }

      std::lock_guard<std::mutex> lk(state.benchMutex);
      state.runningBenchIndex = -1;
      state.benchmarksRunning = false;
    });
  };

  while (true) {
    // Join completed benchmark worker thread (if any).
    tryJoinBenchThread();

    // Drain pending input events.
    DWORD nEvents = 0;
    if (hIn != INVALID_HANDLE_VALUE && GetNumberOfConsoleInputEvents(hIn, &nEvents) && nEvents > 0) {
      std::vector<INPUT_RECORD> events;
      events.resize(static_cast<std::size_t>(nEvents));
      DWORD read = 0;
      if (ReadConsoleInputW(hIn, events.data(), nEvents, &read)) {
        for (DWORD i = 0; i < read; ++i) {
          const auto& e = events[static_cast<std::size_t>(i)];
          if (e.EventType == KEY_EVENT) {
            const Command cmd = mapKey(e.Event.KeyEvent);
            if (cmd == Command::Quit) {
              benchStop.store(true);
              if (benchThread.joinable()) benchThread.join();
              return 0;
            }
            if (cmd != Command::None) {
              if (cmd == Command::Refresh && state.screen == Screen::Hardware) {
                state.hardwareDirty = true;
              }

              // Apply config actions directly for now.
              if (state.screen == Screen::Config) {
                if (cmd == Command::Defaults) {
                  cfg = Config{};
                }
                if (cmd == Command::Toggle || cmd == Command::Activate) {
                  switch (state.configSel) {
                    case 0:
                      cfg.showCpu = !cfg.showCpu;
                      break;
                    case 1:
                      cfg.showGpu = !cfg.showGpu;
                      break;
                    case 2:
                      cfg.showDiskRead = !cfg.showDiskRead;
                      break;
                    case 3:
                      cfg.showDiskWrite = !cfg.showDiskWrite;
                      break;
                    case 4:
                      cfg.showNetRx = !cfg.showNetRx;
                      break;
                    case 5:
                      cfg.showNetTx = !cfg.showNetTx;
                      break;
                    case 6:
                      cfg.showPcieRx = !cfg.showPcieRx;
                      break;
                    case 7:
                      cfg.showPcieTx = !cfg.showPcieTx;
                      break;
                    case 8:
                      cfg.showRam = !cfg.showRam;
                      break;
                    case 9:
                      cfg.showVram = !cfg.showVram;
                      break;
                    case 10:
                      cfg = Config{};
                      break;
                    default:
                      break;
                  }
                }
                if (cmd == Command::Save) {
                  cfg.save();
                }
              }

              if (state.screen == Screen::Benchmarks && cmd == Command::Activate) {
                // Do not block the UI thread; run in the background.
                const int sel = state.benchmarksSel;
                if (sel == 0) {
                  startBenchThread(true, -1);
                } else {
                  const int benchIndex = sel - 1;
                  startBenchThread(false, benchIndex);
                }
              }

              applyCommand(state, cfg, cmd);
            }
          }
        }
      }
    }

    if (state.screen == Screen::Hardware && state.hardwareDirty) {
      state.hardwareLines = HardwareInfo::probe().toLines();
      state.hardwareDirty = false;
    }

    // Sample telemetry at a modest rate (keeps PDH and CPU collector happy).
    // We keep this very simple for now; timelines graphs will use history later.
    ++state.tick;
    state.latest.cpu = cpuCollector.sample();

    state.latest.disk.reset();
    if (cfg.showDiskRead) state.latest.diskRead = diskReadCollector.sample();
    else state.latest.diskRead.reset();
    if (cfg.showDiskWrite) state.latest.diskWrite = diskWriteCollector.sample();
    else state.latest.diskWrite.reset();

    if (cfg.showNetRx) state.latest.netRx = netRxCollector.sample();
    else state.latest.netRx.reset();
    if (cfg.showNetTx) state.latest.netTx = netTxCollector.sample();
    else state.latest.netTx.reset();

    // PCIe is best-effort (NVML-only right now).
    state.latest.pcieRx = pcieRxCollector.sample();
    state.latest.pcieTx = pcieTxCollector.sample();

    if (const auto ram = readRamUsage()) {
      // Match the ncurses title style roughly: "used/totalG(pct%)".
      char buf[128]{};
      std::snprintf(buf, sizeof(buf), "%.1f/%.1fG(%.0f%%)", ram->usedGiB, ram->totalGiB, ram->usedPct);
      state.latest.ramText = buf;

      state.latest.ramPct = Sample{ram->usedPct, "%", "os"};
    } else {
      state.latest.ramText.clear();
      state.latest.ramPct.reset();
    }

    // Aggregate VRAM% via NVML when available.
    if (const auto nv = readNvmlTelemetry(); nv && nv->memTotalGiB > 0.0) {
      const double pct = 100.0 * nv->memUsedGiB / nv->memTotalGiB;
      state.latest.vramPct = Sample{pct, "%", "nvml"};
    } else {
      state.latest.vramPct.reset();
    }

    const Viewport vp = currentViewport(hOut);

    // Keep timeline capacity large enough for the viewport width so scrolling bars can fill the screen.
    // Timeline is a ring; we rebuild only if we need more capacity.
    if (vp.width > 0) {
      const std::size_t want = static_cast<std::size_t>(std::max(240, vp.width));
      if (state.cpuTl.capacity() < want) {
        Timeline resized(want);
        for (double v : state.cpuTl.values()) resized.push(v);
        state.cpuTl = std::move(resized);
      }
      if (state.ramTl.capacity() < want) {
        Timeline resized(want);
        for (double v : state.ramTl.values()) resized.push(v);
        state.ramTl = std::move(resized);
      }
      if (state.vramTl.capacity() < want) {
        Timeline resized(want);
        for (double v : state.vramTl.values()) resized.push(v);
        state.vramTl = std::move(resized);
      }
      if (state.diskTl.capacity() < want) {
        Timeline resized(want);
        for (double v : state.diskTl.values()) resized.push(v);
        state.diskTl = std::move(resized);
      }
      if (state.diskReadTl.capacity() < want) {
        Timeline resized(want);
        for (double v : state.diskReadTl.values()) resized.push(v);
        state.diskReadTl = std::move(resized);
      }
      if (state.diskWriteTl.capacity() < want) {
        Timeline resized(want);
        for (double v : state.diskWriteTl.values()) resized.push(v);
        state.diskWriteTl = std::move(resized);
      }
      if (state.netRxTl.capacity() < want) {
        Timeline resized(want);
        for (double v : state.netRxTl.values()) resized.push(v);
        state.netRxTl = std::move(resized);
      }
      if (state.netTxTl.capacity() < want) {
        Timeline resized(want);
        for (double v : state.netTxTl.values()) resized.push(v);
        state.netTxTl = std::move(resized);
      }
      if (state.pcieRxTl.capacity() < want) {
        Timeline resized(want);
        for (double v : state.pcieRxTl.values()) resized.push(v);
        state.pcieRxTl = std::move(resized);
      }
      if (state.pcieTxTl.capacity() < want) {
        Timeline resized(want);
        for (double v : state.pcieTxTl.values()) resized.push(v);
        state.pcieTxTl = std::move(resized);
      }
    }

    // Push current samples into history.
    if (state.latest.cpu) state.cpuTl.push(state.latest.cpu->value);
    if (state.latest.ramPct) state.ramTl.push(state.latest.ramPct->value);
    if (state.latest.vramPct) state.vramTl.push(state.latest.vramPct->value);
    if (state.latest.disk) state.diskTl.push(state.latest.disk->value);
    if (state.latest.diskRead) state.diskReadTl.push(state.latest.diskRead->value);
    if (state.latest.diskWrite) state.diskWriteTl.push(state.latest.diskWrite->value);
    if (state.latest.netRx) state.netRxTl.push(state.latest.netRx->value);
    if (state.latest.netTx) state.netTxTl.push(state.latest.netTx->value);
    if (state.latest.pcieRx) state.pcieRxTl.push(state.latest.pcieRx->value);
    if (state.latest.pcieTx) state.pcieTxTl.push(state.latest.pcieTx->value);

    static auto lastAnsiDraw = std::chrono::steady_clock::now();
    static Screen lastScreen = state.screen;

    renderFrame(frame, vp, state, cfg, debugMode);
    if (useAnsi) {
      if (state.screen != lastScreen) {
        prevAnsi.clear();
        lastScreen = state.screen;
      }
      const auto now = std::chrono::steady_clock::now();
      // Diff-based ANSI updates are much cheaper; keep a modest cap.
      if (now - lastAnsiDraw >= std::chrono::milliseconds(33)) {
        presentAnsiSpanDiff(hOut, frame, prevAnsi);
        lastAnsiDraw = now;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } else {
      present(hOut, frame);
      std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
  }
#endif
}

}  // namespace aiz
