#include <aiz/metrics/gpu_memory_util.h>

#include <aiz/metrics/nvidia_nvml.h>

#if defined(AI_Z_PLATFORM_LINUX)
#include <aiz/metrics/linux_gpu_sysfs.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>

#include <cstdint>
#endif

namespace aiz {

#if defined(_WIN32)
namespace {

std::optional<double> readWindowsVramUtil() {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    return std::nullopt;
  }

  IDXGIAdapter1* adapter = nullptr;
  if (factory->EnumAdapters1(0, &adapter) != S_OK) {
    factory->Release();
    return std::nullopt;
  }

  DXGI_ADAPTER_DESC1 desc{};
  adapter->GetDesc1(&desc);

  IDXGIAdapter3* adapter3 = nullptr;
  if (FAILED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
    adapter->Release();
    factory->Release();
    return std::nullopt;
  }

  DXGI_QUERY_VIDEO_MEMORY_INFO info{};
  if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
    adapter3->Release();
    adapter->Release();
    factory->Release();
    return std::nullopt;
  }

  const std::uint64_t total = desc.DedicatedVideoMemory ? desc.DedicatedVideoMemory : info.Budget;
  const std::uint64_t used = info.CurrentUsage;

  adapter3->Release();
  adapter->Release();
  factory->Release();

  if (!total) return std::nullopt;
  const double pct = (static_cast<double>(used) / static_cast<double>(total)) * 100.0;
  return pct;
}

}  // namespace
#endif

std::optional<Sample> GpuMemoryUtilCollector::sample() {
  const auto t = readNvmlTelemetry();
  if (t) {
    std::string label = "nvml";
    if (const auto n = nvmlGpuCount()) {
      label += " (" + std::to_string(*n) + ")";
    }

    return Sample{t->memUtilPct, "%", label};
  }

#if defined(AI_Z_PLATFORM_LINUX)
  if (const auto lt = readLinuxGpuTelemetry(0); lt && lt->vramUsedGiB && lt->vramTotalGiB && *lt->vramTotalGiB > 0.0) {
    const double pct = (*lt->vramUsedGiB / *lt->vramTotalGiB) * 100.0;
    return Sample{pct, "%", lt->source.empty() ? std::string("sysfs") : lt->source};
  }
#endif

#if defined(_WIN32)
  if (const auto pct = readWindowsVramUtil()) {
    return Sample{*pct, "%", "dxgi"};
  }
#endif

  return std::nullopt;
}

}  // namespace aiz
