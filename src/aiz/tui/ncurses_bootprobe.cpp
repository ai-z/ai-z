#include "ncurses_bootprobe.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#endif

#include <utility>

namespace aiz::ncurses {

BootHardwareProbe::~BootHardwareProbe() {
  stop();
}

void BootHardwareProbe::start() {
  if (thread_.joinable()) return;
  ready_.store(false);

  thread_ = std::thread([this]() {
#if defined(_WIN32)
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInited = SUCCEEDED(hrCo);
#endif

    HardwareInfo hw = HardwareInfo::probe();
    std::vector<std::string> lines = hw.toLines();

    {
      std::lock_guard<std::mutex> lk(mu_);
      hw_ = std::move(hw);
      lines_ = std::move(lines);
    }

    ready_.store(true);

#if defined(_WIN32)
    if (coInited) {
      CoUninitialize();
    }
#endif
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
