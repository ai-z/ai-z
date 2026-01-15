#include <aiz/tui/ui.h>

#include <aiz/hw/hardware_info.h>
#include <aiz/metrics/cpu_usage.h>
#include <aiz/metrics/disk_bandwidth.h>
#include <aiz/metrics/gpu_usage.h>
#include <aiz/metrics/pcie_bandwidth.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <thread>

#if defined(_WIN32)
#include <conio.h>
#define NOMINMAX
#include <windows.h>
#endif

namespace aiz {

#if defined(_WIN32)
static void enableVirtualTerminalIfPossible() {
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut == INVALID_HANDLE_VALUE) return;

  DWORD mode = 0;
  if (!GetConsoleMode(hOut, &mode)) return;

  mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
  (void)SetConsoleMode(hOut, mode);
}
#endif

static void clearScreen() {
  // Works on most terminals; on legacy Windows consoles this may be ignored.
  std::fputs("\x1b[2J\x1b[H", stdout);
}

class WindowsConsoleUi final : public Ui {
public:
  int run(Config& cfg, bool debugMode) override {
    (void)debugMode;

#if defined(_WIN32)
    enableVirtualTerminalIfPossible();
#endif

    std::cout << "ai-z (Windows console UI)\n";
    std::cout << "Press 'q' to quit.\n\n";

    const auto hw = HardwareInfo::probe();

    CpuUsageCollector cpuCol;
    GpuUsageCollector gpuCol;
    DiskBandwidthCollector diskCol;
    PcieBandwidthCollector pcieCol;

    while (true) {
#if defined(_WIN32)
      if (_kbhit()) {
        const int ch = _getch();
        if (ch == 'q' || ch == 'Q') return 0;
      }
#endif

      clearScreen();
      std::cout << "ai-z (Windows console UI)\n";
      std::cout << "Press 'q' to quit.\n\n";

      for (const auto& line : hw.toLines()) {
        std::cout << line << "\n";
      }
      std::cout << "\n";

      auto printSample = [](const char* name, const std::optional<Sample>& s) {
        if (!s) {
          std::cout << name << ": unavailable\n";
          return;
        }
        std::cout << name << ": " << s->value;
        if (!s->unit.empty()) std::cout << " " << s->unit;
        if (!s->label.empty()) std::cout << " (" << s->label << ")";
        std::cout << "\n";
      };

      if (cfg.showCpu) printSample("CPU", cpuCol.sample());
      if (cfg.showGpu) printSample("GPU", gpuCol.sample());
      if (cfg.showDisk) printSample("Disk", diskCol.sample());
      if (cfg.showPcie) printSample("PCIe", pcieCol.sample());

      std::cout.flush();
      std::this_thread::sleep_for(std::chrono::milliseconds(cfg.refreshMs));
    }
  }
};

std::unique_ptr<Ui> makeUi() {
  return std::make_unique<WindowsConsoleUi>();
}

}  // namespace aiz
