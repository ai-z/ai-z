#include <aiz/metrics/cpu_usage.h>

#include <aiz/platform/metrics/cpu.h>

namespace aiz {

std::optional<Sample> CpuUsageCollector::sample() {
  const auto times = platform::readCpuTimes();
  if (!times) return std::nullopt;

  const std::uint64_t idle = times->idle;
  const std::uint64_t total = times->total;

  if (!hasPrev_) {
    hasPrev_ = true;
    prevIdle_ = idle;
    prevTotal_ = total;
    return Sample{0.0, "%", "warming"};
  }

  const std::uint64_t idleDelta = idle - prevIdle_;
  const std::uint64_t totalDelta = total - prevTotal_;
  prevIdle_ = idle;
  prevTotal_ = total;

  if (totalDelta == 0) {
    return Sample{0.0, "%", ""};
  }

  const double busy = 100.0 * (1.0 - (static_cast<double>(idleDelta) / static_cast<double>(totalDelta)));
  return Sample{busy, "%", ""};
}

std::optional<Sample> CpuMaxCoreUsageCollector::sample() {
  const auto perCore = platform::readPerCoreCpuTimes();
  if (!perCore) return std::nullopt;

  std::vector<std::uint64_t> idle;
  std::vector<std::uint64_t> total;
  idle.reserve(perCore->size());
  total.reserve(perCore->size());
  for (const auto& t : *perCore) {
    idle.push_back(t.idle);
    total.push_back(t.total);
  }

  if (!hasPrev_ || prevIdle_.size() != idle.size() || prevTotal_.size() != total.size()) {
    hasPrev_ = true;
    prevIdle_ = std::move(idle);
    prevTotal_ = std::move(total);
    return Sample{0.0, "%", "warming"};
  }

  double maxBusy = 0.0;
  for (std::size_t i = 0; i < idle.size(); ++i) {
    const std::uint64_t idleDelta = idle[i] - prevIdle_[i];
    const std::uint64_t totalDelta = total[i] - prevTotal_[i];
    if (totalDelta == 0) continue;
    const double busy = 100.0 * (1.0 - (static_cast<double>(idleDelta) / static_cast<double>(totalDelta)));
    if (busy > maxBusy) maxBusy = busy;
  }

  prevIdle_ = std::move(idle);
  prevTotal_ = std::move(total);

  return Sample{maxBusy, "%", ""};
}

}  // namespace aiz
