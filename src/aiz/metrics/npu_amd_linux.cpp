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
  while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
    line.pop_back();
  }
  return line;
}

// Parse hex value from sysfs
static std::uint32_t parseHex(const std::string& s) {
  if (s.empty()) return 0;
  try {
    return static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
  } catch (...) {
    return 0;
  }
}

// AMD NPU detection via DRM accel subsystem
// AMD XDNA/Ryzen AI NPU driver exposes devices under /sys/class/accel/
// AMD vendor ID: 0x1002 or 0x1022
static std::vector<NpuDeviceInfo> probeAmdNpuLinux() {
  std::vector<NpuDeviceInfo> devices;
  
  const char* accelPath = "/sys/class/accel";
  DIR* dir = opendir(accelPath);
  if (!dir) {
    return devices;
  }
  
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name = entry->d_name;
    
    // Look for accel* devices
    if (name.find("accel") != 0) continue;
    
    std::string deviceDir = std::string(accelPath) + "/" + name + "/device";
    
    // Check if this is an AMD device
    std::string vendorStr = readSysfsAttr(deviceDir + "/vendor");
    std::uint32_t vendorId = parseHex(vendorStr);
    
    // AMD vendor IDs: 0x1002 (main) or 0x1022 (alternate)
    if (vendorId != 0x1002 && vendorId != 0x1022) continue;
    
    // Check the driver name for amdxdna
    std::string driverLink = deviceDir + "/driver";
    char driverPath[PATH_MAX];
    ssize_t len = readlink(driverLink.c_str(), driverPath, sizeof(driverPath) - 1);
    if (len <= 0) continue;
    
    driverPath[len] = '\0';
    std::string driverName = std::filesystem::path(driverPath).filename().string();
    
    bool isXdna = (driverName.find("amdxdna") != std::string::npos ||
                   driverName.find("xdna") != std::string::npos);
    
    // Also check device class for Processing Accelerator
    std::string classStr = readSysfsAttr(deviceDir + "/class");
    std::uint32_t deviceClass = parseHex(classStr);
    bool isNpuClass = ((deviceClass >> 8) == 0x0b40);
    
    if (!isXdna && !isNpuClass) continue;
    
    NpuDeviceInfo info;
    info.vendor = NpuVendor::AMD;
    info.vendorId = vendorId;
    
    std::string deviceIdStr = readSysfsAttr(deviceDir + "/device");
    info.deviceId = parseHex(deviceIdStr);
    
    // Map known AMD XDNA/Ryzen AI NPU device IDs
    // These are AMD Ryzen AI processors with XDNA architecture
    switch (info.deviceId) {
      // Phoenix/Hawk Point (Ryzen 7040/8040 series)
      case 0x1502:
        info.name = "AMD Ryzen AI (Phoenix/Hawk Point NPU)";
        info.peakTops = 10.0;  // ~10 INT8 TOPS
        break;
      // Strix Point (Ryzen AI 300 series)
      case 0x17F0:
        info.name = "AMD Ryzen AI (Strix Point NPU)";
        info.peakTops = 50.0;  // ~50 INT8 TOPS (XDNA 2)
        break;
      case 0x17F1:
        info.name = "AMD Ryzen AI (Strix Point NPU)";
        info.peakTops = 50.0;
        break;
      // Strix Halo
      case 0x17E0:
        info.name = "AMD Ryzen AI (Strix Halo NPU)";
        info.peakTops = 50.0;
        break;
      // Kraken Point
      case 0x17F8:
        info.name = "AMD Ryzen AI (Kraken Point NPU)";
        info.peakTops = 55.0;  // Estimated XDNA 2+
        break;
      default:
        info.name = "AMD Ryzen AI NPU";
        break;
    }
    
    // Device path
    info.devicePath = "/dev/" + name;
    
    // Read driver version from module info
    std::string modVersionPath = "/sys/module/" + driverName + "/version";
    info.driverVersion = readSysfsAttr(modVersionPath);
    
    if (info.driverVersion.empty()) {
      // Try from DKMS or kernel version
      std::ifstream proc_version("/proc/version");
      if (proc_version) {
        std::string kernelInfo;
        std::getline(proc_version, kernelInfo);
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
    
    info.detailLines.push_back(" Architecture: AMD XDNA");
    
    devices.push_back(std::move(info));
  }
  
  closedir(dir);
  return devices;
}

}  // namespace

NpuAvailability probeAmdNpu() {
  NpuAvailability result;
  
  auto devices = probeAmdNpuLinux();
  
  if (devices.empty()) {
    // Check if XDNA hardware might exist but driver not loaded
    std::string lspciPath = "/sys/bus/pci/devices";
    DIR* dir = opendir(lspciPath.c_str());
    if (dir) {
      struct dirent* entry;
      bool foundNpuHardware = false;
      std::uint32_t foundDeviceId = 0;
      
      // Known AMD XDNA device IDs
      const std::vector<std::uint32_t> knownXdnaIds = {
        0x1502, 0x17F0, 0x17F1, 0x17E0, 0x17F8
      };
      
      while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        
        std::string deviceDir = lspciPath + "/" + entry->d_name;
        std::string vendorStr = readSysfsAttr(deviceDir + "/vendor");
        std::string deviceIdStr = readSysfsAttr(deviceDir + "/device");
        std::string classStr = readSysfsAttr(deviceDir + "/class");
        
        std::uint32_t vendorId = parseHex(vendorStr);
        std::uint32_t deviceId = parseHex(deviceIdStr);
        std::uint32_t deviceClass = parseHex(classStr);
        
        // AMD vendor, check for known XDNA device IDs or Processing Accelerator class
        if ((vendorId == 0x1002 || vendorId == 0x1022)) {
          if (std::find(knownXdnaIds.begin(), knownXdnaIds.end(), deviceId) != knownXdnaIds.end() ||
              (deviceClass >> 8) == 0x0b40) {
            foundNpuHardware = true;
            foundDeviceId = deviceId;
            break;
          }
        }
      }
      closedir(dir);
      
      if (foundNpuHardware) {
        result.status = NpuStatus::NoDriver;
        result.diagnostics = "AMD Ryzen AI NPU hardware detected but driver not loaded. "
                            "Install amdxdna driver (https://github.com/amd/xdna-driver) and ensure it's loaded.";
        return result;
      }
    }
    
    result.status = NpuStatus::NoDevice;
    result.diagnostics = "No AMD NPU detected.";
    return result;
  }
  
  result.status = NpuStatus::Available;
  result.devices = std::move(devices);
  result.diagnostics = "AMD Ryzen AI NPU available.";
  return result;
}

}  // namespace detail
}  // namespace aiz

#endif  // __linux__
