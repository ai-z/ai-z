#include <aiz/metrics/gpu_usage.h>

#if defined(AI_Z_PLATFORM_LINUX)
#include <aiz/metrics/linux_gpu_sysfs.h>
#endif
#include <aiz/metrics/nvidia_nvml.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>
#endif

namespace aiz {

#if defined(_WIN32)
namespace {

bool containsEngType(std::wstring_view name, std::wstring_view token) {
  return name.find(token) != std::wstring_view::npos;
}

std::optional<double> readWindowsGpuUtilization() {
  static HQUERY query = nullptr;
  static HCOUNTER counter = nullptr;
  static bool initialized = false;
  static bool primed = false;

  if (!initialized) {
    initialized = true;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return std::nullopt;
    if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS) {
      PdhCloseQuery(query);
      query = nullptr;
      return std::nullopt;
    }
  }

  if (!query || !counter) return std::nullopt;

  if (PdhCollectQueryData(query) != ERROR_SUCCESS) return std::nullopt;
  if (!primed) {
    primed = true;
    return std::nullopt;
  }

  DWORD bufferSize = 0;
  DWORD itemCount = 0;
  const DWORD flags = PDH_FMT_DOUBLE;
  PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, flags, &bufferSize, &itemCount, nullptr);
  if (status != PDH_MORE_DATA || bufferSize == 0 || itemCount == 0) return std::nullopt;

  std::vector<std::byte> buffer(bufferSize);
  auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
  status = PdhGetFormattedCounterArrayW(counter, flags, &bufferSize, &itemCount, items);
  if (status != ERROR_SUCCESS) return std::nullopt;

  double total = 0.0;
  for (DWORD i = 0; i < itemCount; ++i) {
    const auto* name = items[i].szName;
    if (!name) continue;
    const std::wstring_view inst(name);
    if (containsEngType(inst, L"engtype_3D") ||
        containsEngType(inst, L"engtype_Copy") ||
        containsEngType(inst, L"engtype_Compute") ||
        containsEngType(inst, L"engtype_VideoDecode") ||
        containsEngType(inst, L"engtype_VideoEncode") ||
        containsEngType(inst, L"engtype_VideoProcessing")) {
      const double v = items[i].FmtValue.doubleValue;
      if (std::isfinite(v)) total += v;
    }
  }

  total = std::clamp(total, 0.0, 100.0);
  return total;
}

}  // namespace
#endif

std::optional<Sample> GpuUsageCollector::sample() {
  // NVIDIA via NVML (preferred).
  if (const auto t = readNvmlTelemetry()) {
    std::string label = "nvml";
    if (const auto n = nvmlGpuCount()) {
      label += " (" + std::to_string(*n) + ")";
    }
    return Sample{t->gpuUtilPct, "%", label};
  }

#if defined(AI_Z_PLATFORM_LINUX)
  // AMD/Intel via Linux sysfs (best-effort).
  if (const auto t = readLinuxGpuTelemetry(0); t && t->utilPct) {
    return Sample{*t->utilPct, "%", t->source.empty() ? std::string("sysfs") : t->source};
  }
#endif

#if defined(_WIN32)
  if (const auto util = readWindowsGpuUtilization()) {
    return Sample{*util, "%", "pdh"};
  }
#endif

  return std::nullopt;
}

}  // namespace aiz
