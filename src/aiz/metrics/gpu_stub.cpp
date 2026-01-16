#include <aiz/metrics/gpu_usage.h>

#include <aiz/metrics/linux_gpu_sysfs.h>
#include <aiz/metrics/nvidia_nvml.h>

namespace aiz {

std::optional<Sample> GpuUsageCollector::sample() {
  // NVIDIA via NVML (preferred).
  if (const auto t = readNvmlTelemetry()) {
    std::string label = "nvml";
    if (const auto n = nvmlGpuCount()) {
      label += " (" + std::to_string(*n) + ")";
    }
    return Sample{t->gpuUtilPct, "%", label};
  }

  // AMD/Intel via Linux sysfs (best-effort).
  if (const auto t = readLinuxGpuTelemetry(0); t && t->utilPct) {
    return Sample{*t->utilPct, "%", t->source.empty() ? std::string("sysfs") : t->source};
  }

  return std::nullopt;
}

}  // namespace aiz
