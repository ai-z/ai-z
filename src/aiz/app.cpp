#include <aiz/app.h>

#include <aiz/config/config.h>
#include <aiz/tui/ui.h>
#include <aiz/version.h>

#include <iostream>
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
        "  ai-z [--debug] [--help|-h] [--version]\n"
        "\n"
        "Options:\n"
        "  --debug      Run with synthetic/fake timelines\n"
        "  --help, -h   Show this help and exit\n"
        "  --version    Print version and exit\n";
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

  Config config = Config::load();
  const bool debugMode = hasFlag(argc, argv, "--debug");
  auto ui = makeUi();
  return ui->run(config, debugMode);
}

}  // namespace aiz
