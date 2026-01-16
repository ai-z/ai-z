#pragma once

#include <string>
#include <vector>

namespace aiz {

struct HardwareInfo {
  std::string osPretty;
  std::string kernelVersion;
  std::string cpuName;
  std::string cpuInstructions;
  std::string ramSummary;
  std::string gpuName;
  std::string gpuDriver;

  // Optional extra GPU detail lines (e.g., per-GPU VRAM).
  // These are already formatted for display.
  std::vector<std::string> perGpuLines;

  std::string vramSummary;
  std::string cudaVersion;
  std::string nvmlVersion;
  std::string rocmVersion;
  std::string openglVersion;
  std::string vulkanVersion;

  // Preformatted for UI rendering.
  std::vector<std::string> toLines() const;

  static HardwareInfo probe();
};

}  // namespace aiz
