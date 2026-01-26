#include <aiz/app.h>

#include <aiz/i18n.h>
#include <aiz/config/config.h>
#include <aiz/hw/hardware_info.h>
#include <aiz/metrics/amd_adlx.h>
#include <aiz/tui/ui.h>
#include <aiz/version.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

namespace aiz {

static bool hasFlag(int argc, char** argv, std::string_view flag) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::string_view(argv[i]) == flag) return true;
  }
  return false;
}

static void printHelp(std::ostream& os) {
  os << "ai-z performance timelines (CPU/GPU/Disk/PCIe) and benchmarks\n"
        "\n"
        "Usage:\n"
        "  ai-z [--debug] [--help|-h] [--version] [--hardware] [--lang <tag>]\n"
        "\n"
        "Options:\n"
        "  --debug      Run with synthetic/fake timelines\n"
        "  --help, -h   Show this help and exit\n"
        "  --version    Print version and exit\n"
        "  --hardware   Print hardware info and exit (no TUI)\n"
      "  --diag-pcie  Print Windows PCIe link diagnostics and exit (Windows)\n"
        "  --diag-adlx  Print AMD ADLX diagnostics and exit (Windows)\n"
        "  --lang TAG   UI language (en, zh-CN). Also reads AI_Z_LANG / LANG\n";
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
    std::cout << "ai-z " << AIZ_VERSION << "\n";
    std::cout << AIZ_WEBSITE << "\n";
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

  if (hasFlag(argc, argv, "--hardware")) {
    const HardwareInfo hw = HardwareInfo::probe();
    for (const auto& line : hw.toLines()) {
      std::cout << line << "\n";
    }
    return 0;
  }

  initUiLanguage(argc, argv);

  Config config = Config::load();
  const bool debugMode = hasFlag(argc, argv, "--debug");
  auto ui = makeUi();
  return ui->run(config, debugMode);
}

}  // namespace aiz
