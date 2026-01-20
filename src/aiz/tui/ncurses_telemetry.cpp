#include "ncurses_telemetry.h"

#include <aiz/metrics/linux_gpu_sysfs.h>
#include <aiz/metrics/nvidia_nvml.h>

#include <ios>
#include <optional>
#include <sstream>
#include <string>

namespace aiz::ncurses {

namespace {
std::string fmt1(double v) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(1);
  oss << v;
  return oss.str();
}

std::string fmt0(double v) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(0);
  oss << v;
  return oss.str();
}
}  // namespace

std::string formatRamText(const std::optional<RamUsage>& ram) {
  if (!ram) return "--";
  return fmt1(ram->usedGiB) + "/" + fmt1(ram->totalGiB) + "G(" + fmt0(ram->usedPct) + "%)";
}

std::optional<GpuTelemetry> readGpuTelemetryPreferNvml(unsigned int index) {
  GpuTelemetry t;
  bool any = false;

  if (const auto nv = readNvmlTelemetryForGpu(index)) {
    t.utilPct = nv->gpuUtilPct;
    t.memUtilPct = nv->memUtilPct;
    t.vramUsedGiB = nv->memUsedGiB;
    t.vramTotalGiB = nv->memTotalGiB;
    t.watts = nv->powerWatts;
    t.tempC = nv->tempC;
    t.pstate = nv->pstate;
    t.source = "nvml";
    any = true;
  }

  // Query PCIe link info independently: telemetry calls can fail while link queries still work.
  if (const auto link = readNvmlPcieLinkForGpu(index)) {
    t.pcieLinkWidth = link->width;
    t.pcieLinkGen = link->generation;
    any = true;
  }

  if (any) return t;

  // AMD/Intel via Linux sysfs (best-effort).
  if (const auto lt = readLinuxGpuTelemetry(index)) {
    t.utilPct = lt->utilPct;
    t.vramUsedGiB = lt->vramUsedGiB;
    t.vramTotalGiB = lt->vramTotalGiB;
    t.watts = lt->watts;
    t.tempC = lt->tempC;
    t.pstate = lt->pstate;
    t.source = lt->source;
    return t;
  }

  return std::nullopt;
}

}  // namespace aiz::ncurses
