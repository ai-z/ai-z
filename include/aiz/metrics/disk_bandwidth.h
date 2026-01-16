#pragma once

#include <aiz/metrics/collectors.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#if defined(_WIN32)
// Keep these includes here so the collector can own PDH handles per instance.
// This avoids static globals which break when we want separate read/write collectors.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#endif

namespace aiz {

enum class DiskBandwidthMode {
  Total,
  Read,
  Write,
};

class DiskBandwidthCollector final : public ICollector {
public:
  explicit DiskBandwidthCollector(std::string devicePrefix = "");
  DiskBandwidthCollector(DiskBandwidthMode mode, std::string devicePrefix = "");

  ~DiskBandwidthCollector() override;

  std::string name() const override { return "Disk bandwidth"; }
  std::optional<Sample> sample() override;

private:
  DiskBandwidthMode mode_ = DiskBandwidthMode::Total;
  std::string devicePrefix_;
  bool hasPrev_ = false;
  std::uint64_t prevBytes_ = 0;
  std::chrono::steady_clock::time_point prevTime_{};

#if defined(_WIN32)
  bool initialized_ = false;
  bool warmed_ = false;
  HQUERY query_ = nullptr;
  HCOUNTER counter_ = nullptr;
#endif
};

}  // namespace aiz
