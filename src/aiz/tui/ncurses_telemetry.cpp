#include "ncurses_telemetry.h"

#include <aiz/metrics/nvidia_nvml.h>
#include <aiz/metrics/amd_adl.h>
#if defined(AI_Z_PLATFORM_LINUX)
#include <aiz/metrics/linux_gpu_sysfs.h>
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#endif

#include <ios>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace aiz::ncurses {

namespace {
std::string fmt1(double v) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(1);
  oss << v;
  return oss.str();
}

std::string fmt0(double v) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(0);
  oss << v;
  return oss.str();
}
}  // namespace

#if defined(_WIN32)
namespace {

static bool windowsEnvFlagSet(const char* name) {
  if (!name) return false;
  if (const char* v = std::getenv(name)) {
    if (*v == '\0') return false;
    if (v[0] == '0' && v[1] == '\0') return false;
    return true;
  }
  return false;
}

struct PdhQueryState {
  HQUERY query = nullptr;
  HCOUNTER counter = nullptr;
  bool initialized = false;
  bool primed = false;
};

struct PdhArrayResult {
  // PDH writes structured data into this buffer and assumes natural alignment.
  // Using std::byte would only guarantee 1-byte alignment, which can crash.
  std::vector<std::uint64_t> buffer;
  PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
  DWORD count = 0;
};

std::optional<PdhArrayResult> readPdhArray(PdhQueryState& state, const wchar_t* path) {
  if (!state.initialized) {
    state.initialized = true;
    if (PdhOpenQueryW(nullptr, 0, &state.query) != ERROR_SUCCESS) return std::nullopt;
    if (PdhAddEnglishCounterW(state.query, path, 0, &state.counter) != ERROR_SUCCESS) {
      PdhCloseQuery(state.query);
      state.query = nullptr;
      state.counter = nullptr;
      return std::nullopt;
    }
  }

  if (!state.query || !state.counter) return std::nullopt;
  if (PdhCollectQueryData(state.query) != ERROR_SUCCESS) return std::nullopt;
  if (!state.primed) {
    state.primed = true;
    return std::nullopt;
  }

  DWORD bufferSize = 0;
  DWORD itemCount = 0;
  PDH_STATUS status = PdhGetFormattedCounterArrayW(state.counter, PDH_FMT_LARGE, &bufferSize, &itemCount, nullptr);
  if (status != PDH_MORE_DATA || bufferSize == 0 || itemCount == 0) return std::nullopt;

  PdhArrayResult result;
  const std::size_t words = (static_cast<std::size_t>(bufferSize) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
  result.buffer.resize(words);
  result.items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(result.buffer.data());
  result.count = itemCount;

  DWORD alignedBytes = static_cast<DWORD>(result.buffer.size() * sizeof(std::uint64_t));
  DWORD itemCount2 = itemCount;
  status = PdhGetFormattedCounterArrayW(state.counter, PDH_FMT_LARGE, &alignedBytes, &itemCount2, result.items);
  if (status != ERROR_SUCCESS) return std::nullopt;
  result.count = itemCount2;
  return result;
}

struct PdhArrayResultDouble {
  // PDH writes structured data into this buffer and assumes natural alignment.
  std::vector<std::uint64_t> buffer;
  PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
  DWORD count = 0;
  bool isLarge = false;
};

std::optional<PdhArrayResultDouble> readPdhArrayDouble(PdhQueryState& state, const wchar_t* path) {
  if (!state.initialized) {
    state.initialized = true;
    if (PdhOpenQueryW(nullptr, 0, &state.query) != ERROR_SUCCESS) return std::nullopt;
    if (PdhAddEnglishCounterW(state.query, path, 0, &state.counter) != ERROR_SUCCESS) {
      PdhCloseQuery(state.query);
      state.query = nullptr;
      state.counter = nullptr;
      return std::nullopt;
    }
  }

  if (!state.query || !state.counter) return std::nullopt;
  if (PdhCollectQueryData(state.query) != ERROR_SUCCESS) return std::nullopt;
  if (!state.primed) {
    state.primed = true;
    return std::nullopt;
  }

  DWORD bufferSize = 0;
  DWORD itemCount = 0;
  PDH_STATUS status = PdhGetFormattedCounterArrayW(state.counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);
  bool useLarge = false;
  if (status != PDH_MORE_DATA || bufferSize == 0 || itemCount == 0) {
    status = PdhGetFormattedCounterArrayW(state.counter, PDH_FMT_LARGE, &bufferSize, &itemCount, nullptr);
    useLarge = true;
    if (status != PDH_MORE_DATA || bufferSize == 0 || itemCount == 0) return std::nullopt;
  }

  PdhArrayResultDouble result;
  const std::size_t words = (static_cast<std::size_t>(bufferSize) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
  result.buffer.resize(words);
  result.items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(result.buffer.data());
  result.count = itemCount;

  DWORD alignedBytes = static_cast<DWORD>(result.buffer.size() * sizeof(std::uint64_t));
  DWORD itemCount2 = itemCount;
  status = PdhGetFormattedCounterArrayW(state.counter, useLarge ? PDH_FMT_LARGE : PDH_FMT_DOUBLE, &alignedBytes, &itemCount2, result.items);
  if (status != ERROR_SUCCESS) return std::nullopt;
  result.count = itemCount2;
  result.isLarge = useLarge;
  return result;
}

struct DxgiMemInfo {
  LUID luid{};
  double usedGiB = 0.0;
  double totalGiB = 0.0;
  std::uint32_t vendorId = 0;
  std::string name;
  std::uint32_t bus = 0;
  std::uint32_t device = 0;
  std::uint32_t function = 0;
};

std::optional<DxgiMemInfo> readDxgiMemInfo(unsigned int index) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    return std::nullopt;
  }

  unsigned int hwIndex = 0;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
        adapter->Release();
        continue;
      }
      if (hwIndex == index) {
        IDXGIAdapter3* adapter3 = nullptr;
        if (FAILED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
          adapter->Release();
          factory->Release();
          return std::nullopt;
        }

        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
        const bool okLocal = SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local));
        const bool okNonLocal = SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal));

        const std::uint64_t dedicated = desc.DedicatedVideoMemory;
        const std::uint64_t total = dedicated ? dedicated
          : (okLocal && local.Budget ? local.Budget
          : (okNonLocal ? nonLocal.Budget : 0));
        const std::uint64_t used = dedicated ? (okLocal ? local.CurrentUsage : 0)
          : (okNonLocal ? nonLocal.CurrentUsage : 0);

        DxgiMemInfo info;
        info.luid = desc.AdapterLuid;
        info.totalGiB = total ? static_cast<double>(total) / (1024.0 * 1024.0 * 1024.0) : 0.0;
        info.usedGiB = used ? static_cast<double>(used) / (1024.0 * 1024.0 * 1024.0) : 0.0;
        info.vendorId = desc.VendorId;
        info.name = std::wstring(desc.Description).empty() ? std::string() : std::string();
        if (desc.Description[0] != L'\0') {
          int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
          if (len > 1) {
            std::string out(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, out.data(), len, nullptr, nullptr);
            info.name = std::move(out);
          }
        }
        info.bus = static_cast<std::uint32_t>((desc.AdapterLuid.HighPart >> 16) & 0xFF);
        info.device = static_cast<std::uint32_t>((desc.AdapterLuid.HighPart >> 8) & 0xFF);
        info.function = static_cast<std::uint32_t>(desc.AdapterLuid.HighPart & 0xFF);

        adapter3->Release();
        adapter->Release();
        factory->Release();
        if (info.totalGiB > 0.0) return info;
        return std::nullopt;
      }
      ++hwIndex;
    }
    adapter->Release();
  }

  factory->Release();
  return std::nullopt;
}

std::optional<unsigned int> readDxgiAmdOrdinal(unsigned int index) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    return std::nullopt;
  }

  unsigned int hwIndex = 0;
  unsigned int amdOrdinal = 0;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
        if (desc.VendorId == 0x1002 || desc.VendorId == 0x1022) {
          if (hwIndex == index) {
            adapter->Release();
            factory->Release();
            return amdOrdinal;
          }
          ++amdOrdinal;
        }
        ++hwIndex;
      }
    }
    adapter->Release();
  }

  factory->Release();
  return std::nullopt;
}

// AMD PCIe link info via shared ADL helper (best-effort).
std::optional<std::pair<unsigned int, unsigned int>> readAdlPcieLinkForDxgiName(const std::string& dxgiName,
                                                                               const std::optional<aiz::AdlAdapterLuid>& adapterLuid,
                                                                               const std::optional<unsigned int>& amdOrdinal,
                                                                               const std::optional<DxgiMemInfo>& memInfo) {
  std::optional<::aiz::AdlPciLocation> loc;
  if (memInfo) {
    loc = ::aiz::AdlPciLocation{memInfo->bus, memInfo->device, memInfo->function};
  }
  const auto link = ::aiz::readAdlPcieLinkForDxgi(dxgiName, adapterLuid, amdOrdinal, loc);
  if (!link) return std::nullopt;
  return std::make_pair(link->generation, link->width);
}

bool containsEngType(std::wstring_view name, std::wstring_view token) {
  return name.find(token) != std::wstring_view::npos;
}

bool parseHex(std::wstring_view s, std::size_t& pos, std::uint32_t& out) {
  std::uint32_t value = 0;
  std::size_t i = pos;
  if (i + 1 < s.size() && s[i] == L'0' && (s[i + 1] == L'x' || s[i + 1] == L'X')) {
    i += 2;
  }
  std::size_t start = i;
  while (i < s.size() && std::iswxdigit(s[i])) {
    wchar_t ch = s[i];
    value *= 16;
    if (ch >= L'0' && ch <= L'9') value += static_cast<std::uint32_t>(ch - L'0');
    else if (ch >= L'a' && ch <= L'f') value += static_cast<std::uint32_t>(10 + (ch - L'a'));
    else if (ch >= L'A' && ch <= L'F') value += static_cast<std::uint32_t>(10 + (ch - L'A'));
    ++i;
  }
  if (i == start) return false;
  pos = i;
  out = value;
  return true;
}

bool parseLuidFromInstance(std::wstring_view inst, LUID& out) {
  const std::wstring_view key = L"luid_";
  const std::size_t start = inst.find(key);
  if (start == std::wstring_view::npos) return false;
  std::size_t pos = start + key.size();
  std::uint32_t high = 0;
  std::uint32_t low = 0;
  if (!parseHex(inst, pos, high)) return false;
  if (pos >= inst.size() || inst[pos] != L'_') return false;
  ++pos;
  if (!parseHex(inst, pos, low)) return false;
  out.HighPart = static_cast<long>(high);
  out.LowPart = static_cast<unsigned long>(low);
  return true;
}

std::optional<double> readWindowsGpuUtilizationForLuid(const std::optional<LUID>& luidOpt) {
  if (windowsEnvFlagSet("AI_Z_DISABLE_PDH")) return std::nullopt;
  static PdhQueryState state;
  auto arr = readPdhArrayDouble(state, L"\\GPU Engine(*)\\Utilization Percentage");
  if (!arr) return std::nullopt;

  const bool hasLuid = luidOpt.has_value();

  double total = 0.0;
  double matched = 0.0;
  bool hasMatch = false;
  for (DWORD i = 0; i < arr->count; ++i) {
    const auto* name = arr->items[i].szName;
    if (!name) continue;
    const std::wstring_view inst(name);
    if (!(containsEngType(inst, L"engtype_3D") ||
          containsEngType(inst, L"engtype_Copy") ||
          containsEngType(inst, L"engtype_Compute") ||
          containsEngType(inst, L"engtype_VideoDecode") ||
          containsEngType(inst, L"engtype_VideoEncode") ||
          containsEngType(inst, L"engtype_VideoProcessing"))) {
      continue;
    }

    const double v = arr->items[i].FmtValue.doubleValue;
    if (!std::isfinite(v)) continue;
    total += v;
    if (hasLuid) {
      LUID instLuid{};
      if (parseLuidFromInstance(inst, instLuid) && instLuid.HighPart == luidOpt->HighPart && instLuid.LowPart == luidOpt->LowPart) {
        matched += v;
        hasMatch = true;
      }
    }
  }

  double out = hasMatch ? matched : total;
  out = std::clamp(out, 0.0, 100.0);
  return out;
}

struct PdhMemTotals {
  double usedGiB = 0.0;
  double totalGiB = 0.0;
};

struct LuidKey {
  std::uint32_t high = 0;
  std::uint32_t low = 0;

  bool operator==(const LuidKey& other) const {
    return high == other.high && low == other.low;
  }
};

struct LuidKeyHash {
  std::size_t operator()(const LuidKey& key) const noexcept {
    return (static_cast<std::size_t>(key.high) << 32) ^ key.low;
  }
};

struct MemAccum {
  double dedicatedUsed = 0.0;
  double dedicatedLimit = 0.0;
  double sharedUsed = 0.0;
  double sharedLimit = 0.0;
};

std::optional<PdhMemTotals> readWindowsGpuMemoryFromPdh(unsigned int index, const std::optional<LUID>& luidOpt) {
  if (windowsEnvFlagSet("AI_Z_DISABLE_PDH")) return std::nullopt;
  static PdhQueryState dedicatedUsage;
  static PdhQueryState dedicatedLimit;
  static PdhQueryState sharedUsage;
  static PdhQueryState sharedLimit;

  auto du = readPdhArray(dedicatedUsage, L"\\GPU Adapter Memory(*)\\Dedicated Usage");
  auto dl = readPdhArray(dedicatedLimit, L"\\GPU Adapter Memory(*)\\Dedicated Limit");
  auto su = readPdhArray(sharedUsage, L"\\GPU Adapter Memory(*)\\Shared Usage");
  auto sl = readPdhArray(sharedLimit, L"\\GPU Adapter Memory(*)\\Shared Limit");

  if (!du) return std::nullopt;

  std::unordered_map<LuidKey, MemAccum, LuidKeyHash> map;
  std::vector<LuidKey> order;

  auto addArray = [&](const PdhArrayResult& arr, auto setter) {
    for (DWORD i = 0; i < arr.count; ++i) {
      const auto* name = arr.items[i].szName;
      if (!name) continue;
      const std::wstring_view inst(name);
      LUID instLuid{};
      if (!parseLuidFromInstance(inst, instLuid)) continue;
      const LuidKey key{static_cast<std::uint32_t>(instLuid.HighPart), static_cast<std::uint32_t>(instLuid.LowPart)};
      if (map.find(key) == map.end()) {
        order.push_back(key);
      }
      const double v = static_cast<double>(arr.items[i].FmtValue.largeValue);
      setter(map[key], v);
    }
  };

  addArray(*du, [](MemAccum& a, double v) { a.dedicatedUsed += v; });
  if (dl) addArray(*dl, [](MemAccum& a, double v) { a.dedicatedLimit += v; });
  if (su) addArray(*su, [](MemAccum& a, double v) { a.sharedUsed += v; });
  if (sl) addArray(*sl, [](MemAccum& a, double v) { a.sharedLimit += v; });

  auto makeTotals = [](const MemAccum& a) -> PdhMemTotals {
    double total = a.dedicatedLimit > 0.0 ? a.dedicatedLimit : a.sharedLimit;
    double used = a.dedicatedUsed > 0.0 ? a.dedicatedUsed : a.sharedUsed;
    if (total <= 0.0) total = a.dedicatedLimit + a.sharedLimit;
    if (used <= 0.0) used = a.dedicatedUsed + a.sharedUsed;
    PdhMemTotals out;
    out.usedGiB = used > 0.0 ? (used / (1024.0 * 1024.0 * 1024.0)) : 0.0;
    out.totalGiB = total > 0.0 ? (total / (1024.0 * 1024.0 * 1024.0)) : 0.0;
    return out;
  };

  if (luidOpt) {
    const LuidKey key{static_cast<std::uint32_t>(luidOpt->HighPart), static_cast<std::uint32_t>(luidOpt->LowPart)};
    auto it = map.find(key);
    if (it != map.end()) {
      const auto out = makeTotals(it->second);
      if (out.totalGiB > 0.0 || out.usedGiB > 0.0) return out;
    }
  }

  if (index < order.size()) {
    auto it = map.find(order[index]);
    if (it != map.end()) {
      const auto out = makeTotals(it->second);
      if (out.totalGiB > 0.0 || out.usedGiB > 0.0) return out;
    }
  }

  if (!map.empty()) {
    MemAccum total{};
    for (const auto& kv : map) {
      total.dedicatedUsed += kv.second.dedicatedUsed;
      total.dedicatedLimit += kv.second.dedicatedLimit;
      total.sharedUsed += kv.second.sharedUsed;
      total.sharedLimit += kv.second.sharedLimit;
    }
    const auto out = makeTotals(total);
    if (out.totalGiB > 0.0 || out.usedGiB > 0.0) return out;
  }

  return std::nullopt;
}

}  // namespace
#endif

std::string formatRamText(const std::optional<RamUsage>& ram) {
  if (!ram) return "--";
  return fmt1(ram->usedGiB) + "/" + fmt1(ram->totalGiB) + "G(" + fmt0(ram->usedPct) + "%)";
}

std::optional<GpuTelemetry> readGpuTelemetryPreferNvml(unsigned int index) {
  GpuTelemetry t;
  bool any = false;

  if (const auto nv = readNvmlTelemetryForGpu(index)) {
    t.utilPct = nv->gpuUtilPct;
    t.memUtilPct = nv->memUtilPct;
    t.vramUsedGiB = nv->memUsedGiB;
    t.vramTotalGiB = nv->memTotalGiB;
    t.watts = nv->powerWatts;
    t.tempC = nv->tempC;
    t.pstate = nv->pstate;
    if (nv->gpuClockMHz > 0) t.gpuClockMHz = nv->gpuClockMHz;
    if (nv->memClockMHz > 0) t.memClockMHz = nv->memClockMHz;
    if (nv->encoderUtilPct >= 0.0) t.encoderUtilPct = nv->encoderUtilPct;
    if (nv->decoderUtilPct >= 0.0) t.decoderUtilPct = nv->decoderUtilPct;
    t.source = "nvml";
    any = true;
  }

  // Query PCIe link info independently: telemetry calls can fail while link queries still work.
  if (const auto link = readNvmlPcieLinkForGpu(index)) {
    t.pcieLinkWidth = link->width;
    t.pcieLinkGen = link->generation;
    any = true;
  }

  if (any) return t;

#if defined(AI_Z_PLATFORM_LINUX)
  // AMD/Intel via Linux sysfs (best-effort).
  if (const auto lt = readLinuxGpuTelemetry(index)) {
    t.utilPct = lt->utilPct;
    t.vramUsedGiB = lt->vramUsedGiB;
    t.vramTotalGiB = lt->vramTotalGiB;
    t.watts = lt->watts;
    t.tempC = lt->tempC;
    t.pstate = lt->pstate;
    t.source = lt->source;
    return t;
  }
#endif

#if defined(_WIN32)
  std::optional<DxgiMemInfo> memInfo = readDxgiMemInfo(index);
  if (memInfo) {
    t.vramUsedGiB = memInfo->usedGiB;
    t.vramTotalGiB = memInfo->totalGiB;
    t.source = "dxgi";
    any = true;
  }

  // Width alone is still useful; some backends can't reliably report a Gen.
  if (!t.pcieLinkWidth) {
    const auto amdOrdinal = readDxgiAmdOrdinal(index);
    const std::string dxgiName = memInfo ? memInfo->name : std::string();
    const bool isAmd = (memInfo && (memInfo->vendorId == 0x1002 || memInfo->vendorId == 0x1022)) || amdOrdinal.has_value();
    if (isAmd) {
      std::optional<AdlPciLocation> loc;
      if (memInfo) loc = AdlPciLocation{memInfo->bus, memInfo->device, memInfo->function};

      std::optional<AdlAdapterLuid> luid;
      if (memInfo) {
        luid = AdlAdapterLuid{memInfo->luid.LowPart, memInfo->luid.HighPart};
      }

      if (const auto link = readAdlPcieLinkForDxgi(dxgiName, luid, amdOrdinal, loc)) {
        t.pcieLinkWidth = link->width;
        if (link->generation) t.pcieLinkGen = link->generation;
        any = true;
      } else {
        // Still show *why* it's missing (so the UI doesn't just show "--" silently).
        t.pcieLinkNote = amdPcieLinkNoteForDxgi(luid);
      }
    }
  }

  if (const auto util = readWindowsGpuUtilizationForLuid(memInfo ? std::optional<LUID>(memInfo->luid) : std::nullopt)) {
    t.utilPct = *util;
    if (t.source.empty()) t.source = "pdh";
    any = true;
  }

  if (const auto pdhMem = readWindowsGpuMemoryFromPdh(index, memInfo ? std::optional<LUID>(memInfo->luid) : std::nullopt)) {
    if (!t.vramTotalGiB || *t.vramTotalGiB <= 0.0 || pdhMem->totalGiB > *t.vramTotalGiB) {
      t.vramTotalGiB = pdhMem->totalGiB;
    }
    if (!t.vramUsedGiB || *t.vramUsedGiB <= 0.0 || pdhMem->usedGiB > 0.0) {
      t.vramUsedGiB = pdhMem->usedGiB;
    }
    if (t.source.empty()) t.source = "pdh";
    any = true;
  }

  if (any) return t;
#endif

  return std::nullopt;
}

}  // namespace aiz::ncurses
