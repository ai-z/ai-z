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

static std::vector<DeviceEnumResult> findAmdNpuByHardwareId() {
  std::vector<DeviceEnumResult> results;
  
  HDEVINFO devInfo = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, 
                                           DIGCF_PRESENT | DIGCF_ALLCLASSES);
  if (devInfo == INVALID_HANDLE_VALUE) {
    return results;
  }
  
  SP_DEVINFO_DATA devInfoData{};
  devInfoData.cbSize = sizeof(devInfoData);
  
  // Known AMD XDNA NPU device IDs
  const std::vector<std::uint32_t> knownNpuDeviceIds = {
    0x1502,  // Phoenix/Hawk Point (Ryzen 7040/8040)
    0x17F0,  // Strix Point 
    0x17F1,  // Strix Point variant
    0x17E0,  // Strix Halo
    0x17F8,  // Kraken Point
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
    
    // Check for AMD vendor (0x1002 or 0x1022)
    bool isAmd = (hwidUpper.find("VEN_1002") != std::string::npos ||
                  hwidUpper.find("VEN_1022") != std::string::npos);
    if (!isAmd) continue;
    
    // Parse vendor and device IDs
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    
    size_t venPos = hwidUpper.find("VEN_");
    if (venPos != std::string::npos && venPos + 8 <= hwidUpper.size()) {
      try {
        vendorId = static_cast<std::uint32_t>(
            std::stoul(hwidUpper.substr(venPos + 4, 4), nullptr, 16));
      } catch (...) {}
    }
    
    size_t devPos = hwidUpper.find("DEV_");
    if (devPos != std::string::npos && devPos + 8 <= hwidUpper.size()) {
      try {
        deviceId = static_cast<std::uint32_t>(
            std::stoul(hwidUpper.substr(devPos + 4, 4), nullptr, 16));
      } catch (...) {
        continue;
      }
    }
    
    // Check if this is a known XDNA NPU device ID
    bool isKnownNpu = std::find(knownNpuDeviceIds.begin(), knownNpuDeviceIds.end(), 
                                 deviceId) != knownNpuDeviceIds.end();
    
    // Also check device description for NPU/XDNA keywords
    bufferSize = sizeof(buffer);
    std::string description;
    if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData, SPDRP_DEVICEDESC,
                                          nullptr, reinterpret_cast<PBYTE>(buffer),
                                          bufferSize, nullptr)) {
      description = wideToUtf8(buffer);
      std::string descUpper = description;
      std::transform(descUpper.begin(), descUpper.end(), descUpper.begin(), ::toupper);
      
      if (descUpper.find("NPU") != std::string::npos ||
          descUpper.find("XDNA") != std::string::npos ||
          descUpper.find("RYZEN AI") != std::string::npos ||
          descUpper.find("NEURAL") != std::string::npos ||
          descUpper.find("AI ACCELERATOR") != std::string::npos) {
        isKnownNpu = true;
      }
    }
    
    if (!isKnownNpu) continue;
    
    DeviceEnumResult dev;
    dev.hardwareId = hwid;
    dev.description = description;
    dev.vendorId = vendorId;
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

NpuAvailability probeAmdNpu() {
  NpuAvailability result;
  
  std::vector<DeviceEnumResult> npuDevices = findAmdNpuByHardwareId();
  
  if (npuDevices.empty()) {
    result.status = NpuStatus::NoDevice;
    result.diagnostics = "No AMD Ryzen AI NPU detected.";
    return result;
  }
  
  // Convert to NpuDeviceInfo
  for (const auto& dev : npuDevices) {
    NpuDeviceInfo info;
    info.vendor = NpuVendor::AMD;
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
        case 0x1502:
          info.name = "AMD Ryzen AI (Phoenix/Hawk Point NPU)";
          info.peakTops = 10.0;
          break;
        case 0x17F0:
        case 0x17F1:
          info.name = "AMD Ryzen AI (Strix Point NPU)";
          info.peakTops = 50.0;
          break;
        case 0x17E0:
          info.name = "AMD Ryzen AI (Strix Halo NPU)";
          info.peakTops = 50.0;
          break;
        case 0x17F8:
          info.name = "AMD Ryzen AI (Kraken Point NPU)";
          info.peakTops = 55.0;
          break;
        default:
          info.name = "AMD Ryzen AI NPU";
          break;
      }
    }
    
    // Set peak TOPS based on device ID if not already set
    if (!info.peakTops) {
      switch (dev.deviceId) {
        case 0x1502: info.peakTops = 10.0; break;
        case 0x17F0:
        case 0x17F1: info.peakTops = 50.0; break;
        case 0x17E0: info.peakTops = 50.0; break;
        case 0x17F8: info.peakTops = 55.0; break;
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
    
    info.detailLines.push_back(" Architecture: AMD XDNA");
    
    result.devices.push_back(std::move(info));
  }
  
  result.status = NpuStatus::Available;
  result.diagnostics = "AMD Ryzen AI NPU available.";
  return result;
}

}  // namespace detail
}  // namespace aiz

#endif  // _WIN32
