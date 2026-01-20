#pragma once

#include <string>
#include <vector>

namespace aiz {

struct HardwareInfo {
  std::string osPretty;
  std::string kernelVersion;
  std::string cpuName;
  std::string cpuPhysicalCores;
  std::string cpuLogicalCores;
  std::string cpuCacheL1;
  std::string cpuCacheL2;
  std::string cpuCacheL3;
  std::string cpuInstructions;
  std::string ramSummary;
  std::string gpuName;
  std::string gpuDriver;

  // Optional extra GPU detail lines (e.g., per-GPU VRAM).
  // These are already formatted for display.
  std::vector<std::string> perGpuLines;

  // Optional extra NIC detail lines (e.g., per-interface driver/speed).
  // These are already formatted for display.
  std::vector<std::string> perNicLines;

  // Optional extra disk detail lines (e.g., per-device model/size).
  // These are already formatted for display.
  std::vector<std::string> perDiskLines;

  std::string vramSummary;
  std::string cudaVersion;
  std::string nvmlVersion;
  std::string rocmVersion;
  std::string openclVersion;
  std::string vulkanVersion;

  // Preformatted for UI rendering.
  std::vector<std::string> toLines() const;

  static HardwareInfo probe();
};

}  // namespace aiz
