#include <aiz/hw/hardware_info.h>

#include <aiz/dyn/cuda.h>
#include <aiz/metrics/amd_adlx.h>
#include <aiz/metrics/intel_igcl.h>
#include <aiz/metrics/windows_d3dkmt.h>
#include <aiz/metrics/nvidia_nvml.h>
#include <aiz/metrics/npu_info.h>
#include <aiz/platform/metrics/memory.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <intrin.h>

#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace aiz {

static constexpr const char* kUnknown = "--";

static std::string wideToUtf8(const wchar_t* s) {
  if (!s) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string out(len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), len, nullptr, nullptr);
  return out;
}

static std::string readRegString(const wchar_t* keyPath, const wchar_t* valueName) {
  HKEY hKey;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
    return {};
  }

  wchar_t buf[256]{};
  DWORD size = sizeof(buf);
  if (RegQueryValueExW(hKey, valueName, nullptr, nullptr, reinterpret_cast<LPBYTE>(buf), &size) != ERROR_SUCCESS) {
    RegCloseKey(hKey);
    return {};
  }
  RegCloseKey(hKey);
  return wideToUtf8(buf);
}

static std::string probeOsPrettyNameWindows() {
  const auto name = readRegString(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
  const auto display = readRegString(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
  if (!name.empty() && !display.empty()) return name + " " + display;
  if (!name.empty()) return name;
  return "Windows";
}

static std::string probeKernelVersionWindows() {
  const auto build = readRegString(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber");
  if (!build.empty()) return "Build " + build;
  return kUnknown;
}

static std::string probeCpuNameWindows() {
  int cpuInfo[4]{};
  char brand[49]{};

  __cpuid(cpuInfo, 0x80000000);
  if (static_cast<unsigned int>(cpuInfo[0]) >= 0x80000004) {
    __cpuid(reinterpret_cast<int*>(brand), 0x80000002);
    __cpuid(reinterpret_cast<int*>(brand + 16), 0x80000003);
    __cpuid(reinterpret_cast<int*>(brand + 32), 0x80000004);
    return std::string(brand);
  }
  return kUnknown;
}

static std::string probeCpuLogicalCoresWindows() {
  SYSTEM_INFO sysInfo;
  GetSystemInfo(&sysInfo);
  return std::to_string(sysInfo.dwNumberOfProcessors);
}

static std::string probeCpuPhysicalCoresWindows() {
  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
  if (length == 0) return kUnknown;

  std::vector<char> buffer(length);
  auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) return kUnknown;

  DWORD count = 0;
  char* ptr = buffer.data();
  while (ptr < buffer.data() + length) {
    auto* current = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(ptr);
    if (current->Relationship == RelationProcessorCore) ++count;
    ptr += current->Size;
  }
  return count ? std::to_string(count) : kUnknown;
}

static std::string probeRamSummary() {
  const auto mem = platform::readMemoryInfo();
  if (!mem) return kUnknown;
  const double totalGiB = static_cast<double>(mem->totalBytes) / (1024.0 * 1024.0 * 1024.0);
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(1);
  oss << totalGiB << " GiB";
  return oss.str();
}

static std::string formatGiB(std::uint64_t bytes) {
  if (!bytes) return std::string(kUnknown);
  const double totalGiB = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(1);
  oss << totalGiB << " GiB";
  return oss.str();
}

struct DxgiGpuInfo {
  std::string name;
  std::uint64_t dedicatedBytes = 0;
  std::uint64_t sharedBytes = 0;
  std::uint64_t localBudgetBytes = 0;
  std::uint64_t localCurrentUsageBytes = 0;
  std::uint32_t vendorId = 0;
  std::uint32_t deviceId = 0;
  std::uint32_t subsysId = 0;
  std::uint32_t revision = 0;
  std::optional<AmdAdapterLuid> adapterLuid;
  unsigned int duplicateCount = 1;
};

static std::vector<DxgiGpuInfo> probeDxgiGpus() {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    return {};
  }

  std::vector<DxgiGpuInfo> gpus;
  std::unordered_map<std::string, std::size_t> byKey;
  for (UINT i = 0; ; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;

    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      // Skip known software / placeholder adapters.
      const std::string name = wideToUtf8(desc.Description);
      const bool isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
      const bool isBasicRenderDriver = (desc.VendorId == 0x1414) || (name == "Microsoft Basic Render Driver");

      if (!isSoftware && !isBasicRenderDriver) {
        DxgiGpuInfo info;
        info.name = name;
        info.dedicatedBytes = desc.DedicatedVideoMemory;
        info.sharedBytes = desc.SharedSystemMemory;
        info.vendorId = desc.VendorId;
        info.deviceId = desc.DeviceId;
        info.subsysId = desc.SubSysId;
        info.revision = desc.Revision;
        // Prefer the adapter LUID for matching.
        info.adapterLuid = AmdAdapterLuid{static_cast<std::uint32_t>(desc.AdapterLuid.LowPart),
                  static_cast<std::int32_t>(desc.AdapterLuid.HighPart)};

        // Best-effort: query current local VRAM usage/budget via DXGI.
        // This works on WDDM 2.x and provides useful stats for Intel as well.
        {
          IDXGIAdapter3* adapter3 = nullptr;
          if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) && adapter3) {
            DXGI_QUERY_VIDEO_MEMORY_INFO vm{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm))) {
              info.localBudgetBytes = static_cast<std::uint64_t>(vm.Budget);
              info.localCurrentUsageBytes = static_cast<std::uint64_t>(vm.CurrentUsage);
            }
            adapter3->Release();
            adapter3 = nullptr;
          }
        }

        // Collapse duplicate DXGI adapters that appear identical.
        // This avoids listing the same physical GPU multiple times on some systems.
        std::ostringstream key;
        key << info.vendorId << ":" << info.deviceId << ":" << info.subsysId << ":" << info.revision << ":";
        key << info.dedicatedBytes << ":" << info.sharedBytes << ":" << info.name;
        const std::string k = key.str();

        if (const auto it = byKey.find(k); it != byKey.end()) {
          gpus[it->second].duplicateCount++;
        } else {
          byKey.emplace(k, gpus.size());
          gpus.push_back(std::move(info));
        }
      }
    }

    adapter->Release();
  }

  factory->Release();
  return gpus;
}

static std::string vendorNameFromId(std::uint32_t vendorId) {
  switch (vendorId) {
    case 0x10DE:
      return "nvidia";
    case 0x1002:
    case 0x1022:
      return "amd";
    case 0x8086:
      return "intel";
    default:
      return {};
  }
}

static std::string probeCudaVersion() {
  std::string err;
  const auto* cu = dyn::cuda::api(&err);
  if (!cu) return kUnknown;
  if (!cu->cuInit || !cu->cuDriverGetVersion) return kUnknown;
  if (cu->cuInit(0) != dyn::cuda::CUDA_SUCCESS) return kUnknown;

  int v = 0;
  if (cu->cuDriverGetVersion(&v) != dyn::cuda::CUDA_SUCCESS || v <= 0) return kUnknown;
  const int major = v / 1000;
  const int minor = (v % 1000) / 10;
  return std::to_string(major) + "." + std::to_string(minor);
}

static std::string probeNvmlVersion() {
  if (auto v = readNvmlLibraryVersion()) return *v;
  return kUnknown;
}

static std::string probeGpuName() {
  if (auto n = readNvmlGpuNameForGpu(0)) return *n;
  return kUnknown;
}

static std::string probeGpuDriver() {
  if (auto v = readNvmlDriverVersion()) return "nvidia " + *v;
  return kUnknown;
}

static std::string probeVramSummary() {
  if (const auto t = readNvmlTelemetry()) {
    const auto n = nvmlGpuCount();
    const double total = t->memTotalGiB;
    if (total > 0.0) {
      std::ostringstream oss;
      oss.setf(std::ios::fixed);
      oss.precision(1);
      oss << total << " GiB";
      if (n && *n > 1) oss << " (" << *n << " GPUs)";
      return oss.str();
    }
  }
  return kUnknown;
}

static std::vector<std::string> probePerGpuLinesNvidia() {
  const auto n = nvmlGpuCount();
  if (!n || *n == 0) return {};

  std::vector<std::string> lines;
  const std::string indent = " ";
  for (unsigned int gpuIndex = 0; gpuIndex < *n; ++gpuIndex) {
    const std::string name = readNvmlGpuNameForGpu(gpuIndex).value_or(std::string(kUnknown));
    lines.push_back("GPU" + std::to_string(gpuIndex) + ": " + name);
    if (const auto t = readNvmlTelemetryForGpu(gpuIndex)) {
      if (t->memTotalGiB > 0.0) {
        std::ostringstream l;
        l.setf(std::ios::fixed);
        l.precision(1);
        l << indent << "Memory: " << t->memTotalGiB << " GiB";
        lines.push_back(l.str());
      }
    }
  }
  return lines;
}

static std::vector<std::string> probePerGpuLinesDxgi(const std::vector<DxgiGpuInfo>& gpus) {
  std::vector<std::string> lines;
  const std::string indent = " ";
  for (std::size_t i = 0; i < gpus.size(); ++i) {
    const auto& gpu = gpus[i];
    const std::string name = gpu.name.empty() ? std::string(kUnknown) : gpu.name;
    {
      std::string header = "GPU" + std::to_string(i) + ": " + name;
      if (gpu.duplicateCount > 1) header += " (x" + std::to_string(gpu.duplicateCount) + ")";
      lines.push_back(std::move(header));
    }

    const std::uint64_t mem = gpu.dedicatedBytes ? gpu.dedicatedBytes : gpu.sharedBytes;
    if (mem) {
      std::ostringstream l;
      l << indent << "Memory: " << formatGiB(mem);
      if (gpu.dedicatedBytes) l << " (Dedicated)";
      else if (gpu.sharedBytes) l << " (Shared)";
      lines.push_back(l.str());
    }

    // Best-effort VRAM usage via D3DKMT (works without vendor SDKs).
    {
      std::uint64_t used = gpu.localCurrentUsageBytes;
      std::uint64_t budget = gpu.localBudgetBytes;

      if ((used == 0 && budget == 0) && gpu.adapterLuid) {
        LUID luid{};
        luid.LowPart = gpu.adapterLuid->lowPart;
        luid.HighPart = gpu.adapterLuid->highPart;
        if (const auto vm = queryD3dkmtLocalVideoMemoryForLuid(luid)) {
          used = vm->currentUsageBytes;
          budget = vm->budgetBytes;
        }
      }

      if (used > 0 || budget > 0) {
        std::ostringstream l;
        l << indent << "VRAM used: " << formatGiB(used);
        if (budget > 0) l << " / " << formatGiB(budget) << " budget";
        lines.push_back(l.str());
      }
    }

    // AMD PCIe link info via ADLX (best-effort).
    if (gpu.vendorId == 0x1002 || gpu.vendorId == 0x1022) {
      if (const auto link = readAdlxPcieLinkForDxgi(gpu.adapterLuid)) {
          std::ostringstream l;
          l.setf(std::ios::fixed);
          l.precision(1);
          l << indent << "PCIe: " << link->width << "x";
          if (link->generation) l << "@Gen" << link->generation;
        lines.push_back(l.str());
      } else {
        const std::string note = amdPcieLinkNoteForDxgi(gpu.adapterLuid);
        if (!note.empty()) {
          lines.push_back(indent + std::string("PCIe: unavailable (") + note + ")");
        } else {
          const auto st = adlxStatus();
          if (st == AdlxStatus::MissingDll) {
            lines.push_back(indent + std::string("PCIe: unavailable (ADLX not found)"));
          } else if (st == AdlxStatus::Unusable) {
            lines.push_back(indent + std::string("PCIe: unavailable (ADLX unusable)"));
          }
        }
      }
    }
  }
  return lines;
}

std::vector<std::string> HardwareInfo::toLines() const {
  const bool hasPerGpu = !perGpuLines.empty();
  const bool hasPerNic = !perNicLines.empty();
  const bool hasPerDisk = !perDiskLines.empty();
  const std::string indent = " ";
  std::vector<std::string> lines;
  lines.reserve(48);

  lines.push_back("OS: " + (osPretty.empty() ? std::string(kUnknown) : osPretty));
  lines.push_back("Kernel: " + (kernelVersion.empty() ? std::string(kUnknown) : kernelVersion));

  lines.push_back("GPU Driver: " + (gpuDriver.empty() ? std::string(kUnknown) : gpuDriver));
  lines.push_back("CUDA: " + (cudaVersion.empty() ? std::string(kUnknown) : cudaVersion));
  lines.push_back("NVML: " + (nvmlVersion.empty() ? std::string(kUnknown) : nvmlVersion));
  {
    const auto a = adlxAvailability();
    if (a.available) {
      std::string s = a.backend.empty() ? std::string("available") : a.backend;
      if (!a.dll.empty()) s += " (" + a.dll + ")";
      lines.push_back("ADLX: " + s);
    } else {
      const auto st = adlxStatus();
      if (st == AdlxStatus::MissingDll) lines.push_back("ADLX: unavailable (amdadlx64.dll not found)");
      else lines.push_back("ADLX: unavailable");
    }
  }
  {
    const auto a = igclAvailability();
    if (a.available) {
      std::string s = a.backend.empty() ? std::string("available") : a.backend;
      if (!a.dll.empty()) s += " (" + a.dll + ")";
      lines.push_back("IGCL: " + s);
    } else {
      const auto st = igclStatus();
      if (st == IgclStatus::MissingDll) lines.push_back("IGCL: unavailable (ControlLib.dll not found)");
      else lines.push_back("IGCL: unavailable");
    }
  }
  lines.push_back("ROCm: " + (rocmVersion.empty() ? std::string(kUnknown) : rocmVersion));
  lines.push_back("OpenCL: " + (openclVersion.empty() ? std::string(kUnknown) : openclVersion));
  lines.push_back("Vulkan: " + (vulkanVersion.empty() ? std::string(kUnknown) : vulkanVersion));

  lines.push_back(std::string());

  lines.push_back("CPU: " + (cpuName.empty() ? std::string(kUnknown) : cpuName));
  lines.push_back(indent + "Physical cores: " + (cpuPhysicalCores.empty() ? std::string(kUnknown) : cpuPhysicalCores));
  lines.push_back(indent + "Logical cores: " + (cpuLogicalCores.empty() ? std::string(kUnknown) : cpuLogicalCores));
  lines.push_back(indent + "Cache L1: " + (cpuCacheL1.empty() ? std::string(kUnknown) : cpuCacheL1));
  lines.push_back(indent + "Cache L2: " + (cpuCacheL2.empty() ? std::string(kUnknown) : cpuCacheL2));
  lines.push_back(indent + "Cache L3: " + (cpuCacheL3.empty() ? std::string(kUnknown) : cpuCacheL3));
  lines.push_back(indent + "Instructions: " + (cpuInstructions.empty() ? std::string(kUnknown) : cpuInstructions));
  lines.push_back(indent + "RAM: " + (ramSummary.empty() ? std::string(kUnknown) : ramSummary));

  lines.push_back(std::string());

  if (!hasPerGpu) {
    lines.push_back("GPU: " + (gpuName.empty() ? std::string(kUnknown) : gpuName));
    lines.push_back("Memory: " + (vramSummary.empty() ? std::string(kUnknown) : vramSummary));
  } else {
    for (const auto& l : perGpuLines) lines.push_back(l);
  }

  // NPU (Neural Processing Unit) details after GPU.
  const bool hasPerNpu = !perNpuLines.empty();
  if (hasPerNpu) {
    lines.push_back(std::string());
    for (const auto& l : perNpuLines) lines.push_back(l);
  } else if (!npuSummary.empty() && npuSummary != kUnknown) {
    lines.push_back(std::string());
    lines.push_back("NPU: " + npuSummary);
  }

  if (hasPerNic || hasPerDisk) lines.push_back(std::string());
  for (const auto& l : perNicLines) lines.push_back(l);
  for (const auto& l : perDiskLines) lines.push_back(l);

  return lines;
}

HardwareInfo HardwareInfo::probe() {
  HardwareInfo info;
  info.osPretty = probeOsPrettyNameWindows();
  info.kernelVersion = probeKernelVersionWindows();
  info.cpuName = probeCpuNameWindows();
  info.cpuPhysicalCores = probeCpuPhysicalCoresWindows();
  info.cpuLogicalCores = probeCpuLogicalCoresWindows();
  info.cpuCacheL1 = kUnknown;
  info.cpuCacheL2 = kUnknown;
  info.cpuCacheL3 = kUnknown;
  info.cpuInstructions = kUnknown;
  info.ramSummary = probeRamSummary();
  info.gpuName = probeGpuName();
  info.gpuDriver = probeGpuDriver();
  info.perGpuLines = probePerGpuLinesNvidia();
  info.vramSummary = probeVramSummary();
  info.cudaVersion = probeCudaVersion();
  info.nvmlVersion = probeNvmlVersion();
  info.rocmVersion = kUnknown;
  info.openclVersion = kUnknown;
  info.vulkanVersion = kUnknown;

  if (info.gpuName.empty() || info.gpuName == kUnknown || info.perGpuLines.empty()) {
    const auto dxgiGpus = probeDxgiGpus();
    if (!dxgiGpus.empty()) {
      if (info.gpuName.empty() || info.gpuName == kUnknown) {
        info.gpuName = dxgiGpus[0].name.empty() ? std::string(kUnknown) : dxgiGpus[0].name;
      }

      if (info.vramSummary.empty() || info.vramSummary == kUnknown) {
        const std::uint64_t mem = dxgiGpus[0].dedicatedBytes ? dxgiGpus[0].dedicatedBytes : dxgiGpus[0].sharedBytes;
        info.vramSummary = mem ? formatGiB(mem) : std::string(kUnknown);
      }

      if (info.perGpuLines.empty()) {
        info.perGpuLines = probePerGpuLinesDxgi(dxgiGpus);
      }

      if (info.gpuDriver.empty() || info.gpuDriver == kUnknown) {
        const std::string vendor = vendorNameFromId(dxgiGpus[0].vendorId);
        info.gpuDriver = vendor.empty() ? std::string("windows") : vendor + " (windows)";
      }
    }
  }

  // Probe NPU (Intel/AMD Neural Processing Units)
  auto npuResult = probeNpuDevices();
  if (npuResult.status == NpuStatus::Available && !npuResult.devices.empty()) {
    // Build summary from first device
    info.npuSummary = npuResult.devices[0].name;
    if (npuResult.devices.size() > 1) {
      info.npuSummary += " (+" + std::to_string(npuResult.devices.size() - 1) + " more)";
    }

    // Build per-NPU lines
    for (std::size_t i = 0; i < npuResult.devices.size(); ++i) {
      const auto& npu = npuResult.devices[i];
      info.perNpuLines.push_back("NPU" + std::to_string(i) + ": " + npu.name);
      for (const auto& detail : npu.detailLines) {
        info.perNpuLines.push_back(detail);
      }
    }
  } else {
    info.npuSummary = kUnknown;
  }

  return info;
}

}  // namespace aiz
