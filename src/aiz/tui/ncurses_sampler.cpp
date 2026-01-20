#include "ncurses_sampler.h"

#include <aiz/metrics/linux_gpu_sysfs.h>
#include <aiz/metrics/nvidia_nvml.h>

#include <chrono>
#include <cstddef>
#include <thread>

namespace aiz::ncurses {

GpuTelemetrySampler::GpuTelemetrySampler(unsigned int gpuCount, bool hasNvml)
    : gpuCount_(gpuCount), hasNvml_(hasNvml) {
  cachedGpu_.resize(gpuCount_);
}

GpuTelemetrySampler::~GpuTelemetrySampler() {
  stop();
}

void GpuTelemetrySampler::start() {
  if (thread_.joinable()) return;
  stop_.store(false);
  thread_ = std::thread([this]() { run(); });
}

void GpuTelemetrySampler::stop() {
  stop_.store(true);
  if (thread_.joinable()) thread_.join();
}

void GpuTelemetrySampler::snapshot(std::vector<std::optional<GpuTelemetry>>& outGpu,
                                  std::optional<NvmlPcieThroughput>& outPcie) const {
  std::lock_guard<std::mutex> lk(mu_);
  outGpu = cachedGpu_;
  outPcie = cachedPcie_;
}

void GpuTelemetrySampler::run() {
  while (!stop_.load()) {
    std::vector<std::optional<GpuTelemetry>> nextGpu;
    nextGpu.resize(gpuCount_);

    for (unsigned int i = 0; i < gpuCount_; ++i) {
      if (hasNvml_) {
        nextGpu[static_cast<std::size_t>(i)] = readGpuTelemetryPreferNvml(i);
      } else {
        // Avoid NVML wrapper overhead on non-NVIDIA systems.
        if (const auto lt = readLinuxGpuTelemetry(i)) {
          GpuTelemetry t;
          t.utilPct = lt->utilPct;
          t.vramUsedGiB = lt->vramUsedGiB;
          t.vramTotalGiB = lt->vramTotalGiB;
          t.watts = lt->watts;
          t.tempC = lt->tempC;
          t.pstate = lt->pstate;
          t.source = lt->source;
          nextGpu[static_cast<std::size_t>(i)] = t;
        }
      }
    }

    const auto nextPcie = hasNvml_ ? readNvmlPcieThroughput() : std::nullopt;

    {
      std::lock_guard<std::mutex> lk(mu_);
      cachedGpu_ = std::move(nextGpu);
      cachedPcie_ = nextPcie;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

}  // namespace aiz::ncurses
