#include <aiz/app.h>

#include <aiz/config/config.h>
#include <aiz/tui/ui.h>

#include <string_view>

namespace aiz {

static bool hasFlag(int argc, char** argv, std::string_view flag) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::string_view(argv[i]) == flag) return true;
  }
  return false;
}

int App::run(int argc, char** argv) {
  Config config = Config::load();
  const bool debugMode = hasFlag(argc, argv, "--debug");
  auto ui = makeUi();
  return ui->run(config, debugMode);
}

}  // namespace aiz
