#include <aiz/metrics/npu_info.h>

#if defined(__linux__)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace aiz {
namespace detail {
namespace {

// Read a sysfs attribute file, returning trimmed content or empty string
static std::string readSysfsAttr(const std::string& path) {
  std::ifstream in(path);
  if (!in) return {};
  std::string line;
  std::getline(in, line);
  // Trim whitespace
  while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
    line.pop_back();
  }
  return line;
}

// Parse hex value from sysfs (handles "0x" prefix)
static std::uint32_t parseHex(const std::string& s) {
  if (s.empty()) return 0;
  try {
    return static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
  } catch (...) {
    return 0;
  }
}

// Intel NPU detection via DRM accel subsystem (/dev/accel/accel*)
// Intel NPU driver exposes devices under /sys/class/accel/
// Device class: 0x0b4000 (Processing Accelerator - AI Inference Accelerator)
static std::vector<NpuDeviceInfo> probeIntelNpuLinux() {
  std::vector<NpuDeviceInfo> devices;
  
  const char* accelPath = "/sys/class/accel";
  DIR* dir = opendir(accelPath);
  if (!dir) {
    // Try older path for NPU driver
    accelPath = "/sys/class/drm";
    dir = opendir(accelPath);
    if (!dir) return devices;
  }
  
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name = entry->d_name;
    
    // Look for accel* devices (accel0, accel1, etc.)
    if (name.find("accel") != 0) continue;
    
    std::string deviceDir = std::string(accelPath) + "/" + name + "/device";
    
    // Check if this is an Intel device
    std::string vendorStr = readSysfsAttr(deviceDir + "/vendor");
    std::uint32_t vendorId = parseHex(vendorStr);
    
    // Intel vendor ID: 0x8086
    if (vendorId != 0x8086) continue;
    
    // Check device class for AI accelerator (0x0b40xx - Processing Accelerator)
    std::string classStr = readSysfsAttr(deviceDir + "/class");
    std::uint32_t deviceClass = parseHex(classStr);
    
    // Class 0x0b4000 = Processing Accelerator - AI Inference Accelerator
    // Also check for 0x128000 (Signal Processing Controller - used by older VPU)
    bool isNpu = ((deviceClass >> 8) == 0x0b40) || ((deviceClass >> 8) == 0x1280);
    
    if (!isNpu) {
      // Also check the driver name for intel_vpu or intel_npu
      std::string driverLink = deviceDir + "/driver";
      char driverPath[PATH_MAX];
      ssize_t len = readlink(driverLink.c_str(), driverPath, sizeof(driverPath) - 1);
      if (len > 0) {
        driverPath[len] = '\0';
        std::string driverName = std::filesystem::path(driverPath).filename().string();
        if (driverName.find("intel_vpu") != std::string::npos ||
            driverName.find("intel_npu") != std::string::npos ||
            driverName.find("ivpu") != std::string::npos) {
          isNpu = true;
        }
      }
    }
    
    if (!isNpu) continue;
    
    NpuDeviceInfo info;
    info.vendor = NpuVendor::Intel;
    info.vendorId = vendorId;
    
    std::string deviceIdStr = readSysfsAttr(deviceDir + "/device");
    info.deviceId = parseHex(deviceIdStr);
    
    // Get device name from uevent or construct from device ID
    std::string uevent = readSysfsAttr(deviceDir + "/uevent");
    // Parse PCI device info from uevent (format: PCI_SLOT_NAME=...)
    
    // Map known Intel NPU device IDs to names
    switch (info.deviceId) {
      case 0x7D1D:
        info.name = "Intel AI Boost (Meteor Lake NPU)";
        info.peakTops = 10.0;  // ~10 INT8 TOPS
        break;
      case 0xAD1D:
        info.name = "Intel AI Boost (Arrow Lake NPU)";
        info.peakTops = 13.0;  // ~13 INT8 TOPS
        break;
      case 0xB01D:
        info.name = "Intel AI Boost (Lunar Lake NPU)";
        info.peakTops = 48.0;  // ~48 INT8 TOPS (NPU 4)
        break;
      case 0x643E:
        info.name = "Intel AI Boost (Panther Lake NPU)";
        info.peakTops = 60.0;  // Estimated NPU 4+
        break;
      default:
        info.name = "Intel Neural Processing Unit";
        break;
    }
    
    // Device path
    info.devicePath = "/dev/" + name;
    
    // Read driver version from module info
    std::string driverLink = deviceDir + "/driver";
    char driverPath[PATH_MAX];
    ssize_t len = readlink(driverLink.c_str(), driverPath, sizeof(driverPath) - 1);
    if (len > 0) {
      driverPath[len] = '\0';
      std::string driverName = std::filesystem::path(driverPath).filename().string();
      
      // Try to read module version
      std::string modVersionPath = "/sys/module/" + driverName + "/version";
      info.driverVersion = readSysfsAttr(modVersionPath);
      
      if (info.driverVersion.empty()) {
        // Fallback: try to get kernel version as driver version
        std::ifstream proc_version("/proc/version");
        if (proc_version) {
          std::string kernelInfo;
          std::getline(proc_version, kernelInfo);
          // Extract version number
          size_t start = kernelInfo.find("version ");
          if (start != std::string::npos) {
            start += 8;
            size_t end = kernelInfo.find(' ', start);
            if (end != std::string::npos) {
              info.driverVersion = "kernel " + kernelInfo.substr(start, end - start);
            }
          }
        }
      }
    }
    
    // Add detail lines
    std::ostringstream ss;
    ss << " Device ID: 0x" << std::hex << info.deviceId;
    info.detailLines.push_back(ss.str());
    
    if (info.peakTops) {
      std::ostringstream topsStr;
      topsStr << " Peak Performance: " << *info.peakTops << " TOPS (INT8)";
      info.detailLines.push_back(topsStr.str());
    }
    
    if (!info.driverVersion.empty()) {
      info.detailLines.push_back(" Driver: " + info.driverVersion);
    }
    
    devices.push_back(std::move(info));
  }
  
  closedir(dir);
  return devices;
}

}  // namespace

NpuAvailability probeIntelNpu() {
  NpuAvailability result;
  
  auto devices = probeIntelNpuLinux();
  
  if (devices.empty()) {
    // Check if NPU hardware might exist but driver not loaded
    // Look for Intel NPU device in lspci output (device class 0b40)
    std::string lspciPath = "/sys/bus/pci/devices";
    DIR* dir = opendir(lspciPath.c_str());
    if (dir) {
      struct dirent* entry;
      bool foundNpuHardware = false;
      
      while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        
        std::string deviceDir = lspciPath + "/" + entry->d_name;
        std::string vendorStr = readSysfsAttr(deviceDir + "/vendor");
        std::string classStr = readSysfsAttr(deviceDir + "/class");
        
        std::uint32_t vendorId = parseHex(vendorStr);
        std::uint32_t deviceClass = parseHex(classStr);
        
        // Intel vendor, Processing Accelerator class
        if (vendorId == 0x8086 && ((deviceClass >> 8) == 0x0b40)) {
          foundNpuHardware = true;
          break;
        }
      }
      closedir(dir);
      
      if (foundNpuHardware) {
        result.status = NpuStatus::NoDriver;
        result.diagnostics = "Intel NPU hardware detected but driver not loaded. "
                            "Install intel-npu-driver package and ensure intel_vpu module is loaded.";
        return result;
      }
    }
    
    result.status = NpuStatus::NoDevice;
    result.diagnostics = "No Intel NPU detected.";
    return result;
  }
  
  result.status = NpuStatus::Available;
  result.devices = std::move(devices);
  result.diagnostics = "Intel NPU available.";
  return result;
}

}  // namespace detail
}  // namespace aiz

#endif  // __linux__
