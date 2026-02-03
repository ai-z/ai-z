#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aiz {

struct CpuProcessInfo {
  int pid = 0;
  std::string name;
  std::string cmdline;
  double cpuPct = 0.0;
  std::uint64_t ramBytes = 0;
};

struct ProcessIdentity {
  std::string name;
  std::string cmdline;
  std::uint64_t ramBytes = 0;
};

// Reads name + RSS memory for a process (best-effort).
std::optional<ProcessIdentity> readProcessIdentity(int pid);

// Returns true if the process belongs to the current user (best-effort).
bool isUserProcess(int pid);

// Sampling helper that tracks per-process CPU deltas across calls.
class ProcessSampler {
 public:
  std::vector<CpuProcessInfo> sampleTop(std::size_t maxCount);

 private:
  bool hasPrev_ = false;
  std::uint64_t prevTotalJiffies_ = 0;
  std::unordered_map<int, std::uint64_t> prevProcJiffies_;
};

}  // namespace aiz
