#include <aiz/config/config.h>

#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace aiz {

static fs::path configPath() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) {
    return fs::path(xdg) / "ai-z" / "config.ini";
  }
  const char* home = std::getenv("HOME");
  if (home && *home) {
    return fs::path(home) / ".config" / "ai-z" / "config.ini";
  }
  return fs::path("config.ini");
}

static bool parseBool(std::string v, bool fallback) {
  for (char& c : v) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
  if (v == "0" || v == "false" || v == "no" || v == "off") return false;
  return fallback;
}

Config Config::load() {
  Config cfg;
  std::ifstream in(configPath());
  if (!in.is_open()) {
    return cfg;
  }

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);

    if (key == "showCpu") cfg.showCpu = parseBool(val, cfg.showCpu);
    else if (key == "showGpu") cfg.showGpu = parseBool(val, cfg.showGpu);
    else if (key == "showDisk") cfg.showDisk = parseBool(val, cfg.showDisk);
    else if (key == "showPcie") cfg.showPcie = parseBool(val, cfg.showPcie);
    else if (key == "refreshMs") cfg.refreshMs = static_cast<std::uint32_t>(std::stoul(val));
    else if (key == "timelineSamples") cfg.timelineSamples = static_cast<std::uint32_t>(std::stoul(val));
  }
  return cfg;
}

void Config::save() const {
  const fs::path path = configPath();
  fs::create_directories(path.parent_path());

  std::ofstream out(path);
  out << "# ai-z config\n";
  out << "showCpu=" << (showCpu ? "true" : "false") << "\n";
  out << "showGpu=" << (showGpu ? "true" : "false") << "\n";
  out << "showDisk=" << (showDisk ? "true" : "false") << "\n";
  out << "showPcie=" << (showPcie ? "true" : "false") << "\n";
  out << "refreshMs=" << refreshMs << "\n";
  out << "timelineSamples=" << timelineSamples << "\n";
}

}  // namespace aiz
