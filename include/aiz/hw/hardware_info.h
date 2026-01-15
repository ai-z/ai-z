#pragma once

#include <string>
#include <vector>

namespace aiz {

struct HardwareInfo {
  std::string osPretty;
  std::string cpuName;
  std::string ramSummary;
  std::string gpuName;
  std::string gpuDriver;

  // Preformatted for UI rendering.
  std::vector<std::string> toLines() const;

  static HardwareInfo probe();
};

}  // namespace aiz
