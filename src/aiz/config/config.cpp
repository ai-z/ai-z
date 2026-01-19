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

#if defined(_WIN32)
  // Windows: prefer roaming AppData (per-user config).
  const char* appdata = std::getenv("APPDATA");
  if (appdata && *appdata) {
    return fs::path(appdata) / "ai-z" / "config.ini";
  }

  // Fallback: local AppData.
  const char* localappdata = std::getenv("LOCALAPPDATA");
  if (localappdata && *localappdata) {
    return fs::path(localappdata) / "ai-z" / "config.ini";
  }

  // Best-effort fallback if the above are missing.
  const char* userprofile = std::getenv("USERPROFILE");
  if (userprofile && *userprofile) {
    // Typical: C:\Users\<name>\AppData\Roaming
    return fs::path(userprofile) / "AppData" / "Roaming" / "ai-z" / "config.ini";
  }
#endif

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

static TimelineAgg parseTimelineAgg(std::string v, TimelineAgg fallback) {
  for (char& c : v) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (v == "max" || v == "peak" || v == "highest") return TimelineAgg::Max;
  if (v == "avg" || v == "average" || v == "mean") return TimelineAgg::Avg;
  return fallback;
}

static const char* timelineAggToString(TimelineAgg v) {
  switch (v) {
    case TimelineAgg::Max:
      return "max";
    case TimelineAgg::Avg:
      return "avg";
    default:
      return "max";
  }
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
    else if (key == "showGpuMem") cfg.showGpuMem = parseBool(val, cfg.showGpuMem);
    else if (key == "showDisk") {
      // Backward compat: old single toggle controls both.
      const bool v = parseBool(val, cfg.showDisk);
      cfg.showDisk = v;
      cfg.showDiskRead = v;
      cfg.showDiskWrite = v;
    }
    else if (key == "showDiskRead") cfg.showDiskRead = parseBool(val, cfg.showDiskRead);
    else if (key == "showDiskWrite") cfg.showDiskWrite = parseBool(val, cfg.showDiskWrite);
    else if (key == "showNet") {
      // Backward compat: single toggle controls both.
      const bool v = parseBool(val, true);
      cfg.showNetRx = v;
      cfg.showNetTx = v;
    }
    else if (key == "showNetRx") cfg.showNetRx = parseBool(val, cfg.showNetRx);
    else if (key == "showNetTx") cfg.showNetTx = parseBool(val, cfg.showNetTx);
    else if (key == "showPcie") {
      // Backward compat: old single toggle controls both.
      const bool v = parseBool(val, true);
      cfg.showPcieRx = v;
      cfg.showPcieTx = v;
    }
    else if (key == "showPcieRx") cfg.showPcieRx = parseBool(val, cfg.showPcieRx);
    else if (key == "showPcieTx") cfg.showPcieTx = parseBool(val, cfg.showPcieTx);
    else if (key == "showRam") cfg.showRam = parseBool(val, cfg.showRam);
    else if (key == "showVram") cfg.showVram = parseBool(val, cfg.showVram);
    else if (key == "refreshMs") cfg.refreshMs = static_cast<std::uint32_t>(std::stoul(val));
    else if (key == "timelineSamples") cfg.timelineSamples = static_cast<std::uint32_t>(std::stoul(val));
    else if (key == "timelineAgg") cfg.timelineAgg = parseTimelineAgg(val, cfg.timelineAgg);
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
  out << "showGpuMem=" << (showGpuMem ? "true" : "false") << "\n";
  // Prefer explicit per-direction toggles.
  out << "showDiskRead=" << (showDiskRead ? "true" : "false") << "\n";
  out << "showDiskWrite=" << (showDiskWrite ? "true" : "false") << "\n";
  out << "showNetRx=" << (showNetRx ? "true" : "false") << "\n";
  out << "showNetTx=" << (showNetTx ? "true" : "false") << "\n";
  out << "showPcieRx=" << (showPcieRx ? "true" : "false") << "\n";
  out << "showPcieTx=" << (showPcieTx ? "true" : "false") << "\n";
  out << "showRam=" << (showRam ? "true" : "false") << "\n";
  out << "showVram=" << (showVram ? "true" : "false") << "\n";
  out << "refreshMs=" << refreshMs << "\n";
  out << "timelineSamples=" << timelineSamples << "\n";
  out << "timelineAgg=" << timelineAggToString(timelineAgg) << "\n";
}

}  // namespace aiz
