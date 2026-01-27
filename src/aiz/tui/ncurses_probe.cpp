#include "ncurses_probe.h"

#include <aiz/dyn/cuda.h>
#include <aiz/hw/hardware_info.h>
#include <aiz/metrics/nvidia_nvml.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_2.h>

static std::string wideToUtf8(const wchar_t* s) {
  if (!s) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string out(len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), len, nullptr, nullptr);
  return out;
}

static bool isIgnoredAdapter(const DXGI_ADAPTER_DESC1& desc, const std::string& name) {
  const bool isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
  const bool isBasicRenderDriver = (desc.VendorId == 0x1414) || (name == "Microsoft Basic Render Driver");
  return isSoftware || isBasicRenderDriver;
}

static std::string dxgiAdapterKey(const DXGI_ADAPTER_DESC1& desc, const std::string& name) {
  std::ostringstream key;
  key << desc.VendorId << ":" << desc.DeviceId << ":" << desc.SubSysId << ":" << desc.Revision << ":";
  key << desc.DedicatedVideoMemory << ":" << desc.SharedSystemMemory << ":" << name;
  return key.str();
}
#endif

namespace aiz::ncurses {

std::vector<std::string> parseGpuNames(const HardwareInfo& hw, unsigned int gpuCount) {
  std::vector<std::string> names;
  names.resize(gpuCount);
  for (unsigned int i = 0; i < gpuCount; ++i) names[i] = "GPU" + std::to_string(i);

  // Fallback: if we only have a single GPU name (legacy field), use it for GPU0.
  if (gpuCount > 0 && !hw.gpuName.empty() && hw.gpuName != "--") {
    if (names[0] == "GPU0") names[0] = hw.gpuName;
  }

  for (const auto& l : hw.perGpuLines) {
    // Format: "GPU0: <name>".
    if (l.rfind("GPU", 0) != 0) continue;
    const std::size_t sep = l.find(": ");
    if (sep == std::string::npos) continue;
    const std::string left = l.substr(0, sep);
    const std::string right = l.substr(sep + 2);
    if (left.size() < 4) continue;
    try {
      const unsigned int idx = static_cast<unsigned int>(std::stoul(left.substr(3)));
      if (idx < names.size() && !right.empty()) names[idx] = right;
    } catch (...) {
      continue;
    }
  }

  return names;
}

std::string probeCpuNameFast() {
#if defined(__linux__)
  std::ifstream in("/proc/cpuinfo");
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
    const std::size_t pos = line.find(':');
    if (pos == std::string::npos) continue;

    std::string k = line.substr(0, pos);
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();

    std::string kLower;
    kLower.reserve(k.size());
    for (char c : k) kLower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    const bool isModel = (kLower == "model name");
    const bool isHardware = (kLower == "hardware");
    const bool isProcessor = (kLower == "processor");
    if (!isModel && !isHardware && !isProcessor) continue;

    std::string v = line.substr(pos + 1);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    if (!v.empty()) return v;
  }
#endif
  return {};
}

unsigned int probeGpuCountFast() {
#if defined(_WIN32)
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    return 0;
  }

  unsigned int count = 0;
  std::unordered_map<std::string, bool> seen;
  for (UINT i = 0; ; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      const std::string name = wideToUtf8(desc.Description);
      if (isIgnoredAdapter(desc, name)) {
        adapter->Release();
        continue;
      }
      const std::string key = dxgiAdapterKey(desc, name);
      if (seen.emplace(key, true).second) ++count;
    }
    adapter->Release();
  }

  factory->Release();
  return count;
#else
  return 0;
#endif
}

std::vector<std::string> probeGpuNamesFast(unsigned int gpuCount, bool hasNvml) {
  std::vector<std::string> names;
  names.resize(gpuCount);
  for (unsigned int i = 0; i < gpuCount; ++i) names[i] = "GPU" + std::to_string(i);

#if defined(_WIN32)
  // DXGI adapter names on Windows (covers AMD/Intel GPUs).
  IDXGIFactory1* factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    unsigned int hwIndex = 0;
    std::unordered_map<std::string, bool> seen;
    for (UINT i = 0; ; ++i) {
      IDXGIAdapter1* adapter = nullptr;
      if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc{};
      if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        const std::string n = wideToUtf8(desc.Description);
        if (isIgnoredAdapter(desc, n)) {
          adapter->Release();
          continue;
        }
        const std::string key = dxgiAdapterKey(desc, n);
        if (seen.emplace(key, true).second) {
          if (hwIndex < names.size()) {
            if (!n.empty()) names[hwIndex] = n;
          }
          ++hwIndex;
        }
      }
      adapter->Release();
    }
    factory->Release();
  }
  if (hasRealDeviceNames(names)) return names;
#endif

  // Prefer NVML device names when available.
  // Try the in-process variant first, but fall back to the fork/timeout wrapper:
  // some NVML setups behave better when called in the isolated child.
  if (hasNvml) {
    for (unsigned int i = 0; i < gpuCount; ++i) {
      if (const auto n = readNvmlGpuNameForGpuNoFork(i)) {
        if (!n->empty() && *n != "--") {
          names[static_cast<std::size_t>(i)] = *n;
          continue;
        }
      }
      if (const auto n = readNvmlGpuNameForGpu(i)) {
        if (!n->empty() && *n != "--") names[static_cast<std::size_t>(i)] = *n;
      }
    }
  }

  return names;
}

std::vector<std::string> probeGpuNamesCudaFast(unsigned int desiredCount) {
  std::vector<std::string> names;
  names.resize(desiredCount);
  for (unsigned int i = 0; i < desiredCount; ++i) names[i] = "GPU" + std::to_string(i);

  std::string err;
  const auto* cu = dyn::cuda::api(&err);
  if (!cu || !cu->cuInit || !cu->cuDeviceGetCount || !cu->cuDeviceGet || !cu->cuDeviceGetName) return names;

  if (cu->cuInit(0) != dyn::cuda::CUDA_SUCCESS) return names;
  int count = 0;
  if (cu->cuDeviceGetCount(&count) != dyn::cuda::CUDA_SUCCESS || count <= 0) return names;

  const unsigned int n = std::min<unsigned int>(desiredCount, static_cast<unsigned int>(count));
  for (unsigned int i = 0; i < n; ++i) {
    dyn::cuda::CUdevice dev{};
    if (cu->cuDeviceGet(&dev, static_cast<int>(i)) != dyn::cuda::CUDA_SUCCESS) continue;
    char buf[128]{};
    if (cu->cuDeviceGetName(buf, static_cast<int>(sizeof(buf)), dev) != dyn::cuda::CUDA_SUCCESS) continue;
    const std::string s(buf);
    if (!s.empty()) names[static_cast<std::size_t>(i)] = s;
  }

  return names;
}

bool hasRealDeviceNames(const std::vector<std::string>& names) {
  for (std::size_t i = 0; i < names.size(); ++i) {
    const std::string& n = names[i];
    if (n.empty() || n == "--" || n == "unknown") continue;
    const std::string placeholder = "GPU" + std::to_string(i);
    if (n != placeholder) return true;
  }
  return false;
}

}  // namespace aiz::ncurses
