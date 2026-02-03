#include <aiz/platform/config_paths.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>

#include <cstdlib>

namespace aiz::platform {

static std::filesystem::path knownFolderPath(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
    return std::filesystem::path("ai-z");
  }

  std::filesystem::path out(path);
  CoTaskMemFree(path);
  return out / "ai-z";
}

std::filesystem::path configDirectory() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) {
    return std::filesystem::path(xdg) / "ai-z";
  }
  return knownFolderPath(FOLDERID_RoamingAppData);
}

std::filesystem::path dataDirectory() {
  const char* xdg = std::getenv("XDG_DATA_HOME");
  if (xdg && *xdg) {
    return std::filesystem::path(xdg) / "ai-z";
  }
  return knownFolderPath(FOLDERID_LocalAppData);
}

}  // namespace aiz::platform
