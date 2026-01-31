// SPDX-License-Identifier: MIT
// GPU sampler - unified single-shot GPU telemetry for Python bindings

#include <aiz/metrics/gpu_sampler.h>
#include <aiz/metrics/nvidia_nvml.h>
#include <aiz/metrics/linux_gpu_sysfs.h>
#include <aiz/metrics/amd_rocm_smi.h>

#include <algorithm>

namespace aiz {

unsigned int gpuCount() {
    // Try NVML first
    auto nvmlCount = nvmlGpuCount();
    if (nvmlCount && *nvmlCount > 0) {
        return *nvmlCount;
    }

    // Fall back to Linux sysfs enumeration
    return linuxGpuCount();
}

std::optional<GpuTelemetry> sampleGpuTelemetry(unsigned int index) {
    // Try NVML first (for NVIDIA GPUs)
    auto nvmlCount = nvmlGpuCount();
    if (nvmlCount && index < *nvmlCount) {
        auto nvml = readNvmlTelemetryForGpu(index);
        if (nvml) {
            GpuTelemetry t;
            t.index = index;
            t.vendor = GpuVendor::Nvidia;
            t.source = "nvml";

            // Get GPU name
            auto name = readNvmlGpuNameForGpu(index);
            t.name = name.value_or("NVIDIA GPU");

            t.utilPct = nvml->gpuUtilPct;
            t.vramUsedGiB = nvml->memUsedGiB;
            t.vramTotalGiB = nvml->memTotalGiB;
            t.powerWatts = nvml->powerWatts;
            t.tempC = nvml->tempC;
            t.pstate = nvml->pstate;

            if (nvml->gpuClockMHz > 0) t.gpuClockMHz = nvml->gpuClockMHz;
            if (nvml->memClockMHz > 0) t.memClockMHz = nvml->memClockMHz;

            return t;
        }
    }

    // Try Linux sysfs enumeration
    auto gpus = enumerateLinuxGpus();
    if (index >= gpus.size()) {
        return std::nullopt;
    }

    const auto& dev = gpus[index];
    GpuTelemetry t;
    t.index = index;
    t.vendor = dev.vendor;
    t.name = dev.driver + " (" + dev.drmCard + ")";

    // Try ROCm SMI for AMD GPUs
    if (dev.vendor == GpuVendor::Amd && !dev.pciSlotName.empty()) {
        auto rocm = readRocmSmiTelemetryForPciBusId(dev.pciSlotName);
        if (rocm) {
            t.source = "rocm-smi";
            t.utilPct = rocm->utilPct;
            t.vramUsedGiB = rocm->vramUsedGiB;
            t.vramTotalGiB = rocm->vramTotalGiB;
            t.powerWatts = rocm->watts;
            t.tempC = rocm->tempC;
            t.pstate = rocm->pstate;
            return t;
        }
    }

    // Fall back to sysfs
    auto sysfs = readLinuxGpuTelemetry(index);
    if (sysfs) {
        t.source = sysfs->source;
        t.utilPct = sysfs->utilPct;
        t.vramUsedGiB = sysfs->vramUsedGiB;
        t.vramTotalGiB = sysfs->vramTotalGiB;
        t.powerWatts = sysfs->watts;
        t.tempC = sysfs->tempC;
        t.pstate = sysfs->pstate;
        return t;
    }

    // Return partial info even if telemetry unavailable
    t.source = "sysfs";
    return t;
}

std::vector<GpuTelemetry> sampleAllGpuTelemetry() {
    std::vector<GpuTelemetry> result;
    unsigned int count = gpuCount();
    result.reserve(count);

    for (unsigned int i = 0; i < count; ++i) {
        auto t = sampleGpuTelemetry(i);
        if (t) {
            result.push_back(std::move(*t));
        }
    }

    return result;
}

}  // namespace aiz
