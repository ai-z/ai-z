#include <aiz/metrics/windows_d3dkmt.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
// initguid.h must be included BEFORE devpkey.h to define (not just declare) the GUIDs
#include <initguid.h>
#include <devpkey.h>
#include <setupapi.h>
#include <devguid.h>

#include <cstdint>
#include <mutex>
#include <sstream>

// PCIe device property keys (these are in devpkey.h but we need initguid.h before it)
// GUID: {3AB22E31-8264-4B4E-9AF5-A8D2D8E33E62}
// PID 9 = CurrentLinkSpeed, 10 = CurrentLinkWidth, 11 = MaxLinkSpeed, 12 = MaxLinkWidth
static const DEVPROPKEY AI_Z_DEVPKEY_PciDevice_CurrentLinkSpeed = {
    {0x3ab22e31, 0x8264, 0x4b4e, {0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62}}, 9};
static const DEVPROPKEY AI_Z_DEVPKEY_PciDevice_CurrentLinkWidth = {
    {0x3ab22e31, 0x8264, 0x4b4e, {0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62}}, 10};
static const DEVPROPKEY AI_Z_DEVPKEY_PciDevice_MaxLinkSpeed = {
    {0x3ab22e31, 0x8264, 0x4b4e, {0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62}}, 11};
static const DEVPROPKEY AI_Z_DEVPKEY_PciDevice_MaxLinkWidth = {
    {0x3ab22e31, 0x8264, 0x4b4e, {0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62}}, 12};

namespace aiz {

namespace {

using NtStatus = long;
static constexpr NtStatus kStatusSuccess = 0;

using D3DKMT_HANDLE = std::uint32_t;

struct D3DKMT_OPENADAPTERFROMLUID {
  LUID AdapterLuid;
  D3DKMT_HANDLE hAdapter;
};

struct D3DKMT_CLOSEADAPTER {
  D3DKMT_HANDLE hAdapter;
};

enum D3DKMT_MEMORY_SEGMENT_GROUP : std::uint32_t {
  D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL = 0,
  D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL = 1,
};

struct D3DKMT_QUERYVIDEOMEMORYINFO {
  D3DKMT_HANDLE hAdapter;
  std::uint32_t NodeOrdinal;
  D3DKMT_MEMORY_SEGMENT_GROUP MemorySegmentGroup;
  std::uint64_t Budget;
  std::uint64_t CurrentUsage;
  std::uint64_t AvailableForReservation;
  std::uint64_t CurrentReservation;
};

// For D3DKMTQueryAdapterInfo
struct D3DKMT_QUERYADAPTERINFO {
  D3DKMT_HANDLE hAdapter;
  std::uint32_t Type;  // KMTQUERYADAPTERINFOTYPE
  void* pPrivateDriverData;
  std::uint32_t PrivateDriverDataSize;
};

// Performance data from KMTQAITYPE_ADAPTERPERFDATA
// Note: D3DKMT_ALIGN64 forces 8-byte alignment on 64-bit fields
#pragma pack(push, 8)
struct D3DKMT_ADAPTER_PERFDATA {
  std::uint32_t PhysicalAdapterIndex;
  std::uint32_t _padding;            // Explicit padding for 8-byte alignment
  std::uint64_t MemoryFrequency;
  std::uint64_t MaxMemoryFrequency;
  std::uint64_t MaxMemoryFrequencyOC;
  std::uint64_t MemoryBandwidth;
  std::uint64_t PCIEBandwidth;
  std::uint32_t FanRPM;
  std::uint32_t Power;        // Tenths of percent of TDP (1 = 0.1%)
  std::uint32_t Temperature;  // Tenths of Celsius (1 = 0.1°C)
  std::uint8_t PowerStateOverride;
};
#pragma pack(pop)

// KMTQAITYPE_ADAPTERPERFDATA = 62 in Windows SDK
static constexpr std::uint32_t KMTQAITYPE_ADAPTERPERFDATA = 62;

using PFN_D3DKMTOpenAdapterFromLuid = NtStatus(WINAPI*)(D3DKMT_OPENADAPTERFROMLUID*);
using PFN_D3DKMTCloseAdapter = NtStatus(WINAPI*)(D3DKMT_CLOSEADAPTER*);
using PFN_D3DKMTQueryVideoMemoryInfo = NtStatus(WINAPI*)(D3DKMT_QUERYVIDEOMEMORYINFO*);
using PFN_D3DKMTQueryAdapterInfo = NtStatus(WINAPI*)(D3DKMT_QUERYADAPTERINFO*);

struct D3dkmtApi {
  HMODULE gdi32 = nullptr;
  PFN_D3DKMTOpenAdapterFromLuid openAdapterFromLuid = nullptr;
  PFN_D3DKMTCloseAdapter closeAdapter = nullptr;
  PFN_D3DKMTQueryVideoMemoryInfo queryVideoMemoryInfo = nullptr;
  PFN_D3DKMTQueryAdapterInfo queryAdapterInfo = nullptr;
  bool ready = false;
};

static const D3dkmtApi& d3dkmtApi() {
  static D3dkmtApi api;
  static std::once_flag once;
  std::call_once(once, [&]() {
    api.gdi32 = LoadLibraryW(L"gdi32.dll");
    if (!api.gdi32) return;

    api.openAdapterFromLuid = reinterpret_cast<PFN_D3DKMTOpenAdapterFromLuid>(
        GetProcAddress(api.gdi32, "D3DKMTOpenAdapterFromLuid"));
    api.closeAdapter = reinterpret_cast<PFN_D3DKMTCloseAdapter>(
        GetProcAddress(api.gdi32, "D3DKMTCloseAdapter"));
    api.queryVideoMemoryInfo = reinterpret_cast<PFN_D3DKMTQueryVideoMemoryInfo>(
        GetProcAddress(api.gdi32, "D3DKMTQueryVideoMemoryInfo"));
    api.queryAdapterInfo = reinterpret_cast<PFN_D3DKMTQueryAdapterInfo>(
        GetProcAddress(api.gdi32, "D3DKMTQueryAdapterInfo"));

    api.ready = api.openAdapterFromLuid && api.closeAdapter && api.queryVideoMemoryInfo;
  });
  return api;
}

}  // namespace

std::optional<D3dkmtVideoMemoryInfo> queryD3dkmtLocalVideoMemoryForLuid(const LUID& luid) {
  const auto& api = d3dkmtApi();
  if (!api.ready) return std::nullopt;

  D3DKMT_OPENADAPTERFROMLUID open{};
  open.AdapterLuid = luid;
  if (api.openAdapterFromLuid(&open) != kStatusSuccess) return std::nullopt;

  auto closeAdapter = [&]() {
    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = open.hAdapter;
    (void)api.closeAdapter(&close);
  };

  D3DKMT_QUERYVIDEOMEMORYINFO q{};
  q.hAdapter = open.hAdapter;
  q.NodeOrdinal = 0;
  q.MemorySegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;

  if (api.queryVideoMemoryInfo(&q) != kStatusSuccess) {
    closeAdapter();
    return std::nullopt;
  }

  D3dkmtVideoMemoryInfo out;
  out.budgetBytes = static_cast<std::uint64_t>(q.Budget);
  out.currentUsageBytes = static_cast<std::uint64_t>(q.CurrentUsage);
  out.availableForReservationBytes = static_cast<std::uint64_t>(q.AvailableForReservation);
  out.currentReservationBytes = static_cast<std::uint64_t>(q.CurrentReservation);

  closeAdapter();
  return out;
}

std::optional<D3dkmtAdapterPerfData> queryD3dkmtAdapterPerfData(const LUID& luid) {
  const auto& api = d3dkmtApi();
  if (!api.ready || !api.queryAdapterInfo) return std::nullopt;

  D3DKMT_OPENADAPTERFROMLUID open{};
  open.AdapterLuid = luid;
  if (api.openAdapterFromLuid(&open) != kStatusSuccess) return std::nullopt;

  auto closeAdapter = [&]() {
    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = open.hAdapter;
    (void)api.closeAdapter(&close);
  };

  D3DKMT_ADAPTER_PERFDATA perfData{};
  perfData.PhysicalAdapterIndex = 0;

  D3DKMT_QUERYADAPTERINFO q{};
  q.hAdapter = open.hAdapter;
  q.Type = KMTQAITYPE_ADAPTERPERFDATA;
  q.pPrivateDriverData = &perfData;
  q.PrivateDriverDataSize = sizeof(perfData);

  if (api.queryAdapterInfo(&q) != kStatusSuccess) {
    closeAdapter();
    return std::nullopt;
  }

  D3dkmtAdapterPerfData out;
  // Temperature in tenths of Celsius (e.g., 370 = 37.0°C)
  out.temperatureC = static_cast<double>(perfData.Temperature) / 10.0;
  // Power field: MS docs say "tenths of percent of TDP", but Intel drivers report milliwatts.
  // Heuristic: if value > 1000, assume milliwatts -> convert to watts.
  // (1000 mW = 1W, and 1000 tenths of percent = 100% TDP which is implausible idle power)
  if (perfData.Power > 1000) {
    out.powerWatts = static_cast<double>(perfData.Power) / 1000.0;  // milliwatts -> watts
  } else if (perfData.Power > 0) {
    // Likely tenths of percent; we can't convert to watts without knowing TDP.
    out.powerWatts = std::nullopt;  // Can't determine watts
  }
  out.fanRpm = perfData.FanRPM;
  out.memoryFrequencyHz = perfData.MemoryFrequency;
  out.maxMemoryFrequencyHz = perfData.MaxMemoryFrequency;
  out.memoryBandwidthBytes = perfData.MemoryBandwidth;
  out.pcieBandwidthBytes = perfData.PCIEBandwidth;
  out.powerStateOverride = perfData.PowerStateOverride;

  closeAdapter();
  return out;
}

std::optional<WinPcieLinkInfo> queryWinPcieLinkInfo(const LUID& luid) {
  // First get the PCI device path from DXGI adapter
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    return std::nullopt;
  }

  std::wstring deviceInstanceId;
  for (UINT i = 0; ; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) {
        // Found matching adapter. Use vendor/device ID to find PCI device.
        adapter->Release();
        factory->Release();

        // Enumerate display adapters via SetupAPI
        HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr,
                                                 DIGCF_PRESENT);
        if (devInfo == INVALID_HANDLE_VALUE) return std::nullopt;

        SP_DEVINFO_DATA devInfoData{};
        devInfoData.cbSize = sizeof(devInfoData);

        for (DWORD j = 0; SetupDiEnumDeviceInfo(devInfo, j, &devInfoData); ++j) {
          // Check if vendor/device IDs match
          DWORD hardwareIds[2] = {0};
          DEVPROPTYPE propType = 0;

          // Get VendorID and DeviceID
          DWORD vendorId = 0, deviceId = 0;
          if (SetupDiGetDevicePropertyW(devInfo, &devInfoData, &DEVPKEY_Device_Address,
                                        &propType, reinterpret_cast<PBYTE>(&hardwareIds),
                                        sizeof(hardwareIds), nullptr, 0)) {
            // DEVPKEY_Device_Address contains bus/device/function, not vendor/device.
            // Try matching via DEVPKEY_Device_HardwareIds instead.
          }

          // Get Hardware ID string to match vendor/device
          wchar_t hwIdBuf[512] = {0};
          if (SetupDiGetDevicePropertyW(devInfo, &devInfoData, &DEVPKEY_Device_HardwareIds,
                                        &propType, reinterpret_cast<PBYTE>(hwIdBuf),
                                        sizeof(hwIdBuf), nullptr, 0)) {
            // Parse VEN_XXXX&DEV_XXXX from hwIdBuf
            std::wstring hwId(hwIdBuf);
            wchar_t searchVen[32], searchDev[32];
            swprintf_s(searchVen, L"VEN_%04X", desc.VendorId);
            swprintf_s(searchDev, L"DEV_%04X", desc.DeviceId);
            if (hwId.find(searchVen) != std::wstring::npos &&
                hwId.find(searchDev) != std::wstring::npos) {
              // Found matching device. Query PCIe link properties.
              WinPcieLinkInfo linkInfo{};
              DWORD val = 0;
              DWORD size = sizeof(val);

              // AI_Z_DEVPKEY_PciDevice_CurrentLinkSpeed: 1=Gen1, 2=Gen2, 3=Gen3, 4=Gen4, 5=Gen5
              if (SetupDiGetDevicePropertyW(devInfo, &devInfoData,
                                            &AI_Z_DEVPKEY_PciDevice_CurrentLinkSpeed, &propType,
                                            reinterpret_cast<PBYTE>(&val), size, nullptr, 0)) {
                linkInfo.currentLinkSpeed = static_cast<int>(val);
              }

              if (SetupDiGetDevicePropertyW(devInfo, &devInfoData,
                                            &AI_Z_DEVPKEY_PciDevice_CurrentLinkWidth, &propType,
                                            reinterpret_cast<PBYTE>(&val), size, nullptr, 0)) {
                linkInfo.currentLinkWidth = static_cast<int>(val);
              }

              if (SetupDiGetDevicePropertyW(devInfo, &devInfoData,
                                            &AI_Z_DEVPKEY_PciDevice_MaxLinkSpeed, &propType,
                                            reinterpret_cast<PBYTE>(&val), size, nullptr, 0)) {
                linkInfo.maxLinkSpeed = static_cast<int>(val);
              }

              if (SetupDiGetDevicePropertyW(devInfo, &devInfoData,
                                            &AI_Z_DEVPKEY_PciDevice_MaxLinkWidth, &propType,
                                            reinterpret_cast<PBYTE>(&val), size, nullptr, 0)) {
                linkInfo.maxLinkWidth = static_cast<int>(val);
              }

              SetupDiDestroyDeviceInfoList(devInfo);

              // Check if we got valid data (values >0)
              if (linkInfo.currentLinkSpeed > 0 || linkInfo.maxLinkSpeed > 0) {
                return linkInfo;
              }
              return std::nullopt;
            }
          }
        }

        SetupDiDestroyDeviceInfoList(devInfo);
        return std::nullopt;
      }
    }
    adapter->Release();
  }

  factory->Release();
  return std::nullopt;
}

namespace {

static std::string wideToUtf8(const wchar_t* s) {
  if (!s) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string out(len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), len, nullptr, nullptr);
  return out;
}

static bool isIgnoredAdapter(const DXGI_ADAPTER_DESC1& desc, const std::string& name) {
  const bool isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
  const bool isBasicRenderDriver = (desc.VendorId == 0x1414) || (name == "Microsoft Basic Render Driver");
  return isSoftware || isBasicRenderDriver;
}

static std::string formatGiB(std::uint64_t bytes) {
  if (!bytes) return std::string("--");
  const double totalGiB = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(2);
  oss << totalGiB << " GiB";
  return oss.str();
}

}  // namespace

std::string d3dkmtDiagnostics() {
  std::ostringstream oss;
  oss << "D3DKMT diagnostics (Windows)\n";

  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    oss << "- factory: failed\n";
    return oss.str();
  }

  unsigned int idx = 0;
  for (UINT i = 0; ; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      const std::string name = wideToUtf8(desc.Description);
      if (!isIgnoredAdapter(desc, name)) {
        oss << "  GPU" << idx++ << ": " << (name.empty() ? std::string("(unknown)") : name) << "\n";
        if (const auto d3 = queryD3dkmtLocalVideoMemoryForLuid(desc.AdapterLuid)) {
          oss << "    budget: " << formatGiB(d3->budgetBytes)
              << " used: " << formatGiB(d3->currentUsageBytes) << "\n";
          oss << "    available: " << formatGiB(d3->availableForReservationBytes)
              << " reserved: " << formatGiB(d3->currentReservationBytes) << "\n";
        } else {
          oss << "    d3dkmt memory: unavailable\n";
        }
        if (const auto perf = queryD3dkmtAdapterPerfData(desc.AdapterLuid)) {
          oss << "    temp: " << perf->temperatureC << " C";
          if (perf->powerWatts) {
            oss << " power: " << *perf->powerWatts << " W";
          }
          oss << " fan: " << perf->fanRpm << " RPM\n";
          oss << "    mem_freq: " << (perf->memoryFrequencyHz / 1000000) << " MHz"
              << " max_mem_freq: " << (perf->maxMemoryFrequencyHz / 1000000) << " MHz"
              << " pcie_bw: " << (perf->pcieBandwidthBytes / 1048576) << " MB\n";
        } else {
          oss << "    d3dkmt perf: unavailable\n";
        }
        // PCIe link info via SetupAPI
        if (const auto pcie = queryWinPcieLinkInfo(desc.AdapterLuid)) {
          oss << "    pcie link: Gen" << pcie->currentLinkSpeed << " x" << pcie->currentLinkWidth
              << " (max: Gen" << pcie->maxLinkSpeed << " x" << pcie->maxLinkWidth << ")\n";
        } else {
          oss << "    pcie link: unavailable\n";
        }
      }
    }
    adapter->Release();
  }

  factory->Release();
  return oss.str();
}

}  // namespace aiz

#endif
