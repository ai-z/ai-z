#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aiz {

// NPU vendor enumeration
enum class NpuVendor {
  Unknown,
  Intel,  // Intel NPU (formerly Movidius VPU / Intel AI Boost)
  AMD,    // AMD XDNA / Ryzen AI
};

// NPU device information
struct NpuDeviceInfo {
  NpuVendor vendor = NpuVendor::Unknown;
  std::string name;
  std::string driverVersion;
  std::uint32_t deviceId = 0;
  std::uint32_t vendorId = 0;
  
  // Performance hints (TOPS = Tera Operations Per Second)
  std::optional<double> peakTops;
  
  // Device path (Linux: /dev/accel/accel*, Windows: device instance path)
  std::string devicePath;
  
  // Additional info lines for detailed display
  std::vector<std::string> detailLines;
};

// NPU availability status
enum class NpuStatus {
  Available,       // NPU detected and driver loaded
  NoDevice,        // No NPU hardware found
  NoDriver,        // NPU hardware may exist but driver not loaded/available
  Unsupported,     // NPU detected but not supported by this build
};

struct NpuAvailability {
  NpuStatus status = NpuStatus::NoDevice;
  std::vector<NpuDeviceInfo> devices;
  std::string diagnostics;
};

// Probe for all NPUs on the system
NpuAvailability probeNpuDevices();

// Platform-specific probes (implemented in separate files)
namespace detail {
  NpuAvailability probeIntelNpu();
  NpuAvailability probeAmdNpu();
}

// Get a human-readable string for the NPU vendor
std::string npuVendorToString(NpuVendor vendor);

// Get a human-readable string for the NPU status
std::string npuStatusToString(NpuStatus status);

// ONNX Runtime NPU execution provider availability
struct OrtNpuProvider {
  bool intelNpuAvailable = false;   // Intel OpenVINO EP or NPU EP
  bool amdNpuAvailable = false;     // AMD Vitis AI EP or NPU EP
  std::string intelProviderName;    // e.g., "OpenVINOExecutionProvider" or "NPUExecutionProvider"
  std::string amdProviderName;      // e.g., "VitisAIExecutionProvider" or "AMDNPUExecutionProvider"
};

// Check ONNX Runtime NPU execution provider availability
OrtNpuProvider probeOrtNpuProviders();

}  // namespace aiz
