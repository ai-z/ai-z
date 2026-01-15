#include <aiz/metrics/cpu_usage.h>

#if defined(_WIN32)

#define NOMINMAX
#include <windows.h>

namespace aiz {

std::optional<Sample> CpuUsageCollector::sample() {
  FILETIME idleFt{}, kernelFt{}, userFt{};
  if (!GetSystemTimes(&idleFt, &kernelFt, &userFt)) {
    return std::nullopt;
  }

  auto toU64 = [](const FILETIME& ft) -> std::uint64_t {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<std::uint64_t>(u.QuadPart);
  };

  const std::uint64_t idle = toU64(idleFt);
  const std::uint64_t total = toU64(kernelFt) + toU64(userFt);
  if (total == 0) return std::nullopt;

  if (!hasPrev_) {
    hasPrev_ = true;
    prevIdle_ = idle;
    prevTotal_ = total;
    return Sample{0.0, "%", "warming"};
  }

  const std::uint64_t idleDelta = idle - prevIdle_;
  const std::uint64_t totalDelta = total - prevTotal_;
  prevIdle_ = idle;
  prevTotal_ = total;

  if (totalDelta == 0) {
    return Sample{0.0, "%", ""};
  }

  // Note: kernel time includes idle time, which matches the typical formula:
  // busy = 1 - idleDelta/totalDelta.
  const double busy = 100.0 * (1.0 - (static_cast<double>(idleDelta) / static_cast<double>(totalDelta)));
  return Sample{busy, "%", ""};
}

}  // namespace aiz

#else

#include <fstream>
#include <sstream>

namespace aiz {

static bool readProcStat(std::uint64_t& idleOut, std::uint64_t& totalOut) {
  std::ifstream in("/proc/stat");
  if (!in.is_open()) return false;

  std::string line;
  if (!std::getline(in, line)) return false;

  std::istringstream iss(line);
  std::string cpu;
  iss >> cpu;
  if (cpu != "cpu") return false;

  std::uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
  iss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

  idleOut = idle + iowait;
  totalOut = user + nice + system + idle + iowait + irq + softirq + steal;
  return totalOut != 0;
}

std::optional<Sample> CpuUsageCollector::sample() {
  std::uint64_t idle = 0;
  std::uint64_t total = 0;
  if (!readProcStat(idle, total)) {
    return std::nullopt;
  }

  if (!hasPrev_) {
    hasPrev_ = true;
    prevIdle_ = idle;
    prevTotal_ = total;
    return Sample{0.0, "%", "warming"};
  }

  const std::uint64_t idleDelta = idle - prevIdle_;
  const std::uint64_t totalDelta = total - prevTotal_;
  prevIdle_ = idle;
  prevTotal_ = total;

  if (totalDelta == 0) {
    return Sample{0.0, "%", ""};
  }

  const double busy = 100.0 * (1.0 - (static_cast<double>(idleDelta) / static_cast<double>(totalDelta)));
  return Sample{busy, "%", ""};
}

}  // namespace aiz

#endif
