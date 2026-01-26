#include <aiz/platform/metrics/memory.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace aiz::platform {

std::optional<MemoryInfo> readMemoryInfo() {
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  if (!GlobalMemoryStatusEx(&mem)) return std::nullopt;

  return MemoryInfo{mem.ullTotalPhys, mem.ullAvailPhys};
}

}  // namespace aiz::platform
