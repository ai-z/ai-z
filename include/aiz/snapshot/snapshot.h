// SPDX-License-Identifier: MIT
// Snapshot - JSON-serializable system telemetry snapshots

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace aiz {

/// Represents a single device's telemetry snapshot.
/// All values are pre-formatted strings for JSON output.
struct DeviceSnapshot {
  std::string device_type;     // "gpu", "cpu", "npu", "disk", "network", "ram"
  std::string device_name;

  // GPU-specific
  std::optional<std::string> gpu_clock;
  std::optional<std::string> mem_clock;
  std::optional<std::string> temp;
  std::optional<std::string> fan_speed;
  std::optional<std::string> power_draw;
  std::optional<std::string> gpu_util;
  std::optional<std::string> mem_util;
  std::optional<std::string> vram_used;
  std::optional<std::string> vram_total;

  // CPU-specific
  std::optional<std::string> cpu_util;
  std::optional<std::string> core_count;

  // NPU-specific
  std::optional<std::string> npu_vendor;
  std::optional<std::string> peak_tops;
  std::optional<std::string> driver_version;

  // Disk-specific
  std::optional<std::string> read_bw;
  std::optional<std::string> write_bw;
  std::optional<std::string> total_bw;

  // Network-specific
  std::optional<std::string> rx_bw;
  std::optional<std::string> tx_bw;

  // RAM-specific
  std::optional<std::string> ram_used;
  std::optional<std::string> ram_total;
  std::optional<std::string> ram_util;
};

/// Represents a complete system snapshot with timestamp.
struct SystemSnapshot {
  std::string timestamp;
  std::vector<DeviceSnapshot> devices;
};

/// Capture a snapshot of all detected devices.
/// This aggregates GPU, CPU, NPU, RAM, Disk, and Network telemetry.
SystemSnapshot captureSystemSnapshot();

/// Convert a snapshot to JSON string.
std::string snapshotToJson(const SystemSnapshot& snapshot);

/// Run snapshot loop, clearing screen and outputting JSON at the given interval.
/// Returns 0 on clean exit (Ctrl+C), non-zero on error.
int runSnapshotLoop(int intervalMs);

/// Clear the terminal screen (platform-specific).
void clearScreen();

}  // namespace aiz
