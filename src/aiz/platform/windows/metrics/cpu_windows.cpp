#include <aiz/platform/metrics/cpu.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace aiz::platform {

static std::uint64_t fileTimeToU64(const FILETIME& ft) {
  return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::optional<CpuTimes> readCpuTimes() {
  FILETIME idleTime{}, kernelTime{}, userTime{};
  if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) return std::nullopt;

  const std::uint64_t idle = fileTimeToU64(idleTime);
  const std::uint64_t kernel = fileTimeToU64(kernelTime);
  const std::uint64_t user = fileTimeToU64(userTime);

  return CpuTimes{idle, kernel + user};
}

std::optional<std::vector<CpuTimes>> readPerCoreCpuTimes() {
  auto agg = readCpuTimes();
  if (!agg) return std::nullopt;

  SYSTEM_INFO sysInfo;
  GetSystemInfo(&sysInfo);
  const DWORD numCores = sysInfo.dwNumberOfProcessors;
  if (numCores == 0) return std::nullopt;

  std::vector<CpuTimes> result(numCores, *agg);
  return result;
}

}  // namespace aiz::platform
