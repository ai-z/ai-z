#include <aiz/metrics/windows_d3dkmt.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <mutex>
#include <sstream>

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

using PFN_D3DKMTOpenAdapterFromLuid = NtStatus(WINAPI*)(D3DKMT_OPENADAPTERFROMLUID*);
using PFN_D3DKMTCloseAdapter = NtStatus(WINAPI*)(D3DKMT_CLOSEADAPTER*);
using PFN_D3DKMTQueryVideoMemoryInfo = NtStatus(WINAPI*)(D3DKMT_QUERYVIDEOMEMORYINFO*);

struct D3dkmtApi {
  HMODULE gdi32 = nullptr;
  PFN_D3DKMTOpenAdapterFromLuid openAdapterFromLuid = nullptr;
  PFN_D3DKMTCloseAdapter closeAdapter = nullptr;
  PFN_D3DKMTQueryVideoMemoryInfo queryVideoMemoryInfo = nullptr;
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
          oss << "    d3dkmt: unavailable\n";
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
