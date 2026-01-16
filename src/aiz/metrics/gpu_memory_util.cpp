#include <aiz/metrics/gpu_memory_util.h>

#include <aiz/metrics/nvidia_nvml.h>

namespace aiz {

std::optional<Sample> GpuMemoryUtilCollector::sample() {
  const auto t = readNvmlTelemetry();
  if (!t) return std::nullopt;

  std::string label = "nvml";
  if (const auto n = nvmlGpuCount()) {
    label += " (" + std::to_string(*n) + ")";
  }

  return Sample{t->memUtilPct, "%", label};
}

}  // namespace aiz
