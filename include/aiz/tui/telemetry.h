#pragma once

#include <aiz/metrics/collectors.h>

#include <optional>
#include <string>

namespace aiz {

struct GpuTelemetrySnapshot {
  // Percent (0..100).
  std::optional<double> utilPct;
  std::optional<double> memUtilPct;

  // Absolute memory.
  std::optional<double> vramUsedGiB;
  std::optional<double> vramTotalGiB;

  // Board sensors.
  std::optional<double> watts;
  std::optional<double> tempC;

  // Clocks (MHz) when available.
  std::optional<unsigned int> gpuClockMHz;
  std::optional<unsigned int> memClockMHz;

  // Encoder/decoder utilization (0..100).
  std::optional<double> encoderUtilPct;
  std::optional<double> decoderUtilPct;

  // Best-effort textual state.
  std::string pstate;

  // PCIe link info (if known).
  std::optional<int> pcieLinkWidth;
  std::optional<int> pcieLinkGen;

  // Short reason when PCIe link fields are unavailable (e.g. "ADLX missing").
  std::string pcieLinkNote;
};

struct TelemetrySnapshot {
  std::optional<Sample> cpu;
  std::optional<Sample> cpuMax;
  std::optional<Sample> disk;
  std::optional<Sample> diskRead;
  std::optional<Sample> diskWrite;
  std::optional<Sample> netRx;
  std::optional<Sample> netTx;
  std::optional<Sample> gpu;
  std::optional<Sample> gpuMemUtil;
  std::optional<Sample> gpuClock;
  std::optional<Sample> gpuMemClock;
  std::optional<Sample> gpuEnc;
  std::optional<Sample> gpuDec;
  std::optional<Sample> pcieRx;
  std::optional<Sample> pcieTx;

  // Optional per-GPU details for header/status rendering.
  std::vector<GpuTelemetrySnapshot> gpus;

  // Percent (0..100).
  std::optional<Sample> ramPct;
  std::optional<Sample> vramPct;

  // Pre-formatted memory strings are okay for now; we can refactor to typed later.
  std::string ramText;
  std::string vramText;
};

}  // namespace aiz
