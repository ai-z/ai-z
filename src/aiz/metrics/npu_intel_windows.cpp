#include <aiz/metrics/npu_info.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace aiz {
namespace detail {
namespace {

static std::string wideToUtf8(const wchar_t* s) {
  if (!s) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string out(len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), len, nullptr, nullptr);
  return out;
}

static std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (len <= 1) return {};
  std::wstring out(len - 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
  return out;
}

// Intel NPU GUID: {b09b7b9c-c8a4-4c4e-b0e7-8f76d1e5a7d8}
// This is the device interface class GUID for Intel NPU devices
static const GUID GUID_DEVINTERFACE_INTEL_NPU = 
    { 0xb09b7b9c, 0xc8a4, 0x4c4e, { 0xb0, 0xe7, 0x8f, 0x76, 0xd1, 0xe5, 0xa7, 0xd8 } };

// Processing Accelerator class GUID
static const GUID GUID_DEVCLASS_PROCESSOR_ACCELERATOR = 
    { 0x50127dc3, 0x0f36, 0x415e, { 0xa6, 0xcc, 0x4c, 0xb3, 0xbe, 0x91, 0x0b, 0x65 } };

// Compute Accelerator class (Windows 11+)
static const GUID GUID_DEVCLASS_COMPUTE_ACCELERATOR =
    { 0xf01a9d53, 0x3ff6, 0x48d2, { 0x9f, 0x97, 0xc8, 0xa7, 0x00, 0x4b, 0xe1, 0x0c } };

struct DeviceEnumResult {
  std::string devicePath;
  std::string description;
  std::string driver;
  std::string hardwareId;
  std::uint32_t vendorId = 0;
  std::uint32_t deviceId = 0;
};

static std::vector<DeviceEnumResult> enumerateDevicesByClass(const GUID& classGuid) {
  std::vector<DeviceEnumResult> results;
  
  HDEVINFO devInfo = SetupDiGetClassDevsW(&classGuid, nullptr, nullptr, 
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (devInfo == INVALID_HANDLE_VALUE) {
    // Try without DIGCF_DEVICEINTERFACE
    devInfo = SetupDiGetClassDevsW(&classGuid, nullptr, nullptr, DIGCF_PRESENT);
    if (devInfo == INVALID_HANDLE_VALUE) {
      return results;
    }
  }
  
  SP_DEVINFO_DATA devInfoData{};
  devInfoData.cbSize = sizeof(devInfoData);
  
  for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devInfoData); ++i) {
    DeviceEnumResult dev;
    
    wchar_t buffer[512] = {};
    DWORD bufferSize = sizeof(buffer);
    
    // Get device description
    if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData, SPDRP_DEVICEDESC,
                                          nullptr, reinterpret_cast<PBYTE>(buffer),
                                          bufferSize, nullptr)) {
      dev.description = wideToUtf8(buffer);
    }
    
    // Get driver info
    bufferSize = sizeof(buffer);
    if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData, SPDRP_DRIVER,
                                          nullptr, reinterpret_cast<PBYTE>(buffer),
                                          bufferSize, nullptr)) {
      dev.driver = wideToUtf8(buffer);
    }
    
    // Get hardware IDs (first one typically contains VEN_XXXX&DEV_XXXX)
    bufferSize = sizeof(buffer);
    if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData, SPDRP_HARDWAREID,
                                          nullptr, reinterpret_cast<PBYTE>(buffer),
                                          bufferSize, nullptr)) {
      dev.hardwareId = wideToUtf8(buffer);
      
      // Parse VEN_XXXX and DEV_XXXX from hardware ID
      std::string hwid = dev.hardwareId;
      std::transform(hwid.begin(), hwid.end(), hwid.begin(), ::toupper);
      
      size_t venPos = hwid.find("VEN_");
      if (venPos != std::string::npos && venPos + 8 <= hwid.size()) {
        try {
          dev.vendorId = static_cast<std::uint32_t>(
              std::stoul(hwid.substr(venPos + 4, 4), nullptr, 16));
        } catch (...) {}
      }
      
      size_t devPos = hwid.find("DEV_");
      if (devPos != std::string::npos && devPos + 8 <= hwid.size()) {
        try {
          dev.deviceId = static_cast<std::uint32_t>(
              std::stoul(hwid.substr(devPos + 4, 4), nullptr, 16));
        } catch (...) {}
      }
    }
    
    // Get device instance path
    wchar_t instanceId[MAX_DEVICE_ID_LEN] = {};
    if (CM_Get_Device_IDW(devInfoData.DevInst, instanceId, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
      dev.devicePath = wideToUtf8(instanceId);
    }
    
    results.push_back(std::move(dev));
  }
  
  SetupDiDestroyDeviceInfoList(devInfo);
  return results;
}

static std::vector<DeviceEnumResult> findIntelNpuByHardwareId() {
  std::vector<DeviceEnumResult> results;
  
  // Search for devices with Intel NPU hardware IDs across all classes
  HDEVINFO devInfo = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, 
                                           DIGCF_PRESENT | DIGCF_ALLCLASSES);
  if (devInfo == INVALID_HANDLE_VALUE) {
    return results;
  }
  
  SP_DEVINFO_DATA devInfoData{};
  devInfoData.cbSize = sizeof(devInfoData);
  
  // Known Intel NPU device IDs
  const std::vector<std::uint32_t> knownNpuDeviceIds = {
    0x7D1D,  // Meteor Lake NPU
    0xAD1D,  // Arrow Lake NPU  
    0xB01D,  // Lunar Lake NPU
    0x643E,  // Panther Lake NPU (tentative)
  };
  
  for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devInfoData); ++i) {
    wchar_t buffer[512] = {};
    DWORD bufferSize = sizeof(buffer);
    
    // Get hardware ID first
    if (!SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData, SPDRP_HARDWAREID,
                                           nullptr, reinterpret_cast<PBYTE>(buffer),
                                           bufferSize, nullptr)) {
      continue;
    }
    
    std::string hwid = wideToUtf8(buffer);
    std::string hwidUpper = hwid;
    std::transform(hwidUpper.begin(), hwidUpper.end(), hwidUpper.begin(), ::toupper);
    
    // Check for Intel vendor
    if (hwidUpper.find("VEN_8086") == std::string::npos) continue;
    
    // Parse device ID
    std::uint32_t deviceId = 0;
    size_t devPos = hwidUpper.find("DEV_");
    if (devPos != std::string::npos && devPos + 8 <= hwidUpper.size()) {
      try {
        deviceId = static_cast<std::uint32_t>(
            std::stoul(hwidUpper.substr(devPos + 4, 4), nullptr, 16));
      } catch (...) {
        continue;
      }
    }
    
    // Check if this is a known NPU device ID
    bool isKnownNpu = std::find(knownNpuDeviceIds.begin(), knownNpuDeviceIds.end(), 
                                 deviceId) != knownNpuDeviceIds.end();
    
    // Also check device description for NPU keywords
    bufferSize = sizeof(buffer);
    std::string description;
    if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData, SPDRP_DEVICEDESC,
                                          nullptr, reinterpret_cast<PBYTE>(buffer),
                                          bufferSize, nullptr)) {
      description = wideToUtf8(buffer);
      std::string descUpper = description;
      std::transform(descUpper.begin(), descUpper.end(), descUpper.begin(), ::toupper);
      
      if (descUpper.find("NPU") != std::string::npos ||
          descUpper.find("NEURAL") != std::string::npos ||
          descUpper.find("AI BOOST") != std::string::npos ||
          descUpper.find("VPU") != std::string::npos) {
        isKnownNpu = true;
      }
    }
    
    if (!isKnownNpu) continue;
    
    DeviceEnumResult dev;
    dev.hardwareId = hwid;
    dev.description = description;
    dev.vendorId = 0x8086;
    dev.deviceId = deviceId;
    
    // Get driver info
    bufferSize = sizeof(buffer);
    if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData, SPDRP_DRIVER,
                                          nullptr, reinterpret_cast<PBYTE>(buffer),
                                          bufferSize, nullptr)) {
      dev.driver = wideToUtf8(buffer);
    }
    
    // Get device instance path
    wchar_t instanceId[MAX_DEVICE_ID_LEN] = {};
    if (CM_Get_Device_IDW(devInfoData.DevInst, instanceId, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
      dev.devicePath = wideToUtf8(instanceId);
    }
    
    results.push_back(std::move(dev));
  }
  
  SetupDiDestroyDeviceInfoList(devInfo);
  return results;
}

static std::string getDriverVersion(const std::string& driverKey) {
  if (driverKey.empty()) return {};
  
  std::wstring keyPath = L"SYSTEM\\CurrentControlSet\\Control\\Class\\" + utf8ToWide(driverKey);
  
  HKEY hKey;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
    return {};
  }
  
  wchar_t version[256] = {};
  DWORD size = sizeof(version);
  if (RegQueryValueExW(hKey, L"DriverVersion", nullptr, nullptr, 
                       reinterpret_cast<LPBYTE>(version), &size) == ERROR_SUCCESS) {
    RegCloseKey(hKey);
    return wideToUtf8(version);
  }
  
  RegCloseKey(hKey);
  return {};
}

}  // namespace

NpuAvailability probeIntelNpu() {
  NpuAvailability result;
  
  std::vector<DeviceEnumResult> npuDevices;
  
  // Try different methods to find Intel NPU
  // 1. Check Processing Accelerator class
  auto accelDevices = enumerateDevicesByClass(GUID_DEVCLASS_PROCESSOR_ACCELERATOR);
  for (auto& dev : accelDevices) {
    if (dev.vendorId == 0x8086) {
      npuDevices.push_back(std::move(dev));
    }
  }
  
  // 2. Check Compute Accelerator class (Windows 11+)
  auto computeDevices = enumerateDevicesByClass(GUID_DEVCLASS_COMPUTE_ACCELERATOR);
  for (auto& dev : computeDevices) {
    if (dev.vendorId == 0x8086) {
      npuDevices.push_back(std::move(dev));
    }
  }
  
  // 3. Search by known hardware IDs
  if (npuDevices.empty()) {
    npuDevices = findIntelNpuByHardwareId();
  }
  
  if (npuDevices.empty()) {
    result.status = NpuStatus::NoDevice;
    result.diagnostics = "No Intel NPU detected.";
    return result;
  }
  
  // Convert to NpuDeviceInfo
  for (const auto& dev : npuDevices) {
    NpuDeviceInfo info;
    info.vendor = NpuVendor::Intel;
    info.vendorId = dev.vendorId;
    info.deviceId = dev.deviceId;
    info.devicePath = dev.devicePath;
    
    // Get driver version
    info.driverVersion = getDriverVersion(dev.driver);
    
    // Map device IDs to names and performance specs
    if (!dev.description.empty()) {
      info.name = dev.description;
    } else {
      switch (dev.deviceId) {
        case 0x7D1D:
          info.name = "Intel AI Boost (Meteor Lake NPU)";
          info.peakTops = 10.0;
          break;
        case 0xAD1D:
          info.name = "Intel AI Boost (Arrow Lake NPU)";
          info.peakTops = 13.0;
          break;
        case 0xB01D:
          info.name = "Intel AI Boost (Lunar Lake NPU)";
          info.peakTops = 48.0;
          break;
        case 0x643E:
          info.name = "Intel AI Boost (Panther Lake NPU)";
          info.peakTops = 60.0;
          break;
        default:
          info.name = "Intel Neural Processing Unit";
          break;
      }
    }
    
    // Set peak TOPS based on device ID if not set via description
    if (!info.peakTops) {
      switch (dev.deviceId) {
        case 0x7D1D: info.peakTops = 10.0; break;
        case 0xAD1D: info.peakTops = 13.0; break;
        case 0xB01D: info.peakTops = 48.0; break;
        case 0x643E: info.peakTops = 60.0; break;
      }
    }
    
    // Add detail lines
    {
      std::ostringstream ss;
      ss << " Device ID: 0x" << std::hex << std::uppercase << info.deviceId;
      info.detailLines.push_back(ss.str());
    }
    
    if (info.peakTops) {
      std::ostringstream ss;
      ss << " Peak Performance: " << std::fixed << std::setprecision(1) 
         << *info.peakTops << " TOPS (INT8)";
      info.detailLines.push_back(ss.str());
    }
    
    if (!info.driverVersion.empty()) {
      info.detailLines.push_back(" Driver Version: " + info.driverVersion);
    }
    
    result.devices.push_back(std::move(info));
  }
  
  result.status = NpuStatus::Available;
  result.diagnostics = "Intel NPU available.";
  return result;
}

}  // namespace detail
}  // namespace aiz

#endif  // _WIN32
