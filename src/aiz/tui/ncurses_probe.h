#pragma once

#include <string>
#include <vector>

namespace aiz {
struct HardwareInfo;

namespace ncurses {

std::vector<std::string> parseGpuNames(const HardwareInfo& hw, unsigned int gpuCount);

std::string probeCpuNameFast();
unsigned int probeGpuCountFast();
std::vector<std::string> probeGpuNamesFast(unsigned int gpuCount, bool hasNvml);
std::vector<std::string> probeGpuNamesCudaFast(unsigned int desiredCount);

bool hasRealDeviceNames(const std::vector<std::string>& names);

}  // namespace ncurses
}  // namespace aiz
