#include "ncurses_bootprobe.h"

#include <utility>

namespace aiz::ncurses {

BootHardwareProbe::~BootHardwareProbe() {
  stop();
}

void BootHardwareProbe::start() {
  if (thread_.joinable()) return;
  ready_.store(false);

  thread_ = std::thread([this]() {
    HardwareInfo hw = HardwareInfo::probe();
    std::vector<std::string> lines = hw.toLines();

    {
      std::lock_guard<std::mutex> lk(mu_);
      hw_ = std::move(hw);
      lines_ = std::move(lines);
    }

    ready_.store(true);
  });
}

void BootHardwareProbe::stop() {
  if (thread_.joinable()) thread_.join();
}

bool BootHardwareProbe::tryConsume(HardwareInfo& outHw, std::vector<std::string>& outLines) {
  if (!ready_.load()) return false;

  {
    std::lock_guard<std::mutex> lk(mu_);
    outHw = std::move(hw_);
    outLines = std::move(lines_);
  }

  ready_.store(false);
  return true;
}

}  // namespace aiz::ncurses
