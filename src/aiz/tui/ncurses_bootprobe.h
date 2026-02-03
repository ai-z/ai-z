#pragma once

#include <aiz/hw/hardware_info.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace aiz::ncurses {

class BootHardwareProbe {
public:
  BootHardwareProbe() = default;
  ~BootHardwareProbe();

  BootHardwareProbe(const BootHardwareProbe&) = delete;
  BootHardwareProbe& operator=(const BootHardwareProbe&) = delete;

  void start();
  void stop();

  // If the probe finished, moves the results into `outHw`/`outLines` and returns true.
  // Can only return true once per start().
  bool tryConsume(HardwareInfo& outHw, std::vector<std::string>& outLines);

private:
  std::atomic<bool> ready_{false};
  std::mutex mu_;
  HardwareInfo hw_;
  std::vector<std::string> lines_;
  std::thread thread_;
};

}  // namespace aiz::ncurses
