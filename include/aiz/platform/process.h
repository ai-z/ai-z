#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aiz::platform {

using ProcessId = std::uint32_t;

struct ProcessInfo {
  ProcessId pid = 0;
  std::string name;
  std::string cmdline;
  std::uint64_t cpuJiffies = 0;
  std::uint64_t memoryBytes = 0;
};

std::vector<ProcessInfo> enumerateUserProcesses();
std::optional<std::uint64_t> readTotalCpuJiffies();
bool isUserProcess(ProcessId pid);

}  // namespace aiz::platform
