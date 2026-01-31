#include <aiz/app.h>

#include <aiz/i18n.h>
#include <aiz/config/config.h>
#include <aiz/hw/hardware_info.h>
#include <aiz/metrics/amd_adlx.h>
#include <aiz/metrics/intel_igcl.h>
#include <aiz/metrics/windows_d3dkmt.h>
#include <aiz/bench/report.h>
#include <aiz/snapshot/snapshot.h>
#include <aiz/tui/ui.h>
#include <aiz/version.h>

#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string_view>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace aiz {

static constexpr const char* kAppDisplayName = "AI-Z";

#if defined(_WIN32)
namespace ncurses {
std::string windowsPdhGpuMemoryDiagnostics();
}  // namespace ncurses
#endif

static bool hasFlag(int argc, char** argv, std::string_view flag) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::string_view(argv[i]) == flag) return true;
  }
  return false;
}

static void printHelp(std::ostream& os) {
  os << "AI-Z performance timelines (CPU/GPU/Disk/PCIe) and benchmarks\n"
        "\n"
        "Usage:\n"
        "  ai-z [--debug] [--help|-h] [--version] [--hardware] [--bench-report] [--lang <tag>]\n"
        "  ai-z --snapshot [--format json] [--snapshot-loop [MS]]\n"
        "\n"
        "Options:\n"
        "  --debug      Run with synthetic/fake timelines\n"
        "  --help, -h   Show this help and exit\n"
        "  --version    Print version and exit\n"
        "  --hardware   Print hardware info and exit (no TUI)\n"
        "  --bench-report  Run all benchmarks and write an HTML report\n"
        "  --snapshot   Print JSON snapshot of all device telemetry and exit\n"
        "  --format FMT Output format for snapshot: json (default: json)\n"
        "  --snapshot-loop [MS]  Continuous snapshot loop (default: 500ms interval)\n"
        "  --diag-pcie  Print Windows PCIe link diagnostics and exit (Windows)\n"
        "  --diag-adlx  Print AMD ADLX diagnostics and exit (Windows)\n"
        "  --diag-igcl  Print Intel IGCL diagnostics and exit (Windows)\n"
        "  --diag-igcl-full  Print detailed Intel IGCL diagnostics (Windows)\n"
        "  --diag-d3dkmt  Print D3DKMT VRAM diagnostics (Windows)\n"
        "  --diag-pdh-gpu  Print PDH GPU memory diagnostics (Windows)\n"
        "  --lang TAG   UI language (en, zh-CN). Also reads AI_Z_LANG / LANG\n";
}

static void setTerminalTitle(std::string_view title) {
#if defined(_WIN32)
  // Best-effort; no harm if it fails.
  std::wstring w;
  w.reserve(title.size());
  for (const char c : title) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  (void)SetConsoleTitleW(w.c_str());
#else
  // Only emit OSC title sequences when stdout is a TTY.
  if (!::isatty(::fileno(stdout))) return;
  // OSC 0 (icon + window title), BEL-terminated.
  std::fputs("\x1b]0;", stdout);
  std::fwrite(title.data(), 1, title.size(), stdout);
  std::fputc('\a', stdout);
  std::fflush(stdout);
#endif
}

static std::optional<std::string_view> flagValue(int argc, char** argv, std::string_view flag) {
  for (int i = 1; i < argc; ++i) {
    if (!argv[i]) continue;
    const std::string_view a(argv[i]);
    if (a == flag) {
      if (i + 1 < argc && argv[i + 1]) return std::string_view(argv[i + 1]);
      return std::nullopt;
    }
    if (a.rfind(flag, 0) == 0 && a.size() > flag.size() && a[flag.size()] == '=') {
      return a.substr(flag.size() + 1);
    }
  }
  return std::nullopt;
}

static void initUiLanguage(int argc, char** argv) {
  if (const auto cli = flagValue(argc, argv, "--lang")) {
    i18n::setLanguageTag(*cli);
    return;
  }

  if (const char* env = std::getenv("AI_Z_LANG")) {
    if (*env != '\0') {
      i18n::setLanguageTag(env);
      return;
    }
  }

  // Fall back to process locale env vars; common values include zh_CN.UTF-8.
  if (const char* lcAll = std::getenv("LC_ALL")) {
    if (*lcAll != '\0') {
      i18n::setLanguageTag(lcAll);
      return;
    }
  }
  if (const char* lang = std::getenv("LANG")) {
    if (*lang != '\0') {
      i18n::setLanguageTag(lang);
      return;
    }
  }
}

int App::run(int argc, char** argv) {
  if (hasFlag(argc, argv, "--help") || hasFlag(argc, argv, "-h")) {
    printHelp(std::cout);
    return 0;
  }

  if (hasFlag(argc, argv, "--version")) {
    std::cout << kAppDisplayName << " " << AIZ_VERSION << "\n";
    std::cout << AIZ_WEBSITE << "\n";
    return 0;
  }

  if (hasFlag(argc, argv, "--snapshot")) {
    // Check format (only json supported for now)
    auto format = flagValue(argc, argv, "--format").value_or("json");
    if (format != "json") {
      std::cerr << "Error: unsupported format '" << format << "'. Only 'json' is supported.\n";
      return 1;
    }

    // Check for loop mode
    if (hasFlag(argc, argv, "--snapshot-loop")) {
      auto loopVal = flagValue(argc, argv, "--snapshot-loop");
      int intervalMs = 500;  // default
      if (loopVal && !loopVal->empty()) {
        try {
          intervalMs = std::stoi(std::string(*loopVal));
          if (intervalMs < 10) intervalMs = 10;  // minimum 10ms
        } catch (...) {
          std::cerr << "Error: invalid interval '" << *loopVal << "'. Using default 500ms.\n";
          intervalMs = 500;
        }
      }
      return runSnapshotLoop(intervalMs);
    }

    // Single snapshot mode
    auto snapshot = captureSystemSnapshot();
    std::cout << snapshotToJson(snapshot) << "\n";
    return 0;
  }

  if (hasFlag(argc, argv, "--diag-pcie")) {
    #if defined(_WIN32)
    std::cout << pcieDiagnostics();
    #else
    std::cout << "--diag-pcie is only available on Windows.\n";
    #endif
    return 0;
  }

  if (hasFlag(argc, argv, "--diag-adlx")) {
    #if defined(_WIN32)
    std::cout << adlxDiagnostics();
    #else
    std::cout << "--diag-adlx is only available on Windows.\n";
    #endif
    return 0;
  }

  if (hasFlag(argc, argv, "--diag-igcl")) {
    #if defined(_WIN32)
    std::cout << igclDiagnostics();
    #else
    std::cout << "--diag-igcl is only available on Windows.\n";
    #endif
    return 0;
  }

  if (hasFlag(argc, argv, "--diag-igcl-full")) {
    #if defined(_WIN32)
    std::cout << igclDiagnosticsDetailed();
    #else
    std::cout << "--diag-igcl-full is only available on Windows.\n";
    #endif
    return 0;
  }

  if (hasFlag(argc, argv, "--diag-d3dkmt")) {
    #if defined(_WIN32)
    std::cout << d3dkmtDiagnostics();
    #else
    std::cout << "--diag-d3dkmt is only available on Windows.\n";
    #endif
    return 0;
  }

  if (hasFlag(argc, argv, "--diag-pdh-gpu")) {
    #if defined(_WIN32)
    std::cout << ncurses::windowsPdhGpuMemoryDiagnostics();
    #else
    std::cout << "--diag-pdh-gpu is only available on Windows.\n";
    #endif
    return 0;
  }

  if (hasFlag(argc, argv, "--hardware")) {
    const HardwareInfo hw = HardwareInfo::probe();
    for (const auto& line : hw.toLines()) {
      std::cout << line << "\n";
    }
    return 0;
  }

  if (hasFlag(argc, argv, "--bench-report")) {
    const auto report = runBenchmarksAndGenerateHtmlReport();
    if (!report) {
      std::cerr << "Failed to generate benchmark report.\n";
      return 1;
    }

    for (const auto& row : report->rows) {
      if (row.isHeader) {
        std::cout << "\n" << row.title << "\n";
        continue;
      }
      std::cout << "  " << row.title << " : " << row.result << "\n";
    }

    std::cout << "\nREPORT SAVED: " << report->path << "\n";
    return 0;
  }

  initUiLanguage(argc, argv);

  // Set the terminal/window title while the TUI is running.
  setTerminalTitle(kAppDisplayName);

  Config config = Config::load();
  const bool debugMode = hasFlag(argc, argv, "--debug");
  
  if (debugMode) {
    std::cerr << "ai-z: debug mode enabled\n";
    std::cerr << "ai-z: creating UI...\n";
    std::cerr.flush();
  }
  
  auto ui = makeUi();
  
  if (debugMode) {
    std::cerr << "ai-z: UI created, calling run()...\n";
    std::cerr.flush();
  }
  
  return ui->run(config, debugMode);
}

}  // namespace aiz
