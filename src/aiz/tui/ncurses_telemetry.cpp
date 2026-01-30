#include "ncurses_telemetry.h"

#include <aiz/metrics/nvidia_nvml.h>
#include <aiz/metrics/amd_adlx.h>
#include <aiz/metrics/intel_igcl.h>
#if defined(AI_Z_PLATFORM_LINUX)
#include <aiz/metrics/linux_gpu_sysfs.h>
#endif
#if defined(_WIN32)
#include <aiz/metrics/windows_d3dkmt.h>
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

static std::string wideToUtf8(const wchar_t* s) {
  if (!s) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string out(len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), len, nullptr, nullptr);
  return out;
}

static std::string wideViewToUtf8(std::wstring_view s) {
  if (s.empty()) return {};
  std::wstring tmp(s.begin(), s.end());
  return wideToUtf8(tmp.c_str());
}

static bool isIgnoredAdapter(const DXGI_ADAPTER_DESC1& desc, const std::string& name) {
  const bool isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
  const bool isBasicRenderDriver = (desc.VendorId == 0x1414) || (name == "Microsoft Basic Render Driver");
  return isSoftware || isBasicRenderDriver;
}

static std::string dxgiAdapterKey(const DXGI_ADAPTER_DESC1& desc, const std::string& name) {
  std::ostringstream key;
  key << desc.VendorId << ":" << desc.DeviceId << ":" << desc.SubSysId << ":" << desc.Revision << ":";
  key << desc.DedicatedVideoMemory << ":" << desc.SharedSystemMemory << ":" << name;
  return key.str();
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
  std::uint32_t deviceId = 0;
  std::uint32_t revisionId = 0;
  std::uint32_t subSysId = 0;
  std::string name;
};

std::optional<DxgiMemInfo> readDxgiMemInfo(unsigned int index) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    return std::nullopt;
  }

  unsigned int hwIndex = 0;
  std::unordered_map<std::string, bool> seen;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      const std::string name = wideToUtf8(desc.Description);
      if (isIgnoredAdapter(desc, name)) {
        adapter->Release();
        continue;
      }
      const std::string key = dxgiAdapterKey(desc, name);
      if (!seen.emplace(key, true).second) {
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
        std::uint64_t total = 0;
        if (dedicated) {
          total = dedicated;
        } else {
          if (okLocal && local.Budget) total += local.Budget;
          if (okNonLocal && nonLocal.Budget) total += nonLocal.Budget;
        }
        std::uint64_t used = 0;
        if (okLocal) used += local.CurrentUsage;
        if (okNonLocal) used += nonLocal.CurrentUsage;

        DxgiMemInfo info;
        info.luid = desc.AdapterLuid;
        info.totalGiB = total ? static_cast<double>(total) / (1024.0 * 1024.0 * 1024.0) : 0.0;
        info.usedGiB = used ? static_cast<double>(used) / (1024.0 * 1024.0 * 1024.0) : 0.0;
        info.vendorId = desc.VendorId;
        info.deviceId = desc.DeviceId;
        info.revisionId = desc.Revision;
        info.subSysId = desc.SubSysId;
        info.name = name;
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

struct PdhProcMemTotals {
  double usedGiB = 0.0;
};

std::optional<PdhProcMemTotals> readWindowsGpuProcessMemoryFromPdh(const std::optional<LUID>& luidOpt) {
  if (!luidOpt) return std::nullopt;
  if (windowsEnvFlagSet("AI_Z_DISABLE_PDH")) return std::nullopt;
  static PdhQueryState dedicatedUsage;
  static PdhQueryState sharedUsage;

  auto du = readPdhArray(dedicatedUsage, L"\\GPU Process Memory(*)\\Dedicated Usage");
  auto su = readPdhArray(sharedUsage, L"\\GPU Process Memory(*)\\Shared Usage");
  if (!du) return std::nullopt;

  double dedicatedBytes = 0.0;
  double sharedBytes = 0.0;
  for (DWORD i = 0; i < du->count; ++i) {
    const auto* name = du->items[i].szName;
    if (!name) continue;
    const std::wstring_view inst(name);
    LUID instLuid{};
    if (!parseLuidFromInstance(inst, instLuid)) continue;
    if (instLuid.HighPart != luidOpt->HighPart || instLuid.LowPart != luidOpt->LowPart) continue;
    dedicatedBytes += static_cast<double>(du->items[i].FmtValue.largeValue);
  }

  if (su) {
    for (DWORD i = 0; i < su->count; ++i) {
      const auto* name = su->items[i].szName;
      if (!name) continue;
      const std::wstring_view inst(name);
      LUID instLuid{};
      if (!parseLuidFromInstance(inst, instLuid)) continue;
      if (instLuid.HighPart != luidOpt->HighPart || instLuid.LowPart != luidOpt->LowPart) continue;
      sharedBytes += static_cast<double>(su->items[i].FmtValue.largeValue);
    }
  }

  const double used = dedicatedBytes + sharedBytes;
  if (used <= 0.0) return std::nullopt;
  PdhProcMemTotals out;
  out.usedGiB = used / (1024.0 * 1024.0 * 1024.0);
  return out;
}

// Parse PID from instance name like "pid_10160_luid_0x00000000_0x0000acd3_phys_0_eng_0_engtype_3d"
bool parsePidFromInstance(std::wstring_view inst, std::uint32_t& pid) {
  const std::wstring_view key = L"pid_";
  if (inst.size() < key.size() || inst.substr(0, key.size()) != key) return false;
  std::size_t pos = key.size();
  std::uint32_t value = 0;
  std::size_t start = pos;
  while (pos < inst.size() && inst[pos] >= L'0' && inst[pos] <= L'9') {
    value = value * 10 + static_cast<std::uint32_t>(inst[pos] - L'0');
    ++pos;
  }
  if (pos == start) return false;
  pid = value;
  return true;
}

// Internal helper: per-process GPU utilization from GPU Engine counters.
struct ProcGpuAccum {
  double utilPct = 0.0;
  double vramBytes = 0.0;
};

std::unordered_map<std::uint32_t, ProcGpuAccum> readWindowsGpuProcessMapInternal() {
  std::unordered_map<std::uint32_t, ProcGpuAccum> out;
  if (windowsEnvFlagSet("AI_Z_DISABLE_PDH")) return out;

  // Read GPU Engine utilization per process.
  static PdhQueryState gpuEngState;
  auto engArr = readPdhArrayDouble(gpuEngState, L"\\GPU Engine(*)\\Utilization Percentage");
  if (engArr) {
    for (DWORD i = 0; i < engArr->count; ++i) {
      const auto* name = engArr->items[i].szName;
      if (!name) continue;
      const std::wstring_view inst(name);
      // Filter to relevant engine types.
      if (!(containsEngType(inst, L"engtype_3D") ||
            containsEngType(inst, L"engtype_Copy") ||
            containsEngType(inst, L"engtype_Compute") ||
            containsEngType(inst, L"engtype_VideoDecode") ||
            containsEngType(inst, L"engtype_VideoEncode") ||
            containsEngType(inst, L"engtype_VideoProcessing"))) {
        continue;
      }
      std::uint32_t pid = 0;
      if (!parsePidFromInstance(inst, pid) || pid == 0) continue;
      double v = engArr->isLarge ? static_cast<double>(engArr->items[i].FmtValue.largeValue) : engArr->items[i].FmtValue.doubleValue;
      if (!std::isfinite(v)) continue;
      out[pid].utilPct += v;
    }
  }

  // Read GPU Process Memory (VRAM) usage per process.
  static PdhQueryState procDedState;
  static PdhQueryState procSharedState;
  auto pdu = readPdhArray(procDedState, L"\\GPU Process Memory(*)\\Dedicated Usage");
  auto psu = readPdhArray(procSharedState, L"\\GPU Process Memory(*)\\Shared Usage");
  if (pdu) {
    for (DWORD i = 0; i < pdu->count; ++i) {
      const auto* name = pdu->items[i].szName;
      if (!name) continue;
      const std::wstring_view inst(name);
      std::uint32_t pid = 0;
      if (!parsePidFromInstance(inst, pid) || pid == 0) continue;
      const double bytes = static_cast<double>(pdu->items[i].FmtValue.largeValue);
      if (bytes > 0.0) out[pid].vramBytes += bytes;
    }
  }
  if (psu) {
    for (DWORD i = 0; i < psu->count; ++i) {
      const auto* name = psu->items[i].szName;
      if (!name) continue;
      const std::wstring_view inst(name);
      std::uint32_t pid = 0;
      if (!parsePidFromInstance(inst, pid) || pid == 0) continue;
      const double bytes = static_cast<double>(psu->items[i].FmtValue.largeValue);
      if (bytes > 0.0) out[pid].vramBytes += bytes;
    }
  }

  return out;
}


}  // namespace
#endif

#if defined(_WIN32)
std::vector<WindowsGpuProcessInfo> readWindowsGpuProcessList() {
  std::vector<WindowsGpuProcessInfo> out;
  const auto procMap = readWindowsGpuProcessMapInternal();
  out.reserve(procMap.size());
  for (const auto& kv : procMap) {
    // Skip entries with no meaningful GPU activity.
    if (kv.second.utilPct <= 0.0 && kv.second.vramBytes <= 0.0) continue;
    WindowsGpuProcessInfo info;
    info.pid = kv.first;
    info.gpuUtilPct = std::clamp(kv.second.utilPct, 0.0, 100.0);
    info.vramUsedGiB = kv.second.vramBytes / (1024.0 * 1024.0 * 1024.0);
    out.push_back(info);
  }
  return out;
}
#endif

#if defined(_WIN32)
std::string windowsPdhGpuMemoryDiagnostics() {
  std::ostringstream oss;
  oss << "PDH GPU memory diagnostics (Windows)\n";

  if (windowsEnvFlagSet("AI_Z_DISABLE_PDH")) {
    oss << "- status: disabled via AI_Z_DISABLE_PDH\n";
    return oss.str();
  }

  static PdhQueryState dedicatedUsage;
  static PdhQueryState dedicatedLimit;
  static PdhQueryState sharedUsage;
  static PdhQueryState sharedLimit;
  static PdhQueryState procDedicated;
  static PdhQueryState procShared;

  auto du = readPdhArray(dedicatedUsage, L"\\GPU Adapter Memory(*)\\Dedicated Usage");
  auto dl = readPdhArray(dedicatedLimit, L"\\GPU Adapter Memory(*)\\Dedicated Limit");
  auto su = readPdhArray(sharedUsage, L"\\GPU Adapter Memory(*)\\Shared Usage");
  auto sl = readPdhArray(sharedLimit, L"\\GPU Adapter Memory(*)\\Shared Limit");

  auto pdu = readPdhArray(procDedicated, L"\\GPU Process Memory(*)\\Dedicated Usage");
  auto psu = readPdhArray(procShared, L"\\GPU Process Memory(*)\\Shared Usage");

  if (!du) {
    oss << "- adapter memory counters: unavailable\n";
  } else {
    oss << "- adapter memory counters: ok\n";
    for (DWORD i = 0; i < du->count; ++i) {
      const auto* name = du->items[i].szName;
      if (!name) continue;
      const std::wstring_view inst(name);
      LUID instLuid{};
      const bool hasLuid = parseLuidFromInstance(inst, instLuid);
      const double dedicatedUsed = static_cast<double>(du->items[i].FmtValue.largeValue);

      double dedicatedLim = 0.0;
      if (dl) {
        for (DWORD j = 0; j < dl->count; ++j) {
          const auto* name2 = dl->items[j].szName;
          if (!name2) continue;
          if (std::wstring_view(name2) == inst) {
            dedicatedLim = static_cast<double>(dl->items[j].FmtValue.largeValue);
            break;
          }
        }
      }

      double sharedUsed = 0.0;
      if (su) {
        for (DWORD j = 0; j < su->count; ++j) {
          const auto* name2 = su->items[j].szName;
          if (!name2) continue;
          if (std::wstring_view(name2) == inst) {
            sharedUsed = static_cast<double>(su->items[j].FmtValue.largeValue);
            break;
          }
        }
      }

      double sharedLim = 0.0;
      if (sl) {
        for (DWORD j = 0; j < sl->count; ++j) {
          const auto* name2 = sl->items[j].szName;
          if (!name2) continue;
          if (std::wstring_view(name2) == inst) {
            sharedLim = static_cast<double>(sl->items[j].FmtValue.largeValue);
            break;
          }
        }
      }

      oss << "  instance: " << wideViewToUtf8(inst) << "\n";
      if (hasLuid) {
        oss << "    luid: 0x" << std::hex << instLuid.HighPart << ":0x" << instLuid.LowPart << std::dec << "\n";
      }
      oss << "    dedicated used: " << (dedicatedUsed / (1024.0 * 1024.0 * 1024.0)) << " GiB\n";
      if (dedicatedLim > 0.0) oss << "    dedicated limit: " << (dedicatedLim / (1024.0 * 1024.0 * 1024.0)) << " GiB\n";
      if (sharedUsed > 0.0) oss << "    shared used: " << (sharedUsed / (1024.0 * 1024.0 * 1024.0)) << " GiB\n";
      if (sharedLim > 0.0) oss << "    shared limit: " << (sharedLim / (1024.0 * 1024.0 * 1024.0)) << " GiB\n";
    }
  }

  if (!pdu) {
    oss << "- process memory counters: unavailable\n";
  } else {
    oss << "- process memory counters: ok\n";
    for (DWORD i = 0; i < pdu->count; ++i) {
      const auto* name = pdu->items[i].szName;
      if (!name) continue;
      const std::wstring_view inst(name);
      LUID instLuid{};
      const bool hasLuid = parseLuidFromInstance(inst, instLuid);
      const double dedicatedUsed = static_cast<double>(pdu->items[i].FmtValue.largeValue);

      double sharedUsed = 0.0;
      if (psu) {
        for (DWORD j = 0; j < psu->count; ++j) {
          const auto* name2 = psu->items[j].szName;
          if (!name2) continue;
          if (std::wstring_view(name2) == inst) {
            sharedUsed = static_cast<double>(psu->items[j].FmtValue.largeValue);
            break;
          }
        }
      }

      oss << "  proc: " << wideViewToUtf8(inst) << "\n";
      if (hasLuid) {
        oss << "    luid: 0x" << std::hex << instLuid.HighPart << ":0x" << instLuid.LowPart << std::dec << "\n";
      }
      oss << "    dedicated used: " << (dedicatedUsed / (1024.0 * 1024.0 * 1024.0)) << " GiB\n";
      if (sharedUsed > 0.0) oss << "    shared used: " << (sharedUsed / (1024.0 * 1024.0 * 1024.0)) << " GiB\n";
    }
  }

  return oss.str();
}
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
  const bool isAmd = memInfo && (memInfo->vendorId == 0x1002 || memInfo->vendorId == 0x1022);
  const bool isIntel = memInfo && (memInfo->vendorId == 0x8086);
  std::optional<AmdAdapterLuid> luid;
  if (memInfo) luid = AmdAdapterLuid{memInfo->luid.LowPart, memInfo->luid.HighPart};

  if (isIntel && memInfo) {
    IntelAdapterPciIds ids;
    ids.vendorId = memInfo->vendorId;
    ids.deviceId = memInfo->deviceId;
    ids.revisionId = memInfo->revisionId;
    ids.subSysId = memInfo->subSysId;

    if (const auto igcl = readIgclTelemetryForPciIds(ids)) {
      if (igcl->gpuUtilPct) t.utilPct = *igcl->gpuUtilPct;
      if (igcl->tempC) t.tempC = *igcl->tempC;
      if (igcl->powerW) t.watts = *igcl->powerW;
      if (igcl->gpuClockMHz) t.gpuClockMHz = *igcl->gpuClockMHz;
      if (igcl->memClockMHz) t.memClockMHz = *igcl->memClockMHz;
      // PCIe link info from IGCL.
      if (igcl->pcieLinkWidth && *igcl->pcieLinkWidth > 0) {
        t.pcieLinkWidth = static_cast<unsigned int>(*igcl->pcieLinkWidth);
      }
      if (igcl->pcieLinkGen && *igcl->pcieLinkGen > 0) {
        t.pcieLinkGen = static_cast<unsigned int>(*igcl->pcieLinkGen);
      }
      // Fallback: use Windows SetupAPI for PCIe link if IGCL didn't provide it.
      // Only use if values look plausible (Gen2+ and at least x4 lanes for discrete GPUs).
      if (!t.pcieLinkWidth && memInfo) {
        if (const auto pcie = queryWinPcieLinkInfo(memInfo->luid)) {
          // Intel discrete GPUs should be at least PCIe 3.0 x8 - filter out implausible values
          if (pcie->currentLinkWidth >= 4 && pcie->currentLinkSpeed >= 2) {
            t.pcieLinkWidth = static_cast<unsigned int>(pcie->currentLinkWidth);
            t.pcieLinkGen = static_cast<unsigned int>(pcie->currentLinkSpeed);
          }
        }
      }
      // Throttle state as pstate for Intel GPUs.
      if (!igcl->throttleState.empty()) {
        t.pstate = igcl->throttleState;
      }
      if (t.source.empty()) t.source = "igcl";
      any = true;
    }
  }

  if (isAmd) {
    if (const auto adlx = readAdlxTelemetryForDxgi(luid)) {
      if (adlx->gpuUtilPct) t.utilPct = *adlx->gpuUtilPct;
      if (adlx->tempC) t.tempC = *adlx->tempC;
      if (adlx->powerW) t.watts = *adlx->powerW;
      if (adlx->gpuClockMHz) t.gpuClockMHz = *adlx->gpuClockMHz;
      if (adlx->memClockMHz) t.memClockMHz = *adlx->memClockMHz;
      if (adlx->vramUsedGiB) t.vramUsedGiB = *adlx->vramUsedGiB;
      if (adlx->vramTotalGiB) t.vramTotalGiB = *adlx->vramTotalGiB;
      if (t.source.empty()) t.source = "adlx";
      any = true;
    }
  }

  // D3DKMT fallback for VRAM info when DXGI/ADLX can't provide it.
  if (memInfo && (!t.vramUsedGiB || !t.vramTotalGiB || *t.vramTotalGiB <= 0.0)) {
    if (const auto d3 = queryD3dkmtLocalVideoMemoryForLuid(memInfo->luid)) {
      const double total = d3->budgetBytes ? static_cast<double>(d3->budgetBytes) / (1024.0 * 1024.0 * 1024.0) : 0.0;
      const double used = d3->currentUsageBytes ? static_cast<double>(d3->currentUsageBytes) / (1024.0 * 1024.0 * 1024.0) : 0.0;
      if ((!t.vramTotalGiB || *t.vramTotalGiB <= 0.0) && total > 0.0) {
        t.vramTotalGiB = total;
      }
      if ((!t.vramUsedGiB || *t.vramUsedGiB <= 0.0) && used > 0.0) {
        t.vramUsedGiB = used;
      }
      if (t.source.empty()) t.source = "d3dkmt";
      any = true;
    }
  }

  // D3DKMT fallback for performance data (temp, power, fan, memory clock) - works for any vendor.
  if (memInfo) {
    if (!t.tempC || !t.watts || !t.memClockMHz) {
      if (const auto perf = queryD3dkmtAdapterPerfData(memInfo->luid)) {
        if (!t.tempC && perf->temperatureC > 0.0) {
          t.tempC = perf->temperatureC;
        }
        if (!t.watts && perf->powerWatts && *perf->powerWatts > 0.0) {
          t.watts = *perf->powerWatts;
        }
        // Memory clock from D3DKMT (Hz to MHz conversion)
        if (!t.memClockMHz && perf->memoryFrequencyHz > 0) {
          t.memClockMHz = static_cast<unsigned int>(perf->memoryFrequencyHz / 1000000);
        }
        if (t.source.empty()) t.source = "d3dkmt";
        any = true;
      }
    }
  }

  // DXGI fallback for VRAM totals/usage.
  if (memInfo) {
    if ((!t.vramUsedGiB || *t.vramUsedGiB <= 0.0) && memInfo->usedGiB > 0.0) {
      t.vramUsedGiB = memInfo->usedGiB;
    }
    if (!t.vramTotalGiB || *t.vramTotalGiB <= 0.0) t.vramTotalGiB = memInfo->totalGiB;
    if (t.source.empty()) t.source = "dxgi";
    if ((t.vramTotalGiB && *t.vramTotalGiB > 0.0) || (t.vramUsedGiB && *t.vramUsedGiB > 0.0)) any = true;
  }

  // Width alone is still useful; some backends can't reliably report a Gen.
  if (!t.pcieLinkWidth) {
    if (isAmd) {
      if (const auto link = readAdlxPcieLinkForDxgi(luid)) {
        t.pcieLinkWidth = link->width;
        if (link->generation) t.pcieLinkGen = link->generation;
        any = true;
      } else {
        // Still show *why* it's missing (so the UI doesn't just show "--" silently).
        t.pcieLinkNote = amdPcieLinkNoteForDxgi(luid);
      }
    }
  }

  if (!t.utilPct) {
    if (const auto util = readWindowsGpuUtilizationForLuid(memInfo ? std::optional<LUID>(memInfo->luid) : std::nullopt)) {
      t.utilPct = *util;
      if (t.source.empty()) t.source = "pdh";
      any = true;
    }
  }

  if (!t.vramUsedGiB || !t.vramTotalGiB || *t.vramTotalGiB <= 0.0) {
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
  }

  if (memInfo && (!t.vramUsedGiB || *t.vramUsedGiB <= 0.0)) {
    if (const auto procMem = readWindowsGpuProcessMemoryFromPdh(std::optional<LUID>(memInfo->luid))) {
      t.vramUsedGiB = procMem->usedGiB;
      if (t.source.empty()) t.source = "pdh-proc";
      any = true;
    }
  }

  if (!t.memUtilPct && t.vramUsedGiB && t.vramTotalGiB && *t.vramTotalGiB > 0.0) {
    t.memUtilPct = std::clamp((*t.vramUsedGiB / *t.vramTotalGiB) * 100.0, 0.0, 100.0);
  }

  if (t.vramUsedGiB && *t.vramUsedGiB <= 0.0 && t.vramTotalGiB && *t.vramTotalGiB > 0.0) {
    if (t.source == "dxgi" || t.source == "d3dkmt" || t.source == "pdh" || t.source == "pdh-proc") {
      t.vramUsedGiB.reset();
    }
  }

  if (any) return t;
#endif

  return std::nullopt;
}

}  // namespace aiz::ncurses
