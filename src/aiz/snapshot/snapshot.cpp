// SPDX-License-Identifier: MIT
// Snapshot implementation - aggregates telemetry from all sources

#include <aiz/snapshot/snapshot.h>
#include <aiz/snapshot/json_writer.h>
#include <aiz/metrics/gpu_sampler.h>
#include <aiz/metrics/cpu_usage.h>
#include <aiz/metrics/ram_usage.h>
#include <aiz/metrics/disk_bandwidth.h>
#include <aiz/metrics/network_bandwidth.h>
#include <aiz/metrics/npu_info.h>
#include <aiz/hw/hardware_info.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <csignal>
  #include <unistd.h>
#endif

namespace aiz {

namespace {

// Format helpers
std::string formatPercent(double pct) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(0) << pct << "%";
  return ss.str();
}

std::string formatMHz(unsigned int mhz) {
  return std::to_string(mhz) + "MHz";
}

std::string formatCelsius(double temp) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(0) << temp << "C";
  return ss.str();
}

std::string formatWatts(double watts) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(0) << watts << "W";
  return ss.str();
}

std::string formatGiB(double gib) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(1) << gib << "GiB";
  return ss.str();
}

std::string formatBandwidth(double value, const std::string& unit) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(1) << value << " " << unit;
  return ss.str();
}

std::string getCurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::ostringstream ss;
  ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

// Volatile flag for clean shutdown on Ctrl+C
volatile bool g_running = true;

#if defined(_WIN32)
BOOL WINAPI consoleHandler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
    g_running = false;
    return TRUE;
  }
  return FALSE;
}
#else
void signalHandler(int) {
  g_running = false;
}
#endif

}  // namespace

SystemSnapshot captureSystemSnapshot() {
  SystemSnapshot snapshot;
  snapshot.timestamp = getCurrentTimestamp();

  // Collect GPU telemetry
  auto gpus = sampleAllGpuTelemetry();
  for (const auto& gpu : gpus) {
    DeviceSnapshot dev;
    dev.device_type = "gpu";
    dev.device_name = gpu.name;

    if (gpu.gpuClockMHz) {
      dev.gpu_clock = formatMHz(*gpu.gpuClockMHz);
    }
    if (gpu.memClockMHz) {
      dev.mem_clock = formatMHz(*gpu.memClockMHz);
    }
    if (gpu.tempC) {
      dev.temp = formatCelsius(*gpu.tempC);
    }
    if (gpu.powerWatts) {
      dev.power_draw = formatWatts(*gpu.powerWatts);
    }
    if (gpu.utilPct) {
      dev.gpu_util = formatPercent(*gpu.utilPct);
    }
    if (gpu.vramUsedGiB && gpu.vramTotalGiB) {
      dev.vram_used = formatGiB(*gpu.vramUsedGiB);
      dev.vram_total = formatGiB(*gpu.vramTotalGiB);
      if (*gpu.vramTotalGiB > 0) {
        double memPct = (*gpu.vramUsedGiB / *gpu.vramTotalGiB) * 100.0;
        dev.mem_util = formatPercent(memPct);
      }
    }

    snapshot.devices.push_back(std::move(dev));
  }

  // Collect CPU telemetry
  {
    static CpuUsageCollector cpuCollector;
    auto cpuSample = cpuCollector.sample();

    // Get CPU name from hardware info
    auto hwInfo = HardwareInfo::probe();

    DeviceSnapshot dev;
    dev.device_type = "cpu";
    dev.device_name = hwInfo.cpuName.empty() ? "CPU" : hwInfo.cpuName;

    if (cpuSample) {
      dev.cpu_util = formatPercent(cpuSample->value);
    }
    if (!hwInfo.cpuLogicalCores.empty()) {
      dev.core_count = hwInfo.cpuLogicalCores;
    }

    snapshot.devices.push_back(std::move(dev));
  }

  // Collect RAM telemetry
  {
    auto ram = readRamUsage();
    if (ram) {
      DeviceSnapshot dev;
      dev.device_type = "ram";
      dev.device_name = "System Memory";
      dev.ram_used = formatGiB(ram->usedGiB);
      dev.ram_total = formatGiB(ram->totalGiB);
      dev.ram_util = formatPercent(ram->usedPct);
      snapshot.devices.push_back(std::move(dev));
    }
  }

  // Collect NPU telemetry
  {
    auto npuAvail = probeNpuDevices();
    for (const auto& npu : npuAvail.devices) {
      DeviceSnapshot dev;
      dev.device_type = "npu";
      dev.device_name = npu.name;
      dev.npu_vendor = npuVendorToString(npu.vendor);
      if (npu.peakTops) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << *npu.peakTops << " TOPS";
        dev.peak_tops = ss.str();
      }
      if (!npu.driverVersion.empty()) {
        dev.driver_version = npu.driverVersion;
      }
      snapshot.devices.push_back(std::move(dev));
    }
  }

  // Collect Disk telemetry
  // Note: Disk bandwidth requires delta between samples; first call may return nullopt
  {
    static DiskBandwidthCollector diskReadCollector(DiskBandwidthMode::Read);
    static DiskBandwidthCollector diskWriteCollector(DiskBandwidthMode::Write);

    auto readSample = diskReadCollector.sample();
    auto writeSample = diskWriteCollector.sample();

    DeviceSnapshot dev;
    dev.device_type = "disk";
    dev.device_name = "Disk I/O";

    if (readSample) {
      dev.read_bw = formatBandwidth(readSample->value, readSample->unit);
    }
    if (writeSample) {
      dev.write_bw = formatBandwidth(writeSample->value, writeSample->unit);
    }

    // Only add if we have some data
    if (readSample || writeSample) {
      snapshot.devices.push_back(std::move(dev));
    }
  }

  // Collect Network telemetry
  {
    static NetworkBandwidthCollector netRxCollector(NetworkBandwidthMode::Rx);
    static NetworkBandwidthCollector netTxCollector(NetworkBandwidthMode::Tx);

    auto rxSample = netRxCollector.sample();
    auto txSample = netTxCollector.sample();

    DeviceSnapshot dev;
    dev.device_type = "network";
    dev.device_name = "Network I/O";

    if (rxSample) {
      dev.rx_bw = formatBandwidth(rxSample->value, rxSample->unit);
    }
    if (txSample) {
      dev.tx_bw = formatBandwidth(txSample->value, txSample->unit);
    }

    // Only add if we have some data
    if (rxSample || txSample) {
      snapshot.devices.push_back(std::move(dev));
    }
  }

  return snapshot;
}

std::string snapshotToJson(const SystemSnapshot& snapshot) {
  json::ObjectBuilder root;
  root.addString("timestamp", snapshot.timestamp);

  json::ArrayBuilder devicesArray;
  for (const auto& dev : snapshot.devices) {
    json::ObjectBuilder devObj;
    devObj.addString("device_type", dev.device_type);
    devObj.addString("device_name", dev.device_name);

    // GPU fields
    devObj.addOptionalString("gpu_clock", dev.gpu_clock);
    devObj.addOptionalString("mem_clock", dev.mem_clock);
    devObj.addOptionalString("temp", dev.temp);
    devObj.addOptionalString("fan_speed", dev.fan_speed);
    devObj.addOptionalString("power_draw", dev.power_draw);
    devObj.addOptionalString("gpu_util", dev.gpu_util);
    devObj.addOptionalString("mem_util", dev.mem_util);
    devObj.addOptionalString("vram_used", dev.vram_used);
    devObj.addOptionalString("vram_total", dev.vram_total);

    // CPU fields
    devObj.addOptionalString("cpu_util", dev.cpu_util);
    devObj.addOptionalString("core_count", dev.core_count);

    // NPU fields
    devObj.addOptionalString("npu_vendor", dev.npu_vendor);
    devObj.addOptionalString("peak_tops", dev.peak_tops);
    devObj.addOptionalString("driver_version", dev.driver_version);

    // Disk fields
    devObj.addOptionalString("read_bw", dev.read_bw);
    devObj.addOptionalString("write_bw", dev.write_bw);
    devObj.addOptionalString("total_bw", dev.total_bw);

    // Network fields
    devObj.addOptionalString("rx_bw", dev.rx_bw);
    devObj.addOptionalString("tx_bw", dev.tx_bw);

    // RAM fields
    devObj.addOptionalString("ram_used", dev.ram_used);
    devObj.addOptionalString("ram_total", dev.ram_total);
    devObj.addOptionalString("ram_util", dev.ram_util);

    devicesArray.addRaw(devObj.build());
  }

  // Manually construct the final JSON with devices array
  std::ostringstream ss;
  ss << "{\"timestamp\": \"" << json::escape(snapshot.timestamp) << "\", ";
  ss << "\"devices\": " << devicesArray.build() << "}";

  return ss.str();
}

void clearScreen() {
#if defined(_WIN32)
  // Use ANSI escape codes (supported in Windows 10+)
  // Fall back to system("cls") if needed
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (GetConsoleMode(hOut, &mode)) {
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) {
      std::fputs("\033[2J\033[H", stdout);
      std::fflush(stdout);
      return;
    }
  }
  // Fallback for older Windows
  std::system("cls");
#else
  std::fputs("\033[2J\033[H", stdout);
  std::fflush(stdout);
#endif
}

int runSnapshotLoop(int intervalMs) {
  // Set up signal handlers for clean shutdown
#if defined(_WIN32)
  SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
  struct sigaction sa{};
  sa.sa_handler = signalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
#endif

  g_running = true;

  // Prime the collectors by taking an initial sample
  // (bandwidth collectors need two samples to compute delta)
  (void)captureSystemSnapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  while (g_running) {
    clearScreen();
    auto snapshot = captureSystemSnapshot();
    std::cout << snapshotToJson(snapshot) << std::endl;
    
    // Sleep in small increments to allow responsive Ctrl+C handling
    int remaining = intervalMs;
    while (remaining > 0 && g_running) {
      int sleepMs = (remaining > 100) ? 100 : remaining;
      std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
      remaining -= sleepMs;
    }
  }

  std::cout << "\n";  // Clean line after Ctrl+C
  return 0;
}

}  // namespace aiz
