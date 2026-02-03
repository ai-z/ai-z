#include <aiz/platform/config_paths.h>

#include <cstdlib>

namespace aiz::platform {

std::filesystem::path configDirectory() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) {
    return std::filesystem::path(xdg) / "ai-z";
  }
  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::filesystem::path(home) / ".config" / "ai-z";
  }
  return std::filesystem::path("ai-z");
}

std::filesystem::path dataDirectory() {
  const char* xdg = std::getenv("XDG_DATA_HOME");
  if (xdg && *xdg) {
    return std::filesystem::path(xdg) / "ai-z";
  }
  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::filesystem::path(home) / ".local" / "share" / "ai-z";
  }
  return std::filesystem::path("ai-z");
}

}  // namespace aiz::platform
