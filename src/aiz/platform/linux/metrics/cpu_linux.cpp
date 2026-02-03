#include <aiz/platform/metrics/cpu.h>

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace aiz::platform {

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

std::optional<CpuTimes> readCpuTimes() {
  std::uint64_t idle = 0;
  std::uint64_t total = 0;
  if (!readProcStat(idle, total)) return std::nullopt;
  return CpuTimes{idle, total};
}

std::optional<std::vector<CpuTimes>> readPerCoreCpuTimes() {
  std::vector<std::uint64_t> idle;
  std::vector<std::uint64_t> total;
  if (!readProcStatPerCore(idle, total)) return std::nullopt;

  std::vector<CpuTimes> out;
  out.reserve(idle.size());
  for (std::size_t i = 0; i < idle.size(); ++i) {
    out.push_back(CpuTimes{idle[i], total[i]});
  }
  return out;
}

}  // namespace aiz::platform
