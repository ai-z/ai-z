#include <aiz/metrics/gpu_usage.h>

#include <aiz/metrics/nvidia_nvml.h>

namespace aiz {

std::optional<Sample> GpuUsageCollector::sample() {
  // NVIDIA via NVML (preferred). Other vendors TBD.
  const auto t = readNvmlTelemetry();
  if (!t) return std::nullopt;
  std::string label = "nvml";
  if (const auto n = nvmlGpuCount()) {
    label += " (" + std::to_string(*n) + ")";
  }
  return Sample{t->gpuUtilPct, "%", label};
}

}  // namespace aiz
