#include <aiz/metrics/process_list.h>

#include <aiz/platform/process.h>

#include <algorithm>
#include <unordered_map>

namespace aiz {

std::optional<ProcessIdentity> readProcessIdentity(int pid) {
  const auto processes = platform::enumerateUserProcesses();
  for (const auto& p : processes) {
    if (static_cast<int>(p.pid) != pid) continue;
    ProcessIdentity id;
    id.name = p.name;
    id.cmdline = p.cmdline;
    id.ramBytes = p.memoryBytes;
    return id;
  }
  return std::nullopt;
}

bool isUserProcess(int pid) {
  return platform::isUserProcess(static_cast<platform::ProcessId>(pid));
}

std::vector<CpuProcessInfo> ProcessSampler::sampleTop(std::size_t maxCount) {
  std::vector<CpuProcessInfo> out;
  if (maxCount == 0) return out;

  const auto totalJiffies = platform::readTotalCpuJiffies();
  if (!totalJiffies) return out;

  const std::uint64_t deltaTotal = hasPrev_ ? (*totalJiffies - prevTotalJiffies_) : 0;

  std::unordered_map<int, std::uint64_t> curProcJiffies;
  curProcJiffies.reserve(1024);

  const auto processes = platform::enumerateUserProcesses();
  for (const auto& p : processes) {
    const int pid = static_cast<int>(p.pid);
    if (pid <= 0) continue;

    curProcJiffies[pid] = p.cpuJiffies;

    double cpuPct = 0.0;
    if (hasPrev_ && deltaTotal > 0) {
      auto it = prevProcJiffies_.find(pid);
      if (it != prevProcJiffies_.end()) {
        const std::uint64_t deltaProc = p.cpuJiffies - it->second;
        cpuPct = 100.0 * (static_cast<double>(deltaProc) / static_cast<double>(deltaTotal));
      }
    }

    out.push_back(CpuProcessInfo{pid, p.name, p.cmdline, cpuPct, p.memoryBytes});
  }

  prevTotalJiffies_ = *totalJiffies;
  prevProcJiffies_ = std::move(curProcJiffies);
  hasPrev_ = true;

  std::sort(out.begin(), out.end(), [](const CpuProcessInfo& a, const CpuProcessInfo& b) {
    if (a.cpuPct != b.cpuPct) return a.cpuPct > b.cpuPct;
    return a.pid < b.pid;
  });

  if (out.size() > maxCount) out.resize(maxCount);
  return out;
}

}  // namespace aiz
