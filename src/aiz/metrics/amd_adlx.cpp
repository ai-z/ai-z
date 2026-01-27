#include <aiz/metrics/amd_adlx.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dxgi1_2.h>

#if !defined(AI_Z_HAS_ADLX_HEADERS) || (AI_Z_HAS_ADLX_HEADERS == 0)

namespace aiz {

AdlxAvailability adlxAvailability() {
  return {};
}

std::string adlxDiagnostics() {
  return std::string("ADLX diagnostics (Windows)\n- status: unavailable (ADLX headers not found at build time)\n");
}

std::string pcieDiagnostics() {
  return std::string("PCIe diagnostics (Windows)\n- status: unavailable (ADLX headers not found at build time)\n");
}

std::string amdPcieLinkNoteForDxgi(const std::optional<AmdAdapterLuid>&) {
  return std::string("ADLX headers missing");
}

AdlxStatus adlxStatus() {
  return AdlxStatus::Unusable;
}

std::optional<AmdPcieLink> readAdlxPcieLinkForDxgi(const std::optional<AmdAdapterLuid>&) {
  return std::nullopt;
}

std::optional<AdlxGpuTelemetry> readAdlxTelemetryForDxgi(const std::optional<AmdAdapterLuid>&) {
  return std::nullopt;
}

}  // namespace aiz

#else

#include <ADLX.h>
#include <IPerformanceMonitoring.h>
#include <ISystem.h>
#include <ISystem1.h>
#include <ISystem2.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aiz {
namespace {

static std::wstring modulePathW(HMODULE module) {
  if (!module) return {};
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(module, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return std::wstring(buf, buf + n);
}

static std::string narrowUtf8FromWide(const std::wstring& w) {
  if (w.empty()) return {};
  int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
  return out;
}

static std::string toLowerAscii(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static bool containsCaseInsensitiveAscii(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return false;
  const auto h = toLowerAscii(haystack);
  const auto n = toLowerAscii(needle);
  return h.find(n) != std::string::npos;
}

static std::vector<std::string> listPeExportNames(HMODULE module) {
  std::vector<std::string> out;
  if (!module) return out;

  auto* base = reinterpret_cast<std::byte*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return out;

  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return out;

  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (dir.VirtualAddress == 0 || dir.Size == 0) return out;

  auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
  if (!exp->AddressOfNames || exp->NumberOfNames == 0) return out;

  auto* names = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
  out.reserve(static_cast<std::size_t>(exp->NumberOfNames));
  for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
    const DWORD rva = names[i];
    if (!rva) continue;
    const char* s = reinterpret_cast<const char*>(base + rva);
    if (!s) continue;
    out.emplace_back(s);
  }
  return out;
}

static unsigned int pcieGenFromAdlxPciBusType(ADLX_PCI_BUS_TYPE type) {
  switch (type) {
    case PCIE_2_0: return 2;
    case PCIE_3_0: return 3;
    case PCIE_4_0: return 4;
    default: return 0;
  }
}

struct AdlxRuntime {
  HMODULE mod = nullptr;
  std::string dllName;
  std::string dllPath;
  ADLXInitialize_Fn init = nullptr;
  ADLXTerminate_Fn term = nullptr;
  bool ready = false;
};

struct AdlxSession {
  std::mutex mutex;
  bool initAttempted = false;
  bool initOk = false;
  adlx::IADLXSystem* system = nullptr;
};

static AdlxRuntime& adlxRuntime() {
  static AdlxRuntime rt;
  static bool initialized = false;
  if (initialized) return rt;
  initialized = true;

  rt.mod = LoadLibraryW(ADLX_DLL_NAMEW);
  if (!rt.mod) return rt;

  rt.dllName = "amdadlx64.dll";
  rt.dllPath = narrowUtf8FromWide(modulePathW(rt.mod));

  rt.init = reinterpret_cast<ADLXInitialize_Fn>(GetProcAddress(rt.mod, ADLX_INIT_FUNCTION_NAME));
  rt.term = reinterpret_cast<ADLXTerminate_Fn>(GetProcAddress(rt.mod, ADLX_TERMINATE_FUNCTION_NAME));
  if (!rt.init || !rt.term) return rt;

  rt.ready = true;
  return rt;
}

static AdlxSession& adlxSession() {
  static AdlxSession session;
  return session;
}

static adlx::IADLXSystem* adlxSystem() {
  auto& rt = adlxRuntime();
  if (!rt.ready) return nullptr;

  auto& session = adlxSession();
  const std::lock_guard<std::mutex> lock(session.mutex);

  if (session.initAttempted) return session.initOk ? session.system : nullptr;
  session.initAttempted = true;

  adlx::IADLXSystem* system = nullptr;
  const ADLX_RESULT initRes = rt.init(ADLX_FULL_VERSION, &system);
  if (initRes != ADLX_OK || !system) {
    session.initOk = false;
    session.system = nullptr;
    return nullptr;
  }

  session.initOk = true;
  session.system = system;

  std::atexit([]() {
    auto& s = adlxSession();
    auto& r = adlxRuntime();
    const std::lock_guard<std::mutex> guard(s.mutex);
    s.system = nullptr;
    s.initOk = false;
    // Best-effort: allow ADLX to clean up.
    if (r.term) (void)r.term();
  });

  return session.system;
}

static std::optional<AmdPcieLink> queryAdlxPcieLinkForAdapterLuid(const LUID& luid) {
  const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32ull) |
                            static_cast<std::uint64_t>(luid.LowPart);

  static std::mutex cacheMutex;
  static std::unordered_map<std::uint64_t, AmdPcieLink> cacheOk;
  static std::unordered_set<std::uint64_t> cacheFail;
  {
    const std::lock_guard<std::mutex> lock(cacheMutex);
    if (const auto it = cacheOk.find(key); it != cacheOk.end()) return it->second;
    if (cacheFail.find(key) != cacheFail.end()) return std::nullopt;
  }

  auto* system = adlxSystem();
  if (!system) {
    const std::lock_guard<std::mutex> lock(cacheMutex);
    cacheFail.insert(key);
    return std::nullopt;
  }

  adlx::IADLXGPUList* gpus = nullptr;
  if (system->GetGPUs(&gpus) != ADLX_OK || !gpus) {
    const std::lock_guard<std::mutex> lock(cacheMutex);
    cacheFail.insert(key);
    return std::nullopt;
  }

  auto releaseIface = [](auto*& p) {
    if (p) {
      p->Release();
      p = nullptr;
    }
  };

  const adlx_uint count = gpus->Size();
  for (adlx_uint i = 0; i < count; ++i) {
    adlx::IADLXGPU* gpu = nullptr;
    if (gpus->At(i, &gpu) != ADLX_OK || !gpu) continue;

    adlx::IADLXGPU2* gpu2 = nullptr;
    if (gpu->QueryInterface(adlx::IADLXGPU2::IID(), reinterpret_cast<void**>(&gpu2)) != ADLX_OK || !gpu2) {
      releaseIface(gpu);
      continue;
    }

    ADLX_LUID adlxLuid{};
    const bool luidOk = (gpu2->LUID(&adlxLuid) == ADLX_OK);
    releaseIface(gpu2);
    if (!luidOk) {
      releaseIface(gpu);
      continue;
    }

    if (adlxLuid.highPart != luid.HighPart || adlxLuid.lowPart != luid.LowPart) {
      releaseIface(gpu);
      continue;
    }

    adlx::IADLXGPU1* gpu1 = nullptr;
    if (gpu->QueryInterface(adlx::IADLXGPU1::IID(), reinterpret_cast<void**>(&gpu1)) != ADLX_OK || !gpu1) {
      releaseIface(gpu);
      break;
    }

    ADLX_PCI_BUS_TYPE busType = UNDEFINED;
    adlx_uint laneWidth = 0;
    const bool hasBusType = (gpu1->PCIBusType(&busType) == ADLX_OK);
    const bool hasWidth = (gpu1->PCIBusLaneWidth(&laneWidth) == ADLX_OK);

    releaseIface(gpu1);
    releaseIface(gpu);

    if (!hasWidth || laneWidth == 0) break;

    AmdPcieLink out;
    out.width = static_cast<unsigned int>(laneWidth);
    out.generation = hasBusType ? pcieGenFromAdlxPciBusType(busType) : 0u;
    out.maxWidth = 0;
    out.maxGeneration = 0;

    {
      const std::lock_guard<std::mutex> lock(cacheMutex);
      cacheOk[key] = out;
    }

    releaseIface(gpus);
    return out;
  }

  releaseIface(gpus);
  {
    const std::lock_guard<std::mutex> lock(cacheMutex);
    cacheFail.insert(key);
  }
  return std::nullopt;
}

static std::optional<std::string> queryAdlxPcieNoteForAdapterLuid(const LUID& luid) {
  const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32ull) |
                            static_cast<std::uint64_t>(luid.LowPart);

  static std::mutex noteMutex;
  static std::unordered_map<std::uint64_t, std::string> noteCache;
  {
    const std::lock_guard<std::mutex> lock(noteMutex);
    if (const auto it = noteCache.find(key); it != noteCache.end()) {
      if (it->second.empty()) return std::nullopt;
      return it->second;
    }
  }

  auto* system = adlxSystem();
  if (!system) {
    const std::lock_guard<std::mutex> lock(noteMutex);
    noteCache[key] = std::string();
    return std::nullopt;
  }

  adlx::IADLXGPUList* gpus = nullptr;
  if (system->GetGPUs(&gpus) != ADLX_OK || !gpus) {
    const std::lock_guard<std::mutex> lock(noteMutex);
    noteCache[key] = std::string();
    return std::nullopt;
  }

  auto releaseIface = [](auto*& p) {
    if (p) {
      p->Release();
      p = nullptr;
    }
  };

  std::optional<std::string> result;
  const adlx_uint count = gpus->Size();
  for (adlx_uint i = 0; i < count; ++i) {
    adlx::IADLXGPU* gpu = nullptr;
    if (gpus->At(i, &gpu) != ADLX_OK || !gpu) continue;

    adlx::IADLXGPU2* gpu2 = nullptr;
    if (gpu->QueryInterface(adlx::IADLXGPU2::IID(), reinterpret_cast<void**>(&gpu2)) != ADLX_OK || !gpu2) {
      releaseIface(gpu);
      continue;
    }

    ADLX_LUID adlxLuid{};
    const bool luidOk = (gpu2->LUID(&adlxLuid) == ADLX_OK);
    releaseIface(gpu2);
    if (!luidOk) {
      releaseIface(gpu);
      continue;
    }

    if (adlxLuid.highPart != luid.HighPart || adlxLuid.lowPart != luid.LowPart) {
      releaseIface(gpu);
      continue;
    }

    ADLX_GPU_TYPE gpuType = GPUTYPE_UNDEFINED;
    (void)gpu->Type(&gpuType);

    adlx::IADLXGPU1* gpu1 = nullptr;
    const ADLX_RESULT qi1Res = gpu->QueryInterface(adlx::IADLXGPU1::IID(), reinterpret_cast<void**>(&gpu1));
    adlx_uint laneWidth = 0;
    if (qi1Res == ADLX_OK && gpu1) {
      (void)gpu1->PCIBusLaneWidth(&laneWidth);
      releaseIface(gpu1);
    }

    if (gpuType == GPUTYPE_INTEGRATED && laneWidth == 0) {
      result = std::string("integrated (no PCIe)");
    } else if (laneWidth == 0) {
      result = std::string("ADLX lane width unavailable");
    }

    releaseIface(gpu);
    break;
  }

  releaseIface(gpus);

  {
    const std::lock_guard<std::mutex> lock(noteMutex);
    noteCache[key] = result.value_or(std::string());
  }
  return result;
}

static std::optional<AdlxGpuTelemetry> queryAdlxTelemetryForAdapterLuid(const LUID& luid) {
  auto* system = adlxSystem();
  if (!system) return std::nullopt;

  adlx::IADLXGPUList* gpus = nullptr;
  if (system->GetGPUs(&gpus) != ADLX_OK || !gpus) return std::nullopt;

  auto releaseIface = [](auto*& p) {
    if (p) {
      p->Release();
      p = nullptr;
    }
  };

  adlx::IADLXPerformanceMonitoringServices* perf = nullptr;
  (void)system->GetPerformanceMonitoringServices(&perf);

  const adlx_uint count = gpus->Size();
  for (adlx_uint i = 0; i < count; ++i) {
    adlx::IADLXGPU* gpu = nullptr;
    if (gpus->At(i, &gpu) != ADLX_OK || !gpu) continue;

    adlx::IADLXGPU2* gpu2 = nullptr;
    if (gpu->QueryInterface(adlx::IADLXGPU2::IID(), reinterpret_cast<void**>(&gpu2)) != ADLX_OK || !gpu2) {
      releaseIface(gpu);
      continue;
    }

    ADLX_LUID adlxLuid{};
    const bool luidOk = (gpu2->LUID(&adlxLuid) == ADLX_OK);
    releaseIface(gpu2);
    if (!luidOk) {
      releaseIface(gpu);
      continue;
    }

    if (adlxLuid.highPart != luid.HighPart || adlxLuid.lowPart != luid.LowPart) {
      releaseIface(gpu);
      continue;
    }

    AdlxGpuTelemetry out;
    bool any = false;

    adlx_uint totalVramMB = 0;
    if (gpu->TotalVRAM(&totalVramMB) == ADLX_OK && totalVramMB > 0) {
      out.vramTotalGiB = static_cast<double>(totalVramMB) / 1024.0;
      any = true;
    }

    if (perf) {
      adlx::IADLXGPUMetrics* metrics = nullptr;
      if (perf->GetCurrentGPUMetrics(gpu, &metrics) == ADLX_OK && metrics) {
        adlx_double usage = 0.0;
        if (metrics->GPUUsage(&usage) == ADLX_OK && std::isfinite(usage)) {
          out.gpuUtilPct = std::clamp(static_cast<double>(usage), 0.0, 100.0);
          any = true;
        }

        adlx_int gpuClock = 0;
        if (metrics->GPUClockSpeed(&gpuClock) == ADLX_OK && gpuClock > 0) {
          out.gpuClockMHz = static_cast<unsigned int>(gpuClock);
          any = true;
        }

        adlx_int memClock = 0;
        if (metrics->GPUVRAMClockSpeed(&memClock) == ADLX_OK && memClock > 0) {
          out.memClockMHz = static_cast<unsigned int>(memClock);
          any = true;
        }

        adlx_double temp = 0.0;
        if (metrics->GPUTemperature(&temp) == ADLX_OK && std::isfinite(temp)) {
          out.tempC = static_cast<double>(temp);
          any = true;
        }

        adlx_double power = 0.0;
        if (metrics->GPUPower(&power) == ADLX_OK && std::isfinite(power)) {
          out.powerW = static_cast<double>(power);
          any = true;
        }

        adlx_int vramUsedMB = 0;
        if (metrics->GPUVRAM(&vramUsedMB) == ADLX_OK && vramUsedMB >= 0) {
          out.vramUsedGiB = static_cast<double>(vramUsedMB) / 1024.0;
          any = true;
        }

        releaseIface(metrics);
      }
    }

    releaseIface(gpu);
    releaseIface(perf);
    releaseIface(gpus);
    if (any) return out;
    return std::nullopt;
  }

  releaseIface(perf);
  releaseIface(gpus);
  return std::nullopt;
}

static std::string wideToUtf8(const wchar_t* s) {
  if (!s) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string out(len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), len, nullptr, nullptr);
  return out;
}

}  // namespace

AdlxAvailability adlxAvailability() {
  AdlxAvailability out;
  auto& rt = adlxRuntime();
  out.available = (adlxSystem() != nullptr);
  out.backend = out.available ? std::string("ADLX") : std::string();
  out.dll = rt.dllName;
  return out;
}

AdlxStatus adlxStatus() {
  auto& rt = adlxRuntime();
  if (!rt.mod) return AdlxStatus::MissingDll;
  if (!rt.ready) return AdlxStatus::Unusable;
  if (!adlxSystem()) return AdlxStatus::Unusable;
  return AdlxStatus::Available;
}

std::optional<AmdPcieLink> readAdlxPcieLinkForDxgi(const std::optional<AmdAdapterLuid>& adapterLuid) {
  if (!adapterLuid) return std::nullopt;
  LUID luid{};
  luid.LowPart = adapterLuid->lowPart;
  luid.HighPart = adapterLuid->highPart;
  return queryAdlxPcieLinkForAdapterLuid(luid);
}

std::optional<AdlxGpuTelemetry> readAdlxTelemetryForDxgi(const std::optional<AmdAdapterLuid>& adapterLuid) {
  if (!adapterLuid) return std::nullopt;
  LUID luid{};
  luid.LowPart = adapterLuid->lowPart;
  luid.HighPart = adapterLuid->highPart;
  return queryAdlxTelemetryForAdapterLuid(luid);
}

std::string amdPcieLinkNoteForDxgi(const std::optional<AmdAdapterLuid>& adapterLuid) {
  if (adapterLuid) {
    LUID luid{};
    luid.LowPart = adapterLuid->lowPart;
    luid.HighPart = adapterLuid->highPart;
    if (const auto note = queryAdlxPcieNoteForAdapterLuid(luid)) return *note;
  }

  const auto st = adlxStatus();
  if (st == AdlxStatus::MissingDll) return "ADLX missing";
  if (st != AdlxStatus::Available) return "ADLX unavailable";
  return std::string();
}

std::string pcieDiagnostics() {
  std::ostringstream oss;
  oss << "PCIe diagnostics (Windows)\n";

  // ADLX module state.
  {
    auto& rt = adlxRuntime();
    if (!rt.mod) {
      oss << "- ADLX dll: (not loaded)\n";
    } else {
      oss << "- ADLX dll: " << (rt.dllName.empty() ? std::string("(unknown)") : rt.dllName) << "\n";
      if (!rt.dllPath.empty()) oss << "- ADLX dll path: " << rt.dllPath << "\n";
      oss << "- ADLX ready: " << (rt.ready ? "yes" : "no") << "\n";
    }
  }

  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
    oss << "- DXGI: failed to create factory\n";
    return oss.str();
  }

  unsigned int hwIndex = 0;
  for (UINT i = 0;; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;

    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
        const std::string name = wideToUtf8(desc.Description);

        oss << "\n- GPU" << hwIndex << ": " << (name.empty() ? std::string("(unknown)") : name) << "\n";
        oss << "  vendor: 0x";
        oss.setf(std::ios::hex, std::ios::basefield);
        oss << static_cast<unsigned int>(desc.VendorId);
        oss.setf(std::ios::dec, std::ios::basefield);
        oss << "\n";
        oss << "  dxgi luid: " << static_cast<unsigned int>(desc.AdapterLuid.HighPart) << ":" << static_cast<unsigned int>(desc.AdapterLuid.LowPart) << "\n";

        if (desc.VendorId == 0x1002 || desc.VendorId == 0x1022) {
          if (const auto link = queryAdlxPcieLinkForAdapterLuid(desc.AdapterLuid)) {
            oss << "  adlx pcie: " << link->width << "x";
            if (link->generation) oss << "@Gen" << link->generation;
            oss << "\n";
          } else {
            const auto note = queryAdlxPcieNoteForAdapterLuid(desc.AdapterLuid);
            oss << "  adlx pcie: (unavailable)";
            if (note && !note->empty()) oss << " (" << *note << ")";
            oss << "\n";
          }
        } else {
          oss << "  adlx pcie: (n/a)\n";
        }

        ++hwIndex;
      }
    }

    adapter->Release();
  }

  factory->Release();
  return oss.str();
}

std::string adlxDiagnostics() {
  std::ostringstream oss;
  oss << "ADLX diagnostics (Windows)\n";

  HMODULE mod = LoadLibraryW(ADLX_DLL_NAMEW);
  if (!mod) {
    oss << "- dll: (not loaded)\n";
    oss << "- tried: " << ADLX_DLL_NAMEA << "\n";
    oss << "- LoadLibraryW error: " << static_cast<unsigned long>(GetLastError()) << "\n";
    return oss.str();
  }

  oss << "- dll: " << ADLX_DLL_NAMEA << "\n";
  oss << "- dll path: " << narrowUtf8FromWide(modulePathW(mod)) << "\n";

  const auto exports = listPeExportNames(mod);
  oss << "- exports: " << exports.size() << "\n";

  std::size_t shown = 0;
  for (const auto& e : exports) {
    if (containsCaseInsensitiveAscii(e, "adlx")) {
      oss << "  - " << e << "\n";
      if (++shown >= 80) {
        oss << "  - ... (truncated)\n";
        break;
      }
    }
  }
  if (shown == 0) oss << "- note: no exports containing 'ADLX' found\n";

  {
    oss << "\n- adlx init: ";

    auto init = reinterpret_cast<ADLXInitialize_Fn>(GetProcAddress(mod, ADLX_INIT_FUNCTION_NAME));
    auto term = reinterpret_cast<ADLXTerminate_Fn>(GetProcAddress(mod, ADLX_TERMINATE_FUNCTION_NAME));
    if (!init || !term) {
      oss << "(missing required exports for init/terminate)\n";
    } else {
      adlx::IADLXSystem* system = nullptr;
      const ADLX_RESULT initRes = init(ADLX_FULL_VERSION, &system);
      oss << "ret=" << static_cast<int>(initRes) << "\n";
      if (initRes == ADLX_OK && system) {
        adlx::IADLXGPUList* gpus = nullptr;
        const ADLX_RESULT listRes = system->GetGPUs(&gpus);
        oss << "- adlx get gpus: ret=" << static_cast<int>(listRes);
        if (listRes == ADLX_OK && gpus) {
          const adlx_uint count = gpus->Size();
          oss << " count=" << count << "\n";

          for (adlx_uint i = 0; i < count; ++i) {
            adlx::IADLXGPU* gpu = nullptr;
            const ADLX_RESULT atRes = gpus->At(i, &gpu);
            oss << "  - gpu[" << i << "]: At ret=" << static_cast<int>(atRes);
            if (atRes != ADLX_OK || !gpu) {
              oss << "\n";
              continue;
            }

            const char* name = nullptr;
            const ADLX_RESULT nameRes = gpu->Name(&name);

            ADLX_GPU_TYPE gpuType = GPUTYPE_UNDEFINED;
            const ADLX_RESULT typeRes = gpu->Type(&gpuType);

            adlx::IADLXGPU2* gpu2 = nullptr;
            const ADLX_RESULT qi2Res = gpu->QueryInterface(adlx::IADLXGPU2::IID(), reinterpret_cast<void**>(&gpu2));
            ADLX_LUID luid{};
            ADLX_RESULT luidRes = ADLX_FAIL;
            if (qi2Res == ADLX_OK && gpu2) {
              luidRes = gpu2->LUID(&luid);
              gpu2->Release();
              gpu2 = nullptr;
            }

            adlx::IADLXGPU1* gpu1 = nullptr;
            const ADLX_RESULT qi1Res = gpu->QueryInterface(adlx::IADLXGPU1::IID(), reinterpret_cast<void**>(&gpu1));
            ADLX_PCI_BUS_TYPE busType = UNDEFINED;
            adlx_uint laneWidth = 0;
            ADLX_RESULT busRes = ADLX_FAIL;
            ADLX_RESULT widthRes = ADLX_FAIL;
            if (qi1Res == ADLX_OK && gpu1) {
              busRes = gpu1->PCIBusType(&busType);
              widthRes = gpu1->PCIBusLaneWidth(&laneWidth);
              gpu1->Release();
              gpu1 = nullptr;
            }

            oss << " name=\"" << (name ? name : "") << "\"";
            oss << " Name ret=" << static_cast<int>(nameRes);
            oss << " Type ret=" << static_cast<int>(typeRes) << " val=" << static_cast<int>(gpuType);
            oss << " LUID ret=" << static_cast<int>(luidRes) << " val=" << static_cast<unsigned int>(luid.highPart) << ":" << static_cast<unsigned int>(luid.lowPart);
            oss << " PCIBusType ret=" << static_cast<int>(busRes) << " val=" << static_cast<int>(busType);
            oss << " LaneWidth ret=" << static_cast<int>(widthRes) << " val=" << static_cast<unsigned int>(laneWidth) << "\n";

            gpu->Release();
            gpu = nullptr;
          }

          gpus->Release();
          gpus = nullptr;
        } else {
          oss << "\n";
        }
      }
      (void)term();
    }
  }

  FreeLibrary(mod);
  return oss.str();
}

}  // namespace aiz

#endif

#else

namespace aiz {

AdlxAvailability adlxAvailability() {
  return {};
}

std::string adlxDiagnostics() {
  return std::string("ADLX diagnostics\n- status: unavailable (non-Windows build)\n");
}

std::string pcieDiagnostics() {
  return std::string("PCIe diagnostics\n- status: unavailable (non-Windows build)\n");
}

std::string amdPcieLinkNoteForDxgi(const std::optional<AmdAdapterLuid>&) {
  return std::string();
}

AdlxStatus adlxStatus() {
  return AdlxStatus::MissingDll;
}

std::optional<AmdPcieLink> readAdlxPcieLinkForDxgi(const std::optional<AmdAdapterLuid>&) {
  return std::nullopt;
}

std::optional<AdlxGpuTelemetry> readAdlxTelemetryForDxgi(const std::optional<AmdAdapterLuid>&) {
  return std::nullopt;
}

}  // namespace aiz

#endif
