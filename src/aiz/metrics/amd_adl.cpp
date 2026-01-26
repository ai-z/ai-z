#include <aiz/metrics/amd_adl.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dxgi1_2.h>

#ifndef INITGUID
#define INITGUID
#endif
#include <initguid.h>

#include <devguid.h>
#include <devpkey.h>
#include <ntddvdeo.h>
#include <pciprop.h>
#include <setupapi.h>

#if defined(AI_Z_HAS_ADLX_HEADERS)
#include <ADLX.h>
#include <ISystem.h>
#include <ISystem1.h>
#include <ISystem2.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aiz {
namespace {

// Minimal ADL SDK bindings (runtime loaded). Avoid including vendor headers.
using ADL_MAIN_MALLOC_CALLBACK = void* (__stdcall *)(int);
using ADL_MAIN_CONTROL_CREATE = int (__stdcall *)(ADL_MAIN_MALLOC_CALLBACK, int);
using ADL_MAIN_CONTROL_DESTROY = int (__stdcall *)();
using ADL_ADAPTER_NUMBEROFADAPTERS_GET = int (__stdcall *)(int*);
using ADL_ADAPTER_ADAPTERINFO_GET = int (__stdcall *)(void*, int);
using ADL_ADAPTER_PCIEBANDWIDTH_GET = int (__stdcall *)(int, void*);

// ADL2 variants (context-based). Prefer these when present.
using ADL2_MAIN_CONTROL_CREATE = int (__stdcall *)(void**, ADL_MAIN_MALLOC_CALLBACK, int);
using ADL2_MAIN_CONTROL_DESTROY = int (__stdcall *)(void*);
using ADL2_ADAPTER_NUMBEROFADAPTERS_GET = int (__stdcall *)(void*, int*);
using ADL2_ADAPTER_ADAPTERINFO_GET = int (__stdcall *)(void*, void*, int);
using ADL2_ADAPTER_PCIEBANDWIDTH_GET = int (__stdcall *)(void*, int, void*);

constexpr int ADL_OK = 0;
constexpr int ADL_MAX_PATH = 256;

struct ADLAdapterInfo {
  int iSize;
  int iAdapterIndex;
  char strUDID[ADL_MAX_PATH];
  int iBusNumber;
  int iDeviceNumber;
  int iFunctionNumber;
  int iVendorID;
  char strAdapterName[ADL_MAX_PATH];
  char strDisplayName[ADL_MAX_PATH];
  int iPresent;
  int iExist;
  char strDriverPath[ADL_MAX_PATH];
  char strDriverPathExt[ADL_MAX_PATH];
  char strPNPString[ADL_MAX_PATH];
  int iOSDisplayIndex;
};

struct ADLAdapterPCIEBandwidth {
  int iSize;
  int iSpeed;     // current PCIe generation
  int iLanes;     // current lane width
  int iMaxSpeed;  // max PCIe generation
  int iMaxLanes;  // max lane width
};

struct AdlApi {
  HMODULE dll = nullptr;

  std::string dllName;
  std::string dllPath;

  std::vector<std::string> loadAttempts;
  std::vector<unsigned long> loadAttemptErrors;

  // ADL2
  void* ctx = nullptr;
  ADL2_MAIN_CONTROL_CREATE mainCreate2 = nullptr;
  ADL2_MAIN_CONTROL_DESTROY mainDestroy2 = nullptr;
  ADL2_ADAPTER_NUMBEROFADAPTERS_GET numAdapters2 = nullptr;
  ADL2_ADAPTER_ADAPTERINFO_GET adapterInfo2 = nullptr;
  ADL2_ADAPTER_PCIEBANDWIDTH_GET pcieBandwidth2 = nullptr;

  bool usingAdl2 = false;
  bool hasAdl2Symbols = false;
  std::string missingAdl2Symbols;
  int create2Ret = 9999;

  // ADL1 fallback
  ADL_MAIN_CONTROL_CREATE mainCreate = nullptr;
  ADL_MAIN_CONTROL_DESTROY mainDestroy = nullptr;
  ADL_ADAPTER_NUMBEROFADAPTERS_GET numAdapters = nullptr;
  ADL_ADAPTER_ADAPTERINFO_GET adapterInfo = nullptr;
  ADL_ADAPTER_PCIEBANDWIDTH_GET pcieBandwidth = nullptr;
  bool hasAdl1Symbols = false;
  std::string missingAdl1Symbols;
  int createRet = 9999;

  bool initialized = false;
  bool ready = false;
};

void* __stdcall adlAlloc(int size) {
  return std::calloc(1, static_cast<std::size_t>(size));
}

static std::wstring getEnvWide(const wchar_t* name) {
  wchar_t buf[32768];
  DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  if (n == 0 || n >= (sizeof(buf) / sizeof(buf[0]))) return {};
  return std::wstring(buf, buf + n);
}

static std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  std::wstring out = a;
  if (!out.empty() && out.back() != L'\\') out += L"\\";
  out += b;
  return out;
}

static bool fileExistsW(const std::wstring& path) {
  if (path.empty()) return false;
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(path), ec);
}

static std::optional<std::wstring> findAdlInAmdCNextTree(const std::wstring& programFilesBase,
                                                        const wchar_t* fileName) {
  if (programFilesBase.empty() || !fileName || *fileName == L'\0') return std::nullopt;

  const std::filesystem::path root = std::filesystem::path(programFilesBase) / L"AMD" / L"CNext";
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) return std::nullopt;

  // Prefer the canonical path if present.
  const std::filesystem::path canonical = root / L"CNext" / fileName;
  if (std::filesystem::exists(canonical, ec)) return canonical.wstring();

  // Otherwise, do a bounded recursive search under ...\AMD\CNext.
  // This stays small in practice and avoids scanning all of Program Files.
  std::optional<std::wstring> firstHit;
  for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
       it != end;
       it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    const auto& p = it->path();
    if (!it->is_regular_file(ec)) continue;
    if (!p.has_filename()) continue;
    if (!p.filename().native().empty() && p.filename().native() == fileName) {
      const auto ws = p.wstring();
      // Best hit: contains "\\CNext\\CNext\\".
      if (ws.find(L"\\CNext\\CNext\\") != std::wstring::npos) return ws;
      if (!firstHit) firstHit = ws;
    }
  }
  return firstHit;
}

static std::vector<std::wstring> adlPreferredDllCandidates() {
  std::vector<std::wstring> out;
  out.reserve(16);

  // User override (full path). Useful for forcing a specific AMD installation.
  if (auto envOverride = getEnvWide(L"AI_Z_ADL_DLL"); !envOverride.empty()) {
    out.push_back(envOverride);
  }

  // Prefer the Radeon Software copy. The System32 copy can be a forwarder/stub.
  // Canonical location is usually:
  //   C:\Program Files\AMD\CNext\CNext\atiadlxx.dll
  // but some installs differ, so we also search within ...\AMD\CNext.
  std::vector<std::wstring> programFilesBases;
  for (const wchar_t* envName : {L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"}) {
    if (auto base = getEnvWide(envName); !base.empty()) programFilesBases.push_back(std::move(base));
  }
  auto addFound = [&](const std::optional<std::wstring>& p) {
    if (p && !p->empty()) out.push_back(*p);
  };
  for (const auto& base : programFilesBases) {
    addFound(findAdlInAmdCNextTree(base, L"atiadlxx.dll"));
  }
  for (const auto& base : programFilesBases) {
    addFound(findAdlInAmdCNextTree(base, L"atiadlxy.dll"));
  }

  // System directory fallback.
  wchar_t sysDirBuf[MAX_PATH];
  const UINT sysDirLen = GetSystemDirectoryW(sysDirBuf, MAX_PATH);
  const std::wstring sysDir = (sysDirLen > 0 && sysDirLen < MAX_PATH)
    ? std::wstring(sysDirBuf, sysDirBuf + sysDirLen)
    : std::wstring{};
  if (!sysDir.empty()) out.push_back(joinPath(sysDir, L"atiadlxx.dll"));

  // Last resort: rely on loader search order.
  out.push_back(L"atiadlxx.dll");
  out.push_back(L"atiadlxy.dll");

  // Dedupe while preserving order.
  std::vector<std::wstring> dedup;
  dedup.reserve(out.size());
  for (const auto& c : out) {
    if (c.empty()) continue;
    bool seen = false;
    for (const auto& d : dedup) {
      if (d == c) {
        seen = true;
        break;
      }
    }
    if (!seen) dedup.push_back(c);
  }
  return dedup;
}

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

static bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return false;
  const auto h = toLowerAscii(haystack);
  const auto n = toLowerAscii(needle);
  return h.find(n) != std::string::npos;
}

static AdlApi& adlApi() {
  static AdlApi api;
  if (api.initialized) return api;
  api.initialized = true;

  api.loadAttempts.clear();
  api.loadAttemptErrors.clear();

  // Prefer the full Radeon Software DLL before System32 to avoid stubs/forwarders.
  const auto candidates = adlPreferredDllCandidates();
  for (const auto& w : candidates) {
    if (w.empty()) continue;
    api.loadAttempts.push_back(narrowUtf8FromWide(w));
    HMODULE mod = LoadLibraryW(w.c_str());
    if (!mod) {
      api.loadAttemptErrors.push_back(static_cast<unsigned long>(GetLastError()));
      continue;
    }
    api.loadAttemptErrors.push_back(0ul);
    api.dll = mod;
    // Keep dllName as the basename for display purposes.
    api.dllName = narrowUtf8FromWide(std::filesystem::path(w).filename().wstring());
    api.dllPath = narrowUtf8FromWide(modulePathW(mod));
    break;
  }
  if (!api.dll) return api;

  // Try ADL2 first (more reliable on modern drivers).
  api.mainCreate2 = reinterpret_cast<ADL2_MAIN_CONTROL_CREATE>(GetProcAddress(api.dll, "ADL2_Main_Control_Create"));
  api.mainDestroy2 = reinterpret_cast<ADL2_MAIN_CONTROL_DESTROY>(GetProcAddress(api.dll, "ADL2_Main_Control_Destroy"));
  api.numAdapters2 = reinterpret_cast<ADL2_ADAPTER_NUMBEROFADAPTERS_GET>(GetProcAddress(api.dll, "ADL2_Adapter_NumberOfAdapters_Get"));
  api.adapterInfo2 = reinterpret_cast<ADL2_ADAPTER_ADAPTERINFO_GET>(GetProcAddress(api.dll, "ADL2_Adapter_AdapterInfo_Get"));
  api.pcieBandwidth2 = reinterpret_cast<ADL2_ADAPTER_PCIEBANDWIDTH_GET>(GetProcAddress(api.dll, "ADL2_Adapter_PCIeBandwidth_Get"));

  api.hasAdl2Symbols = (api.mainCreate2 && api.mainDestroy2 && api.numAdapters2 && api.adapterInfo2 && api.pcieBandwidth2);
  if (!api.hasAdl2Symbols) {
    std::ostringstream miss;
    bool first = true;
    auto add = [&](const char* s) {
      if (!first) miss << ", ";
      miss << s;
      first = false;
    };
    if (!api.mainCreate2) add("ADL2_Main_Control_Create");
    if (!api.mainDestroy2) add("ADL2_Main_Control_Destroy");
    if (!api.numAdapters2) add("ADL2_Adapter_NumberOfAdapters_Get");
    if (!api.adapterInfo2) add("ADL2_Adapter_AdapterInfo_Get");
    if (!api.pcieBandwidth2) add("ADL2_Adapter_PCIeBandwidth_Get");
    api.missingAdl2Symbols = miss.str();
  }

  if (api.hasAdl2Symbols) {
    // Some drivers are picky about the enumConnectedAdapters flag; try both.
    api.create2Ret = api.mainCreate2(&api.ctx, adlAlloc, 1);
    if (api.create2Ret != ADL_OK || !api.ctx) {
      api.ctx = nullptr;
      api.create2Ret = api.mainCreate2(&api.ctx, adlAlloc, 0);
    }

    if (api.create2Ret == ADL_OK && api.ctx) {
      api.usingAdl2 = true;
      api.ready = true;
      return api;
    }
    api.ctx = nullptr;
  }

  // Fall back to ADL1.
  api.mainCreate = reinterpret_cast<ADL_MAIN_CONTROL_CREATE>(GetProcAddress(api.dll, "ADL_Main_Control_Create"));
  api.mainDestroy = reinterpret_cast<ADL_MAIN_CONTROL_DESTROY>(GetProcAddress(api.dll, "ADL_Main_Control_Destroy"));
  api.numAdapters = reinterpret_cast<ADL_ADAPTER_NUMBEROFADAPTERS_GET>(GetProcAddress(api.dll, "ADL_Adapter_NumberOfAdapters_Get"));
  api.adapterInfo = reinterpret_cast<ADL_ADAPTER_ADAPTERINFO_GET>(GetProcAddress(api.dll, "ADL_Adapter_AdapterInfo_Get"));
  api.pcieBandwidth = reinterpret_cast<ADL_ADAPTER_PCIEBANDWIDTH_GET>(GetProcAddress(api.dll, "ADL_Adapter_PCIeBandwidth_Get"));

  api.hasAdl1Symbols = (api.mainCreate && api.mainDestroy && api.numAdapters && api.adapterInfo && api.pcieBandwidth);
  if (!api.hasAdl1Symbols) {
    std::ostringstream miss;
    bool first = true;
    auto add = [&](const char* s) {
      if (!first) miss << ", ";
      miss << s;
      first = false;
    };
    if (!api.mainCreate) add("ADL_Main_Control_Create");
    if (!api.mainDestroy) add("ADL_Main_Control_Destroy");
    if (!api.numAdapters) add("ADL_Adapter_NumberOfAdapters_Get");
    if (!api.adapterInfo) add("ADL_Adapter_AdapterInfo_Get");
    if (!api.pcieBandwidth) add("ADL_Adapter_PCIeBandwidth_Get");
    api.missingAdl1Symbols = miss.str();
  }

  if (!api.hasAdl1Symbols) {
    return api;
  }

  api.createRet = api.mainCreate(adlAlloc, 1);
  if (api.createRet != ADL_OK) {
    api.createRet = api.mainCreate(adlAlloc, 0);
  }

  if (api.createRet != ADL_OK) {
    return api;
  }

  api.ready = true;
  return api;
}

}  // namespace

AdlAvailability adlAvailability() {
  auto& api = adlApi();
  AdlAvailability out;
  out.available = api.ready;
  out.dll = api.dllName;
  if (api.ready) out.backend = api.usingAdl2 ? "ADL2" : "ADL";
  return out;
}

namespace {
static std::optional<std::vector<ADLAdapterInfo>> listAdapters();
static std::optional<AdlPcieLink> queryLinkForAdapterIndex(int adapterIndex);
}  // namespace

std::string adlDiagnostics() {
  auto& api = adlApi();
  std::ostringstream oss;
  oss << "ADL diagnostics\n";

  if (!api.loadAttempts.empty()) {
    oss << "- attempted dll paths:\n";
    for (std::size_t i = 0; i < api.loadAttempts.size(); ++i) {
      oss << "  - " << api.loadAttempts[i];
      const unsigned long err = (i < api.loadAttemptErrors.size()) ? api.loadAttemptErrors[i] : 0ul;
      if (err != 0ul) oss << " (LoadLibraryW error " << err << ")";
      oss << "\n";
    }
  }

  if (!api.dll) {
    oss << "- dll: (not loaded)\n";
    oss << "- status: missing\n";
    return oss.str();
  }

  oss << "- dll: " << (api.dllName.empty() ? std::string("(unknown)") : api.dllName) << "\n";
  if (!api.dllPath.empty()) oss << "- dll path: " << api.dllPath << "\n";
  oss << "- has ADL2 symbols: " << (api.hasAdl2Symbols ? "yes" : "no") << "\n";
  if (!api.hasAdl2Symbols && !api.missingAdl2Symbols.empty()) oss << "- missing ADL2: " << api.missingAdl2Symbols << "\n";
  if (api.hasAdl2Symbols) oss << "- ADL2 create ret: " << api.create2Ret << "\n";
  oss << "- has ADL1 symbols: " << (api.hasAdl1Symbols ? "yes" : "no") << "\n";
  if (!api.hasAdl1Symbols && !api.missingAdl1Symbols.empty()) oss << "- missing ADL1: " << api.missingAdl1Symbols << "\n";
  if (api.hasAdl1Symbols) oss << "- ADL1 create ret: " << api.createRet << "\n";
  oss << "- backend selected: " << (api.usingAdl2 ? "ADL2" : "ADL") << "\n";
  oss << "- ready: " << (api.ready ? "yes" : "no") << "\n";

  if (!api.ready) {
    oss << "- status: unusable\n";
    return oss.str();
  }

  const auto adaptersOpt = listAdapters();
  if (!adaptersOpt) {
    oss << "- adapters: (failed to enumerate)\n";
    return oss.str();
  }

  const auto& adapters = *adaptersOpt;
  oss << "- adapters: " << adapters.size() << "\n";
  for (std::size_t i = 0; i < adapters.size(); ++i) {
    const auto& a = adapters[i];
    oss << "  - [" << i << "] idx=" << a.iAdapterIndex
        << " vendor=0x";
    oss.setf(std::ios::hex, std::ios::basefield);
    oss << static_cast<unsigned int>(a.iVendorID);
    oss.setf(std::ios::dec, std::ios::basefield);
    oss << " bdf=" << a.iBusNumber << ":" << a.iDeviceNumber << "." << a.iFunctionNumber;
    oss << " name=\"" << a.strAdapterName << "\"";
    if (a.strDisplayName[0] != '\0') oss << " display=\"" << a.strDisplayName << "\"";

    if (const auto link = queryLinkForAdapterIndex(a.iAdapterIndex)) {
      oss << " pcie=" << link->width << "x";
      if (link->generation) oss << "@Gen" << link->generation;
      if (link->maxWidth && link->maxGeneration) {
        oss << " (max " << link->maxWidth << "x@Gen" << link->maxGeneration << ")";
      }
    } else {
      oss << " pcie=(unavailable)";
    }
    oss << "\n";
  }

  return oss.str();
}

AdlStatus adlStatus() {
  auto& api = adlApi();
  if (!api.dll) return AdlStatus::MissingDll;
  if (!api.ready) return AdlStatus::Unusable;
  return AdlStatus::Available;
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

static std::optional<AdlPcieLink> queryWindowsPcieLinkForPciLocation(const AdlPciLocation& pciLoc);
static std::optional<AdlPcieLink> queryWindowsPcieLinkForAdapterLuid(const LUID& luid);

#if defined(AI_Z_HAS_ADLX_HEADERS)
static unsigned int pcieGenFromAdlxPciBusType(ADLX_PCI_BUS_TYPE type) {
  switch (type) {
    case PCIE_2_0: return 2;
    case PCIE_3_0: return 3;
    case PCIE_4_0: return 4;
    // PCIE and others are treated as "unknown".
    default: return 0;
  }
}

static std::optional<AdlPcieLink> queryAdlxPcieLinkForAdapterLuid(const LUID& luid) {
  const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32ull) |
                            static_cast<std::uint64_t>(luid.LowPart);

  // Cache results because ADLX initialization is relatively expensive and PCIe
  // lane width/gen typically doesn't change during a session.
  static std::mutex cacheMutex;
  static std::unordered_map<std::uint64_t, AdlPcieLink> cacheOk;
  static std::unordered_set<std::uint64_t> cacheFail;
  {
    const std::lock_guard<std::mutex> lock(cacheMutex);
    if (const auto it = cacheOk.find(key); it != cacheOk.end()) return it->second;
    if (cacheFail.find(key) != cacheFail.end()) return std::nullopt;
  }

  // Runtime-load ADLX (amdadlx64.dll) and match GPU by adapter LUID.
  HMODULE mod = LoadLibraryW(L"amdadlx64.dll");
  if (!mod) {
    const std::lock_guard<std::mutex> lock(cacheMutex);
    cacheFail.insert(key);
    return std::nullopt;
  }

  auto cleanupModule = [&]() { FreeLibrary(mod); };

  auto init = reinterpret_cast<ADLXInitialize_Fn>(GetProcAddress(mod, ADLX_INIT_FUNCTION_NAME));
  auto term = reinterpret_cast<ADLXTerminate_Fn>(GetProcAddress(mod, ADLX_TERMINATE_FUNCTION_NAME));
  if (!init || !term) {
    cleanupModule();
    const std::lock_guard<std::mutex> lock(cacheMutex);
    cacheFail.insert(key);
    return std::nullopt;
  }

  adlx::IADLXSystem* system = nullptr;
  if (init(ADLX_FULL_VERSION, &system) != ADLX_OK || !system) {
    (void)term();
    cleanupModule();
    const std::lock_guard<std::mutex> lock(cacheMutex);
    cacheFail.insert(key);
    return std::nullopt;
  }

  adlx::IADLXGPUList* gpus = nullptr;
  if (system->GetGPUs(&gpus) != ADLX_OK || !gpus) {
    (void)term();
    cleanupModule();
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

    AdlPcieLink out;
    out.width = static_cast<unsigned int>(laneWidth);
    out.generation = hasBusType ? pcieGenFromAdlxPciBusType(busType) : 0u;
    // ADLX doesn't currently expose max width/speed via these methods.
    out.maxWidth = 0;
    out.maxGeneration = 0;

    {
      const std::lock_guard<std::mutex> lock(cacheMutex);
      cacheOk[key] = out;
    }

    releaseIface(gpus);
    (void)term();
    cleanupModule();
    return out;
  }

  releaseIface(gpus);
  (void)term();
  cleanupModule();
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

  HMODULE mod = LoadLibraryW(L"amdadlx64.dll");
  if (!mod) {
    const std::lock_guard<std::mutex> lock(noteMutex);
    noteCache[key] = std::string();
    return std::nullopt;
  }

  auto init = reinterpret_cast<ADLXInitialize_Fn>(GetProcAddress(mod, ADLX_INIT_FUNCTION_NAME));
  auto term = reinterpret_cast<ADLXTerminate_Fn>(GetProcAddress(mod, ADLX_TERMINATE_FUNCTION_NAME));
  if (!init || !term) {
    FreeLibrary(mod);
    const std::lock_guard<std::mutex> lock(noteMutex);
    noteCache[key] = std::string();
    return std::nullopt;
  }

  adlx::IADLXSystem* system = nullptr;
  if (init(ADLX_FULL_VERSION, &system) != ADLX_OK || !system) {
    (void)term();
    FreeLibrary(mod);
    const std::lock_guard<std::mutex> lock(noteMutex);
    noteCache[key] = std::string();
    return std::nullopt;
  }

  adlx::IADLXGPUList* gpus = nullptr;
  if (system->GetGPUs(&gpus) != ADLX_OK || !gpus) {
    (void)term();
    FreeLibrary(mod);
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
  (void)term();
  FreeLibrary(mod);

  {
    const std::lock_guard<std::mutex> lock(noteMutex);
    noteCache[key] = result.value_or(std::string());
  }
  return result;
}
#endif
}  // namespace

std::string amdPcieLinkNoteForDxgi(const std::optional<AdlAdapterLuid>& adapterLuid) {
#if defined(_WIN32)
#if defined(AI_Z_HAS_ADLX_HEADERS)
  if (adapterLuid) {
    LUID luid{};
    luid.LowPart = adapterLuid->lowPart;
    luid.HighPart = adapterLuid->highPart;
    if (const auto note = queryAdlxPcieNoteForAdapterLuid(luid)) return *note;
  }
#endif

  const auto st = adlStatus();
  if (st == AdlStatus::MissingDll) return "ADL missing";
  if (st != AdlStatus::Available) return "ADL unavailable";
  return std::string();
#else
  (void)adapterLuid;
  return std::string();
#endif
}

std::string pcieDiagnostics() {
  std::ostringstream oss;
  oss << "PCIe diagnostics (Windows)\n";

  // ADL module state (useful to confirm whether we're loading the Radeon Software DLL).
  {
    auto& api = adlApi();
    if (api.dllPath.empty()) {
      oss << "- ADL dll: (not loaded)\n";
    } else {
      oss << "- ADL dll path: " << api.dllPath << "\n";
      oss << "- ADL ready: " << (api.ready ? "yes" : "no") << "\n";
      if (!api.ready) {
        if (!api.hasAdl2Symbols && !api.missingAdl2Symbols.empty()) oss << "- missing ADL2: " << api.missingAdl2Symbols << "\n";
        if (!api.hasAdl1Symbols && !api.missingAdl1Symbols.empty()) oss << "- missing ADL1: " << api.missingAdl1Symbols << "\n";
      }
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

        const std::uint32_t bus = static_cast<std::uint32_t>((desc.AdapterLuid.HighPart >> 16) & 0xFF);
        const std::uint32_t dev = static_cast<std::uint32_t>((desc.AdapterLuid.HighPart >> 8) & 0xFF);
        const std::uint32_t fun = static_cast<std::uint32_t>(desc.AdapterLuid.HighPart & 0xFF);

        oss << "\n- GPU" << hwIndex << ": " << (name.empty() ? std::string("(unknown)") : name) << "\n";
        oss << "  vendor: 0x";
        oss.setf(std::ios::hex, std::ios::basefield);
        oss << static_cast<unsigned int>(desc.VendorId);
        oss.setf(std::ios::dec, std::ios::basefield);
        oss << "\n";
        oss << "  dxgi luid: " << static_cast<unsigned int>(desc.AdapterLuid.HighPart) << ":" << static_cast<unsigned int>(desc.AdapterLuid.LowPart) << "\n";
        oss << "  bdf(best-effort): " << bus << ":" << dev << "." << fun << "\n";

        // Direct Windows PCIe properties (best signal when present).
        if (const auto link = queryWindowsPcieLinkForAdapterLuid(desc.AdapterLuid)) {
          oss << "  windows pcie: " << link->width << "x";
          if (link->generation) oss << "@Gen" << link->generation;
          if (link->maxWidth && link->maxGeneration) {
            oss << " (max " << link->maxWidth << "x@Gen" << link->maxGeneration << ")";
          }
          oss << "\n";
        } else if (const auto link = queryWindowsPcieLinkForPciLocation(AdlPciLocation{bus, dev, fun})) {
          oss << "  windows pcie (bdf match): " << link->width << "x";
          if (link->generation) oss << "@Gen" << link->generation;
          oss << "\n";
        } else {
          oss << "  windows pcie: (unavailable)\n";
        }

        // What ai-z will show (ADL if possible, otherwise Windows fallback).
        AdlAdapterLuid luid{static_cast<std::uint32_t>(desc.AdapterLuid.LowPart), static_cast<std::int32_t>(desc.AdapterLuid.HighPart)};
        if (const auto best = readAdlPcieLinkForDxgi(name, luid, std::nullopt, AdlPciLocation{bus, dev, fun})) {
          oss << "  ai-z pcie: " << best->width << "x";
          if (best->generation) oss << "@Gen" << best->generation;
          oss << "\n";
        } else {
          oss << "  ai-z pcie: (unavailable)\n";
        }

        ++hwIndex;
      }
    }

    adapter->Release();
  }

  factory->Release();
  return oss.str();
}

namespace {
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

static bool containsCaseInsensitiveAscii(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return false;
  const auto h = toLowerAscii(haystack);
  const auto n = toLowerAscii(needle);
  return h.find(n) != std::string::npos;
}
}  // namespace

std::string adlxDiagnostics() {
  std::ostringstream oss;
  oss << "ADLX diagnostics (Windows)\n";

  HMODULE mod = LoadLibraryW(L"amdadlx64.dll");
  if (!mod) {
    oss << "- dll: (not loaded)\n";
    oss << "- tried: amdadlx64.dll\n";
    oss << "- LoadLibraryW error: " << static_cast<unsigned long>(GetLastError()) << "\n";
    return oss.str();
  }

  oss << "- dll: amdadlx64.dll\n";
  oss << "- dll path: " << narrowUtf8FromWide(modulePathW(mod)) << "\n";

  const auto exports = listPeExportNames(mod);
  oss << "- exports: " << exports.size() << "\n";

  // Print only ADLX-related exports to keep output readable.
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

#if defined(AI_Z_HAS_ADLX_HEADERS)
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
#endif

  FreeLibrary(mod);
  return oss.str();
}

namespace {

static std::optional<std::vector<ADLAdapterInfo>> listAdapters() {
  auto& api = adlApi();
  if (!api.ready) return std::nullopt;

  int count = 0;
  if (api.usingAdl2) {
    if (api.numAdapters2(api.ctx, &count) != ADL_OK || count <= 0) return std::nullopt;
  } else {
    if (api.numAdapters(&count) != ADL_OK || count <= 0) return std::nullopt;
  }

  std::vector<ADLAdapterInfo> infos(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    infos[static_cast<std::size_t>(i)].iSize = sizeof(ADLAdapterInfo);
  }

  if (api.usingAdl2) {
    if (api.adapterInfo2(api.ctx, infos.data(), static_cast<int>(sizeof(ADLAdapterInfo) * infos.size())) != ADL_OK) return std::nullopt;
  } else {
    if (api.adapterInfo(infos.data(), static_cast<int>(sizeof(ADLAdapterInfo) * infos.size())) != ADL_OK) return std::nullopt;
  }
  return infos;
}

static std::optional<AdlPcieLink> queryLinkForAdapterIndex(int adapterIndex) {
  auto& api = adlApi();
  if (!api.ready) return std::nullopt;

  ADLAdapterPCIEBandwidth bw{};
  bw.iSize = sizeof(ADLAdapterPCIEBandwidth);
  if (api.usingAdl2) {
    if (api.pcieBandwidth2(api.ctx, adapterIndex, &bw) != ADL_OK) return std::nullopt;
  } else {
    if (api.pcieBandwidth(adapterIndex, &bw) != ADL_OK) return std::nullopt;
  }

  const unsigned int curGen = bw.iSpeed > 0 ? static_cast<unsigned int>(bw.iSpeed) : 0u;
  const unsigned int curWidth = bw.iLanes > 0 ? static_cast<unsigned int>(bw.iLanes) : 0u;
  const unsigned int maxGen = bw.iMaxSpeed > 0 ? static_cast<unsigned int>(bw.iMaxSpeed) : 0u;
  const unsigned int maxWidth = bw.iMaxLanes > 0 ? static_cast<unsigned int>(bw.iMaxLanes) : 0u;

  const unsigned int gen = curGen ? curGen : maxGen;
  const unsigned int width = curWidth ? curWidth : maxWidth;
  if (gen == 0 || width == 0) return std::nullopt;

  AdlPcieLink out;
  out.generation = gen;
  out.width = width;
  out.maxGeneration = maxGen;
  out.maxWidth = maxWidth;
  return out;
}

static bool isAmdVendor(int vendorId) {
  return vendorId == 0x1002 || vendorId == 0x1022;
}

static unsigned int pcieGenFromWindowsLinkSpeed(std::uint32_t speed) {
  // Windows' DEVPKEY_PciDevice_CurrentLinkSpeed is typically either:
  // - a small integer encoding the PCIe generation (1..6), or
  // - the link speed in MT/s (2500, 5000, 8000, 16000, 32000, 64000).
  if (speed >= 1 && speed <= 8) return static_cast<unsigned int>(speed);
  switch (speed) {
    case 2500: return 1;
    case 5000: return 2;
    case 8000: return 3;
    case 16000: return 4;
    case 32000: return 5;
    case 64000: return 6;
    default: return 0;
  }
}

static std::optional<AdlPcieLink> queryWindowsPcieLinkForPciLocation(const AdlPciLocation& pciLoc) {
  HDEVINFO devInfoSet = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
  if (devInfoSet == INVALID_HANDLE_VALUE) return std::nullopt;

  auto cleanup = [&]() { SetupDiDestroyDeviceInfoList(devInfoSet); };

  SP_DEVINFO_DATA devInfo{};
  devInfo.cbSize = sizeof(SP_DEVINFO_DATA);

  for (DWORD index = 0; SetupDiEnumDeviceInfo(devInfoSet, index, &devInfo); ++index) {
    DEVPROPTYPE propType = 0;
    DWORD required = 0;

    DWORD bus = 0;
    if (!SetupDiGetDevicePropertyW(devInfoSet, &devInfo, &DEVPKEY_Device_BusNumber, &propType,
                                   reinterpret_cast<PBYTE>(&bus), sizeof(bus), &required, 0)) {
      continue;
    }
    if (propType != DEVPROP_TYPE_UINT32) continue;

    DWORD address = 0;
    if (!SetupDiGetDevicePropertyW(devInfoSet, &devInfo, &DEVPKEY_Device_Address, &propType,
                                   reinterpret_cast<PBYTE>(&address), sizeof(address), &required, 0)) {
      continue;
    }
    if (propType != DEVPROP_TYPE_UINT32) continue;

    // For PCI devices, Address is typically encoded as (device << 16) | function.
    const DWORD device = (address >> 16) & 0xFFFF;
    const DWORD function = address & 0xFFFF;

    if (bus != pciLoc.bus || device != pciLoc.device || function != pciLoc.function) continue;

    DWORD curWidth = 0;
    DWORD curSpeed = 0;
    DWORD maxWidth = 0;
    DWORD maxSpeed = 0;

    const bool hasCurWidth = !!SetupDiGetDevicePropertyW(devInfoSet, &devInfo, &DEVPKEY_PciDevice_CurrentLinkWidth, &propType,
                                                        reinterpret_cast<PBYTE>(&curWidth), sizeof(curWidth), &required, 0);
    const bool hasCurSpeed = !!SetupDiGetDevicePropertyW(devInfoSet, &devInfo, &DEVPKEY_PciDevice_CurrentLinkSpeed, &propType,
                                                        reinterpret_cast<PBYTE>(&curSpeed), sizeof(curSpeed), &required, 0);
    (void)SetupDiGetDevicePropertyW(devInfoSet, &devInfo, &DEVPKEY_PciDevice_MaxLinkWidth, &propType,
                                   reinterpret_cast<PBYTE>(&maxWidth), sizeof(maxWidth), &required, 0);
    (void)SetupDiGetDevicePropertyW(devInfoSet, &devInfo, &DEVPKEY_PciDevice_MaxLinkSpeed, &propType,
                                   reinterpret_cast<PBYTE>(&maxSpeed), sizeof(maxSpeed), &required, 0);

    const unsigned int width = hasCurWidth ? static_cast<unsigned int>(curWidth) : 0u;
    const unsigned int gen = hasCurSpeed ? pcieGenFromWindowsLinkSpeed(static_cast<std::uint32_t>(curSpeed)) : 0u;
    const unsigned int maxGen = pcieGenFromWindowsLinkSpeed(static_cast<std::uint32_t>(maxSpeed));
    const unsigned int maxW = static_cast<unsigned int>(maxWidth);

    if (width == 0 || gen == 0) {
      cleanup();
      return std::nullopt;
    }

    AdlPcieLink out;
    out.width = width;
    out.generation = gen;
    out.maxWidth = maxW;
    out.maxGeneration = maxGen;
    cleanup();
    return out;
  }

  cleanup();
  return std::nullopt;
}

static std::optional<AdlPcieLink> queryWindowsPcieLinkForAdapterLuid(const LUID& luid) {
  HDEVINFO devInfoSet = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
  if (devInfoSet == INVALID_HANDLE_VALUE) return std::nullopt;

  auto cleanup = [&]() { SetupDiDestroyDeviceInfoList(devInfoSet); };

  SP_DEVINFO_DATA devInfo{};
  devInfo.cbSize = sizeof(SP_DEVINFO_DATA);

  for (DWORD index = 0; SetupDiEnumDeviceInfo(devInfoSet, index, &devInfo); ++index) {
    DEVPROPTYPE propType = 0;
    DWORD required = 0;

    // DEVPKEY_Device_AdapterLuid is DEVPROP_TYPE_BINARY (8 bytes).
    LUID devLuid{};
    if (!SetupDiGetDevicePropertyW(devInfoSet, &devInfo, &DEVPKEY_Device_AdapterLuid, &propType,
                                   reinterpret_cast<PBYTE>(&devLuid), sizeof(devLuid), &required, 0)) {
      continue;
    }
    if (propType != DEVPROP_TYPE_BINARY || required != sizeof(devLuid)) continue;
    if (devLuid.HighPart != luid.HighPart || devLuid.LowPart != luid.LowPart) continue;

    DWORD curWidth = 0;
    DWORD curSpeed = 0;
    DWORD maxWidth = 0;
    DWORD maxSpeed = 0;

    if (!SetupDiGetDevicePropertyW(devInfoSet, &devInfo, &DEVPKEY_PciDevice_CurrentLinkWidth, &propType,
                                   reinterpret_cast<PBYTE>(&curWidth), sizeof(curWidth), &required, 0)) {
      cleanup();
      return std::nullopt;
    }
    if (!SetupDiGetDevicePropertyW(devInfoSet, &devInfo, &DEVPKEY_PciDevice_CurrentLinkSpeed, &propType,
                                   reinterpret_cast<PBYTE>(&curSpeed), sizeof(curSpeed), &required, 0)) {
      cleanup();
      return std::nullopt;
    }
    (void)SetupDiGetDevicePropertyW(devInfoSet, &devInfo, &DEVPKEY_PciDevice_MaxLinkWidth, &propType,
                                   reinterpret_cast<PBYTE>(&maxWidth), sizeof(maxWidth), &required, 0);
    (void)SetupDiGetDevicePropertyW(devInfoSet, &devInfo, &DEVPKEY_PciDevice_MaxLinkSpeed, &propType,
                                   reinterpret_cast<PBYTE>(&maxSpeed), sizeof(maxSpeed), &required, 0);

    const unsigned int width = static_cast<unsigned int>(curWidth);
    const unsigned int gen = pcieGenFromWindowsLinkSpeed(static_cast<std::uint32_t>(curSpeed));
    const unsigned int maxGen = pcieGenFromWindowsLinkSpeed(static_cast<std::uint32_t>(maxSpeed));
    const unsigned int maxW = static_cast<unsigned int>(maxWidth);

    if (width == 0 || gen == 0) {
      cleanup();
      return std::nullopt;
    }

    AdlPcieLink out;
    out.width = width;
    out.generation = gen;
    out.maxWidth = maxW;
    out.maxGeneration = maxGen;
    cleanup();
    return out;
  }

  cleanup();
  return std::nullopt;
}

}  // namespace

std::optional<AdlPcieLink> readAdlPcieLinkForDxgi(const std::string& dxgiName,
                                                 const std::optional<AdlAdapterLuid>& adapterLuid,
                                                 const std::optional<unsigned int>& amdOrdinal,
                                                 const std::optional<AdlPciLocation>& pciLoc) {
  // If ADL doesn't provide the PCIe bandwidth APIs on this driver, fall back to
  // Windows' PCI device properties.
  {
    auto& api = adlApi();
    if (!api.ready) {
      // If available, prefer ADLX (official AMD API) before SetupAPI.
#if defined(AI_Z_HAS_ADLX_HEADERS)
      if (adapterLuid) {
        LUID luid{};
        luid.LowPart = adapterLuid->lowPart;
        luid.HighPart = adapterLuid->highPart;
        if (const auto link = queryAdlxPcieLinkForAdapterLuid(luid)) return link;
      }
#endif
      // First try matching by adapter LUID (most reliable), then fall back to BDF.
      if (adapterLuid) {
        LUID luid{};
        luid.LowPart = adapterLuid->lowPart;
        luid.HighPart = adapterLuid->highPart;
        if (const auto link = queryWindowsPcieLinkForAdapterLuid(luid)) return link;
      }
      if (pciLoc) return queryWindowsPcieLinkForPciLocation(*pciLoc);
      return std::nullopt;
    }
  }

  const auto adaptersOpt = listAdapters();
  if (!adaptersOpt) return std::nullopt;
  const auto& adapters = *adaptersOpt;

  // 1) Best match: PCI location.
  if (pciLoc) {
    for (const auto& info : adapters) {
      if (!isAmdVendor(info.iVendorID)) continue;
      if (info.iBusNumber == static_cast<int>(pciLoc->bus) &&
          info.iDeviceNumber == static_cast<int>(pciLoc->device) &&
          info.iFunctionNumber == static_cast<int>(pciLoc->function)) {
        if (const auto link = queryLinkForAdapterIndex(info.iAdapterIndex)) return link;
      }
    }
  }

  // 2) Fallback: match DXGI name against ADL adapter/display names.
  if (!dxgiName.empty()) {
    for (const auto& info : adapters) {
      if (!isAmdVendor(info.iVendorID)) continue;
      const std::string name = info.strAdapterName;
      const std::string display = info.strDisplayName;
      if (containsCaseInsensitive(name, dxgiName) || containsCaseInsensitive(display, dxgiName)) {
        if (const auto link = queryLinkForAdapterIndex(info.iAdapterIndex)) return link;
      }
    }
  }

  // 3) Last resort: AMD ordinal (DXGI AMD adapter order).
  if (amdOrdinal) {
    unsigned int seen = 0;
    for (const auto& info : adapters) {
      if (!isAmdVendor(info.iVendorID)) continue;
      if (seen == *amdOrdinal) {
        return queryLinkForAdapterIndex(info.iAdapterIndex);
      }
      ++seen;
    }
  }

  // As a last fallback, try Windows' PCI properties.
  // Prefer ADLX (official AMD API) if available.
#if defined(AI_Z_HAS_ADLX_HEADERS)
  if (adapterLuid) {
    LUID luid{};
    luid.LowPart = adapterLuid->lowPart;
    luid.HighPart = adapterLuid->highPart;
    if (const auto link = queryAdlxPcieLinkForAdapterLuid(luid)) return link;
  }
#endif
  if (adapterLuid) {
    LUID luid{};
    luid.LowPart = adapterLuid->lowPart;
    luid.HighPart = adapterLuid->highPart;
    if (const auto link = queryWindowsPcieLinkForAdapterLuid(luid)) return link;
  }
  if (pciLoc) return queryWindowsPcieLinkForPciLocation(*pciLoc);
  return std::nullopt;
}

}  // namespace aiz

#else

namespace aiz {

AdlAvailability adlAvailability() {
  return {};
}

std::string adlDiagnostics() {
  return std::string("ADL diagnostics\n- status: unavailable (non-Windows build)\n");
}

AdlStatus adlStatus() {
  return AdlStatus::MissingDll;
}

std::optional<AdlPcieLink> readAdlPcieLinkForDxgi(const std::string&,
                                                 const std::optional<AdlAdapterLuid>&,
                                                 const std::optional<unsigned int>&,
                                                 const std::optional<AdlPciLocation>&) {
  return std::nullopt;
}

}  // namespace aiz

#endif
