#include <aiz/metrics/windows_d3dkmt.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <mutex>

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

}  // namespace aiz

#endif
