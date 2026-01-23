#include <aiz/metrics/cpu_usage.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cctype>

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

static bool readProcStatPerCore(std::vector<std::uint64_t>& idleOut, std::vector<std::uint64_t>& totalOut) {
  std::ifstream in("/proc/stat");
  if (!in.is_open()) return false;

  std::vector<std::uint64_t> idle;
  std::vector<std::uint64_t> total;

  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("cpu", 0) != 0) continue;
    if (line.size() < 4) continue;
    if (!std::isdigit(static_cast<unsigned char>(line[3]))) continue;  // skip aggregate "cpu"

    std::istringstream iss(line);
    std::string cpu;
    iss >> cpu;

    std::uint64_t user = 0, nice = 0, system = 0, idleV = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    iss >> user >> nice >> system >> idleV >> iowait >> irq >> softirq >> steal;

    const std::uint64_t idleSum = idleV + iowait;
    const std::uint64_t totalSum = user + nice + system + idleV + iowait + irq + softirq + steal;

    if (totalSum == 0) continue;
    idle.push_back(idleSum);
    total.push_back(totalSum);
  }

  if (idle.empty() || idle.size() != total.size()) return false;
  idleOut = std::move(idle);
  totalOut = std::move(total);
  return true;
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

std::optional<Sample> CpuMaxCoreUsageCollector::sample() {
  std::vector<std::uint64_t> idle;
  std::vector<std::uint64_t> total;
  if (!readProcStatPerCore(idle, total)) {
    return std::nullopt;
  }

  if (!hasPrev_ || prevIdle_.size() != idle.size() || prevTotal_.size() != total.size()) {
    hasPrev_ = true;
    prevIdle_ = std::move(idle);
    prevTotal_ = std::move(total);
    return Sample{0.0, "%", "warming"};
  }

  double maxBusy = 0.0;
  for (std::size_t i = 0; i < idle.size(); ++i) {
    const std::uint64_t idleDelta = idle[i] - prevIdle_[i];
    const std::uint64_t totalDelta = total[i] - prevTotal_[i];
    if (totalDelta == 0) continue;
    const double busy = 100.0 * (1.0 - (static_cast<double>(idleDelta) / static_cast<double>(totalDelta)));
    if (busy > maxBusy) maxBusy = busy;
  }

  prevIdle_ = std::move(idle);
  prevTotal_ = std::move(total);

  return Sample{maxBusy, "%", ""};
}

}  // namespace aiz
