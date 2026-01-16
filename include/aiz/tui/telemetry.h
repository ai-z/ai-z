#pragma once

#include <aiz/metrics/collectors.h>

#include <optional>
#include <string>

namespace aiz {

struct TelemetrySnapshot {
  std::optional<Sample> cpu;
  std::optional<Sample> disk;
  std::optional<Sample> diskRead;
  std::optional<Sample> diskWrite;
  std::optional<Sample> netRx;
  std::optional<Sample> netTx;
  std::optional<Sample> gpu;
  std::optional<Sample> pcieRx;
  std::optional<Sample> pcieTx;

  // Percent (0..100).
  std::optional<Sample> ramPct;
  std::optional<Sample> vramPct;

  // Pre-formatted memory strings are okay for now; we can refactor to typed later.
  std::string ramText;
};

}  // namespace aiz
