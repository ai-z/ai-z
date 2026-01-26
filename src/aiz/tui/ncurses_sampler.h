#pragma once

#include "ncurses_telemetry.h"

#include <aiz/metrics/nvidia_nvml.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace aiz::ncurses {

class GpuTelemetrySampler {
public:
  GpuTelemetrySampler(unsigned int gpuCount, bool hasNvml);
  ~GpuTelemetrySampler();

  GpuTelemetrySampler(const GpuTelemetrySampler&) = delete;
  GpuTelemetrySampler& operator=(const GpuTelemetrySampler&) = delete;

  void start();
  void stop();

  void snapshot(std::vector<std::optional<GpuTelemetry>>& outGpu,
                std::optional<NvmlPcieThroughput>& outPcie) const;

  static bool isPcieThroughputSupported();

private:
  void run();

  unsigned int gpuCount_ = 0;
  bool hasNvml_ = false;

  mutable std::mutex mu_;
  std::vector<std::optional<GpuTelemetry>> cachedGpu_;
  std::optional<NvmlPcieThroughput> cachedPcie_;

  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace aiz::ncurses
